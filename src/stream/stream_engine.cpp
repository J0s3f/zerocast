/*
 * SPDX-License-Identifier: MIT
 */

#include "stream_engine.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace stream {

namespace {

long mono_sec()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long>(ts.tv_sec);
}

long wall_sec()
{
    return static_cast<long>(std::time(nullptr));
}

constexpr long kStopGraceSec = 4;
// After SIGKILL, how long to keep waiting before giving up on a process that will not go away.
constexpr long kStopForceSec = 3;
// A stream that has produced no frames this long is stuck (a camera or microphone that opens but never
// delivers, e.g. a wedged USB device). Camera + mic normally start in under 15 s.
constexpr long kStartTimeoutSec = 40;

// Runs a shell command and returns its trimmed stdout ("" on failure).
std::string capture(const std::string& cmd)
{
    FILE* f = ::popen(cmd.c_str(), "r");
    if (!f)
        return "";
    std::string out;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), f))
        out += buf;
    ::pclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

void replace_all(std::string* text, const std::string& from, const std::string& to)
{
    for (size_t pos = text->find(from); pos != std::string::npos; pos = text->find(from, pos + to.size()))
        text->replace(pos, from.size(), to);
}

// Start time of a process in clock ticks since boot (field 22 of /proc/<pid>/stat). Together
// with the pid it identifies a process, so a recycled pid is not mistaken for our supervisor.
bool read_start_ticks(pid_t pid, unsigned long long* ticks, char* state = nullptr)
{
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!f || !std::getline(f, line))
        return false;
    const size_t close = line.rfind(')');  // the command name may contain spaces and parentheses
    if (close == std::string::npos)
        return false;
    std::istringstream rest(line.substr(close + 1));
    std::string field;
    for (int i = 3; i <= 22; ++i) {  // state is field 3, starttime is field 22
        if (!(rest >> field))
            return false;
        if (i == 3 && state)
            *state = field.empty() ? '?' : field[0];
    }
    *ticks = std::strtoull(field.c_str(), nullptr, 10);
    return true;
}

struct SavedState {
    pid_t pid = 0;
    long started = 0;
    unsigned long long ticks = 0;
    std::vector<std::string> modules;
    bool valid = false;
};

void write_state(const std::string& path, const SavedState& s)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    std::ostringstream out;
    out << "pid=" << s.pid << "\nstarted=" << s.started << "\nticks=" << s.ticks << "\nmodules=";
    for (size_t i = 0; i < s.modules.size(); ++i)
        out << (i ? " " : "") << s.modules[i];
    out << "\n";
    const std::string text = out.str();
    if (::write(fd, text.data(), text.size()) < 0) {
        // best effort: without the state file the stream just cannot be re-attached
    }
    ::close(fd);
}

SavedState read_state(const std::string& path)
{
    SavedState s;
    std::ifstream f(path);
    std::string line;
    while (f && std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "pid")
            s.pid = static_cast<pid_t>(std::atol(v.c_str()));
        else if (k == "started")
            s.started = std::atol(v.c_str());
        else if (k == "ticks")
            s.ticks = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "modules") {
            std::istringstream ids(v);
            for (std::string id; ids >> id;)
                s.modules.push_back(id);
        }
    }
    s.valid = s.pid > 1 && s.ticks > 0;
    return s;
}

bool is_same_process(pid_t pid, unsigned long long ticks)
{
    unsigned long long now = 0;
    // A zombie (state Z) has exited and only waits for a parent to reap it. When the app that started a stream
    // is gone, the launcher inherits the supervisor and may never reap it; counting it as alive made
    // "stop" wait forever for a stream that was already over.
    char state = '?';
    return pid > 1 && read_start_ticks(pid, &now, &state) && now == ticks && state != 'Z' && state != 'X';
}

// Unloads PipeWire modules by id, but only ones that are ours (ids are reused after a reboot).
void unload_audio_modules(const std::vector<std::string>& ids)
{
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        const std::string ours =
            capture("pactl list short modules | awk -v id=" + *it + " '$1==id && /zerocast_mix/ {print $1}'");
        if (!ours.empty())
            capture("pactl unload-module " + *it);
    }
}

volatile sig_atomic_t g_app_gone = 0;
void on_app_gone(int) { g_app_gone = 1; }
// SIGTERM to a stand-alone supervisor means systemd (shutdown, logout, `systemctl stop`) wants the stream gone.
volatile sig_atomic_t g_terminate = 0;
void on_terminate(int) { g_terminate = 1; }

// The supervisor: runs in its own session as the parent of the shell pipeline. Only simple
// system calls are used here (it is a fork of the app, so no UI or allocation-heavy code).
// What a stand-alone supervisor (started by systemd, not forked by the app) needs to know.
struct SupervisorJob {
    pid_t app_pid = 0;
    unsigned long long app_ticks = 0;
    std::string keep_file;
    std::string state_file;
    std::string shell;
    std::vector<std::string> modules;
};

// stand_alone: the supervisor was started by systemd (so its parent is the service manager, which reaps it,
// instead of whatever launcher adopts a forked orphan). It cannot use PR_SET_PDEATHSIG because the app is not
// its parent; it watches the app's pid instead, and writes the state file and the exit code itself.
[[noreturn]] void run_supervisor(const std::string& shell,
                                 const std::string& keep_file,
                                 pid_t app_pid,
                                 const std::vector<std::string>& audio_modules,
                                 const SupervisorJob* stand_alone = nullptr)
{
    if (::setsid() < 0)
        ::setpgid(0, 0);  // already a session leader (systemd) or not: make sure we lead our own group
    // The app stops a stream with SIGINT to the whole group; the pipeline must see it, the supervisor
    // must outlive it long enough to report the end of the pipeline.
    ::signal(SIGINT, SIG_IGN);
    ::signal(SIGHUP, SIG_IGN);
    struct sigaction sa {};
    sa.sa_handler = stand_alone ? on_terminate : on_app_gone;
    ::sigaction(SIGTERM, &sa, nullptr);
    if (!stand_alone) {
        // When the app dies for any reason (exit, crash, SIGKILL by the launcher) we get SIGTERM.
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != app_pid)
            _exit(1);  // the app already died before we could register
    }

    const pid_t worker = ::fork();
    if (worker < 0)
        _exit(1);
    if (worker == 0) {
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, 0);
            ::dup2(devnull, 1);
            if (devnull > 2)
                ::close(devnull);
        }
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGHUP, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        ::prctl(PR_SET_PDEATHSIG, 0);
        ::execl("/bin/sh", "sh", "-c", shell.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    if (stand_alone) {
        SavedState me;
        me.pid = ::getpid();
        me.started = wall_sec();
        read_start_ticks(me.pid, &me.ticks);
        me.modules = stand_alone->modules;
        write_state(stand_alone->state_file, me);
    }

    bool shutting_down = false;
    long shutdown_started = 0;
    for (;;) {
        int status = 0;
        if (::waitpid(worker, &status, WNOHANG) == worker) {
            // The app may be gone (killed, or closed with the stream left running), so nobody else
            // would remove the temporary audio mix: do it here.
            unload_audio_modules(audio_modules);
            const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            if (stand_alone) {
                // The app is not our parent, so it cannot wait() for us: leave the result for it.
                const std::string exit_file = stand_alone->state_file + ".exit";
                const int fd = ::open(exit_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
                if (fd >= 0) {
                    const std::string text = "code=" + std::to_string(code) + "\n";
                    if (::write(fd, text.data(), text.size()) < 0) {
                    }
                    ::close(fd);
                }
                ::unlink(stand_alone->state_file.c_str());
            }
            _exit(code);
        }
        if (stand_alone && !g_app_gone && !is_same_process(stand_alone->app_pid, stand_alone->app_ticks))
            g_app_gone = 1;  // no parent-death signal for us: notice the app is gone (or a zombie) ourselves
        if (g_terminate && !shutting_down) {
            shutting_down = true;
            shutdown_started = mono_sec();
            ::killpg(::getpgrp(), SIGINT);
        }
        if (g_app_gone && !shutting_down) {
            if (::access(keep_file.c_str(), F_OK) == 0) {
                g_app_gone = 0;  // "keep streaming when the app closes": carry on without the app
            } else {
                shutting_down = true;
                shutdown_started = mono_sec();
                ::killpg(::getpgrp(), SIGINT);  // graceful: lets ffmpeg finish the stream
            }
        }
        if (shutting_down && mono_sec() - shutdown_started >= kStopGraceSec)
            ::killpg(::getpgrp(), SIGKILL);
        timespec nap{0, 100 * 1000 * 1000};
        ::nanosleep(&nap, nullptr);
    }
}

// The job file hands a stand-alone supervisor everything it needs, including the pipeline (which holds the stream
// keys). It lives in the private runtime directory with mode 0600 and the supervisor deletes it as soon as it
// has read it; nothing secret is passed on a command line.
bool write_job(const std::string& path, const SupervisorJob& job)
{
    if (job.shell.find('\n') != std::string::npos)
        return false;
    ::unlink(path.c_str());
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    std::ostringstream out;
    out << "app_pid=" << job.app_pid << "\napp_ticks=" << job.app_ticks << "\nkeep_file=" << job.keep_file
        << "\nstate_file=" << job.state_file << "\nmodules=";
    for (size_t i = 0; i < job.modules.size(); ++i)
        out << (i ? " " : "") << job.modules[i];
    out << "\nshell=" << job.shell << "\n";
    const std::string text = out.str();
    const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ::close(fd);
    if (!ok)
        ::unlink(path.c_str());
    return ok;
}

bool read_job(const std::string& path, SupervisorJob* job)
{
    std::ifstream f(path);
    std::string line;
    while (f && std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "app_pid")
            job->app_pid = static_cast<pid_t>(std::atol(v.c_str()));
        else if (k == "app_ticks")
            job->app_ticks = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "keep_file")
            job->keep_file = v;
        else if (k == "state_file")
            job->state_file = v;
        else if (k == "shell")
            job->shell = v;
        else if (k == "modules") {
            std::istringstream ids(v);
            for (std::string id; ids >> id;)
                job->modules.push_back(id);
        }
    }
    return job->app_pid > 1 && !job->shell.empty() && !job->state_file.empty();
}

// Asks the service manager to run `zerocast --supervise <job>` as a transient service, so that systemd (not the
// launcher that happens to be our ancestor) is the supervisor's parent: it reaps the process when it ends, and the
// stream is no longer in the launcher's process tree. Returns false if systemd-run is missing or refuses.
bool spawn_with_systemd(const std::string& job_path)
{
    char self[4096];
    const ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0)
        return false;
    self[n] = 0;
    if (::access("/usr/bin/systemd-run", X_OK) != 0)
        return false;

    const uid_t uid = ::geteuid();
    const std::string unit = "--unit=zerocast-stream-" + std::to_string(wall_sec());
    const pid_t child = ::fork();
    if (child < 0)
        return false;
    if (child == 0) {
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, 0);
            ::dup2(devnull, 1);
            ::dup2(devnull, 2);
        }
        if (uid != 0 && !std::getenv("XDG_RUNTIME_DIR"))
            ::setenv("XDG_RUNTIME_DIR", ("/run/user/" + std::to_string(uid)).c_str(), 1);
        std::vector<const char*> argv = {"systemd-run", "--collect", "--quiet", unit.c_str(),
                                         "--description=ZeroCast live stream"};
        if (uid != 0)
            argv.insert(argv.begin() + 1, "--user");
        argv.push_back(self);
        argv.push_back("--supervise");
        argv.push_back(job_path.c_str());
        argv.push_back(nullptr);
        ::execv("/usr/bin/systemd-run", const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    int status = 0;
    for (int i = 0; i < 100; ++i) {  // systemd-run only queues the unit: it returns within a moment
        if (::waitpid(child, &status, WNOHANG) == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        timespec nap{0, 50 * 1000 * 1000};
        ::nanosleep(&nap, nullptr);
    }
    ::kill(child, SIGKILL);
    ::waitpid(child, &status, 0);
    return false;
}

}  // namespace

int run_supervisor_job(const std::string& job_path)
{
    SupervisorJob job;
    const bool ok = read_job(job_path, &job);
    ::unlink(job_path.c_str());  // holds the stream keys: gone as soon as it is read
    if (!ok)
        return 2;
    run_supervisor(job.shell, job.keep_file, job.app_pid, job.modules, &job);
}

const char* state_name(EngineState state)
{
    switch (state) {
    case EngineState::Idle: return "idle";
    case EngineState::Starting: return "starting";
    case EngineState::Live: return "live";
    case EngineState::Stopping: return "stopping";
    case EngineState::Failed: return "failed";
    }
    return "?";
}

StreamEngine::StreamEngine(PipelinePaths paths) : paths_(std::move(paths)) {}

StreamEngine::~StreamEngine()
{
    if (pid_ <= 0)
        return;
    // We still own a running stream: stop it gracefully, then forcefully. (detach() was not called.)
    ::killpg(pid_, SIGINT);
    for (int i = 0; i < 40 && process_alive(); ++i) {
        if (!attached_) {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_)
                break;
        }
        timespec nap{0, 50 * 1000 * 1000};
        ::nanosleep(&nap, nullptr);
    }
    if (process_alive())
        ::killpg(pid_, SIGKILL);
    if (!attached_) {
        int status = 0;
        ::waitpid(pid_, &status, 0);
    }
    ::unlink(paths_.state_file.c_str());
    teardown_audio();
}

bool StreamEngine::process_alive() const
{
    return pid_ > 1 && is_same_process(pid_, start_ticks_);
}

bool StreamEngine::setup_audio(const std::vector<std::string>& commands)
{
    if (commands.empty())
        return true;
    // Remove a mix sink left behind by an earlier crash, or the new one would clash with it.
    const std::string stale = capture("pactl list short modules | awk '/zerocast_mix/ {print $1}'");
    std::istringstream stale_ids(stale);
    for (std::string id; stale_ids >> id;)
        capture("pactl unload-module " + id);

    // pulse-style defaults like @DEFAULT_SOURCE@ are not valid in module arguments.
    const std::string def_source = capture("pactl get-default-source");
    const std::string def_monitor = capture("pactl get-default-sink") + ".monitor";
    for (std::string cmd : commands) {
        replace_all(&cmd, "@DEFAULT_SOURCE@", def_source);
        replace_all(&cmd, "@DEFAULT_MONITOR@", def_monitor);
        const std::string id = capture(cmd);
        if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) {
            error_ = "Audio mixing setup failed";
            teardown_audio();
            return false;
        }
        audio_modules_.push_back(id);
    }
    return true;
}

void StreamEngine::teardown_audio()
{
    unload_audio_modules(audio_modules_);
    audio_modules_.clear();
}

void StreamEngine::set_keep_running(bool keep)
{
    if (keep) {
        const int fd = ::open(paths_.keep_file.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0)
            ::close(fd);
    } else {
        ::unlink(paths_.keep_file.c_str());
    }
}

bool StreamEngine::attach(const Config& cfg)
{
    if (pid_ > 0)
        return false;  // already have one
    const SavedState saved = read_state(paths_.state_file);
    if (!saved.valid)
        return false;
    if (!is_same_process(saved.pid, saved.ticks)) {
        // Stale file from a stream that has ended (or a reboot): clean up what it left behind.
        ::unlink(paths_.state_file.c_str());
        audio_modules_ = saved.modules;
        teardown_audio();
        return false;
    }

    pid_ = saved.pid;
    start_ticks_ = saved.ticks;
    attached_ = true;
    audio_modules_ = saved.modules;
    secrets_.clear();
    for (const Destination& d : cfg.destinations) {
        if (d.enabled && !d.key.empty())
            secrets_.push_back(d.key);
    }
    stats_ = EngineStats();
    error_.clear();
    started_at_ = mono_sec() - std::max(0L, wall_sec() - saved.started);
    stop_requested_at_ = 0;
    state_ = EngineState::Live;
    read_progress();
    return true;
}

void StreamEngine::detach()
{
    if (pid_ <= 0)
        return;
    // The state file already describes the stream (pid, start time, audio modules); the next
    // instance attaches from it. Leave the audio mix sink loaded for the same reason.
    pid_ = -1;
    attached_ = false;
    audio_modules_.clear();
    state_ = EngineState::Idle;
}

bool StreamEngine::start(const Config& cfg)
{
    if (state_ == EngineState::Starting || state_ == EngineState::Live || state_ == EngineState::Stopping) {
        error_ = "Already running";
        return false;
    }
    if (attach(cfg)) {
        error_ = "A stream is already running in the background";
        return false;
    }

    Config resolved = cfg;
    if (resolved.sources.video == VideoSource::Screen && resolved.sources.screen == ScreenTarget::Lcd &&
        resolved.sources.lcd_device == "auto") {
        resolved.sources.lcd_device = detect_lcd_framebuffer();
        if (resolved.sources.lcd_device.empty()) {
            error_ = "Built-in LCD framebuffer not found";
            state_ = EngineState::Failed;
            return false;
        }
    }
    if (resolved.sources.video == VideoSource::Usb) {
        // A chosen camera that was unplugged (or renumbered) falls back to the first one present.
        const std::vector<UsbCamera> cams = list_usb_cameras();
        bool present = false;
        for (const UsbCamera& c : cams)
            present = present || c.device == resolved.sources.usb_device;
        if (resolved.sources.usb_device == "auto" || !present) {
            if (cams.empty()) {
                error_ = "No USB camera found. Check it is plugged in.";
                state_ = EngineState::Failed;
                return false;
            }
            resolved.sources.usb_device = cams.front().device;
        }
        // Ask the camera what it offers and request the best match for the output; a camera that
        // cannot be probed is left to ffmpeg's own negotiation.
        const bool auto_mode = resolved.sources.usb_size == "auto" || resolved.sources.usb_size.empty();
        if (resolved.sources.usb_format == "raw")
            resolved.sources.usb_format = "yuyv422";  // used only if the camera cannot be probed
        // "auto" and "raw" are resolved from what the camera offers. A forced format the camera does not
        // have falls back to auto rather than failing.
        const std::string wanted_format = cfg.sources.usb_format;
        if (wanted_format == "auto" || wanted_format == "raw" || auto_mode) {
            const std::vector<CaptureMode> modes = probe_usb_modes(resolved.sources.usb_device);
            CaptureChoice c = choose_capture_mode(modes, resolved.video.width, resolved.video.height, resolved.video.fps, wanted_format);
            bool format_from_probe = wanted_format == "auto" || wanted_format == "raw";
            if (!c.found && !modes.empty()) {
                c = choose_capture_mode(modes, resolved.video.width, resolved.video.height, resolved.video.fps, "auto");
                format_from_probe = true;
            }
            if (c.found) {
                if (format_from_probe)
                    resolved.sources.usb_format = c.format;
                if (auto_mode) {
                    resolved.sources.usb_size = std::to_string(c.width) + "x" + std::to_string(c.height);
                    if (resolved.sources.usb_fps <= 0)
                        resolved.sources.usb_fps = c.fps;
                }
            }
        }
    }
    if (resolved.sources.video == VideoSource::Screen && resolved.sources.screen == ScreenTarget::Hdmi &&
        resolved.sources.wayland_display == "auto") {
        resolved.sources.wayland_display = detect_wayland_display(resolved.sources.runtime_dir);
        if (resolved.sources.wayland_display.empty()) {
            error_ = "No Wayland desktop session found for screen capture";
            state_ = EngineState::Failed;
            return false;
        }
    }
    Pipeline pl = build_pipeline(resolved, paths_);
    if (!pl.ok()) {
        error_ = pl.errors.empty() ? "Invalid configuration" : pl.errors.front();
        state_ = EngineState::Failed;
        return false;
    }

    if (!setup_audio(pl.audio_setup)) {
        state_ = EngineState::Failed;
        return false;
    }

    // Fresh log/progress files; the log is created 0600 because ffmpeg may echo URLs.
    ::unlink(paths_.progress_file.c_str());
    const int lfd = ::open(paths_.log_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lfd >= 0)
        ::close(lfd);

    const pid_t app_pid = ::getpid();
    pid_t pid = -1;
    bool via_systemd = false;
    SavedState service_state;
    {
        // Preferred: the supervisor runs as a transient systemd service, so systemd reaps it and it is not in the
        // launcher's process tree (the launcher adopts orphans and never reaps them: zombies).
        SupervisorJob job;
        job.app_pid = app_pid;
        read_start_ticks(app_pid, &job.app_ticks);
        job.keep_file = paths_.keep_file;
        job.state_file = paths_.state_file;
        job.shell = pl.shell;
        job.modules = audio_modules_;
        const std::string job_path = paths_.state_file + ".job";
        ::unlink(paths_.state_file.c_str());
        ::unlink((paths_.state_file + ".exit").c_str());
        if (job.app_ticks > 0 && write_job(job_path, job)) {
            if (spawn_with_systemd(job_path)) {
                for (int i = 0; i < 100; ++i) {  // the supervisor writes the state file once it is running
                    service_state = read_state(paths_.state_file);
                    if (service_state.valid && is_same_process(service_state.pid, service_state.ticks))
                        break;
                    timespec nap{0, 50 * 1000 * 1000};
                    ::nanosleep(&nap, nullptr);
                }
                ::unlink(job_path.c_str());
                if (!service_state.valid) {
                    error_ = "The stream service did not start";
                    teardown_audio();
                    state_ = EngineState::Failed;
                    return false;
                }
                pid = service_state.pid;
                via_systemd = true;
            } else {
                ::unlink(job_path.c_str());
            }
        }
    }
    if (!via_systemd) {
        // No systemd (or it refused): fork the supervisor ourselves. Works the same except that a stream left
        // running in the background becomes a child of whatever adopts it when the app exits.
        pid = ::fork();
        if (pid < 0) {
            error_ = std::string("fork failed: ") + std::strerror(errno);
            teardown_audio();
            state_ = EngineState::Failed;
            return false;
        }
        if (pid == 0)
            run_supervisor(pl.shell, paths_.keep_file, app_pid, audio_modules_);
    }

    pid_ = pid;
    attached_ = via_systemd;  // not our child: we cannot wait() for it
    secrets_.clear();
    for (const Destination& d : cfg.destinations) {
        if (d.enabled && !d.key.empty())
            secrets_.push_back(d.key);
    }
    stats_ = EngineStats();
    error_.clear();
    started_at_ = mono_sec();
    stop_requested_at_ = 0;
    start_timed_out_ = false;
    state_ = EngineState::Starting;

    if (via_systemd) {
        start_ticks_ = service_state.ticks;  // the supervisor wrote its own state file
        return true;
    }
    SavedState saved;
    saved.pid = pid;
    saved.started = wall_sec();
    for (int i = 0; i < 20 && !read_start_ticks(pid, &saved.ticks); ++i) {
        timespec nap{0, 5 * 1000 * 1000};
        ::nanosleep(&nap, nullptr);
    }
    saved.modules = audio_modules_;
    start_ticks_ = saved.ticks;
    write_state(paths_.state_file, saved);
    return true;
}

void StreamEngine::stop()
{
    if (pid_ <= 0 || state_ == EngineState::Stopping)
        return;
    ::killpg(pid_, SIGINT);
    state_ = EngineState::Stopping;
    stop_requested_at_ = mono_sec();
    if (!process_alive() && attached_)
        finish(-1);  // already gone (or a zombie): there is nothing to wait for
}

void StreamEngine::finish(int wait_status)
{
    const bool stopped_by_user = (state_ == EngineState::Stopping) && !start_timed_out_;
    if (wait_status < 0) {
        // We could not wait() for the supervisor (it is a systemd service, or from an earlier app instance):
        // it left its exit code in a file.
        std::ifstream exit_file(paths_.state_file + ".exit");
        std::string line;
        if (exit_file && std::getline(exit_file, line) && line.rfind("code=", 0) == 0)
            wait_status = (std::atoi(line.c_str() + 5) & 0xff) << 8;  // as waitpid() reports a normal exit
    }
    ::unlink((paths_.state_file + ".exit").c_str());
    pid_ = -1;
    attached_ = false;
    ::unlink(paths_.state_file.c_str());
    teardown_audio();
    if (stopped_by_user) {
        state_ = EngineState::Idle;
        return;
    }
    // The pipeline ended on its own: that is a failure unless it exited cleanly.
    // (wait_status < 0: we were attached and cannot know the exit status.)
    if (!start_timed_out_ && wait_status >= 0 && WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
        state_ = EngineState::Idle;
        return;
    }
    state_ = EngineState::Failed;
    if (start_timed_out_) {
        start_timed_out_ = false;
        error_ = "No data after " + std::to_string(kStartTimeoutSec) + " s: replug camera/mic";
        return;
    }
    error_ = summarize_error(log_tail(40));
    if (error_.empty()) {
        error_ = (wait_status >= 0 && WIFSIGNALED(wait_status)) ? "Stream was stopped from outside"
                                                              : "Stream ended unexpectedly";
    }
}

void StreamEngine::poll()
{
    if (pid_ <= 0)
        return;

    if (attached_) {
        if (!process_alive()) {
            finish(-1);
            return;
        }
    } else {
        int status = 0;
        if (::waitpid(pid_, &status, WNOHANG) == pid_) {
            finish(status);
            return;
        }
    }

    if (state_ == EngineState::Stopping) {
        const long waited = mono_sec() - stop_requested_at_;
        if (waited >= kStopGraceSec) {
            ::killpg(pid_, SIGKILL);
            ::kill(pid_, SIGKILL);
        }
        // Stopping never hangs: if the process is still listed a few seconds after SIGKILL (a process stuck in the
        // kernel, or a zombie nobody reaps), stop waiting for it and report the stream as stopped.
        if (waited >= kStopGraceSec + kStopForceSec) {
            if (!attached_) {
                int status = 0;
                ::waitpid(pid_, &status, WNOHANG);
            }
            finish(-1);
            return;
        }
    }

    if (state_ == EngineState::Starting || state_ == EngineState::Live) {
        read_progress();
        stats_.uptime_sec = mono_sec() - started_at_;
        // ffmpeg writes progress once it is actually encoding and sending.
        if (state_ == EngineState::Starting && stats_.fps > 0)
            state_ = EngineState::Live;
        else if (state_ == EngineState::Starting && stats_.uptime_sec >= kStartTimeoutSec) {
            start_timed_out_ = true;
            stop();
        }
    }
}

void StreamEngine::read_progress()
{
    std::ifstream f(paths_.progress_file);
    if (!f)
        return;
    // The progress file is append-only blocks; scan it and keep the last values.
    // With -c:v copy ffmpeg reports bitrate=N/A, so derive it from bytes sent / media time.
    long total_size = 0;
    long out_time_us = 0;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if (k == "fps")
            stats_.fps = std::atof(v.c_str());
        else if (k == "speed") {
            if (v != "N/A")
                stats_.speed = std::atof(v.c_str());
        } else if (k == "total_size") {
            total_size = std::atol(v.c_str());
        } else if (k == "out_time_us") {
            out_time_us = std::atol(v.c_str());
        } else if (k == "drop_frames")
            stats_.dropped_frames = std::atol(v.c_str());
    }
    if (total_size > 0 && out_time_us > 1000000)
        stats_.bitrate_kbps = static_cast<int>(total_size * 8 * 1000L / out_time_us);
}

std::string summarize_error(const std::string& log)
{
    // One short, readable line from the pipeline log: the last line that looks like an error, else the
    // last line that is not encoder/camera chatter. "[h264_v4l2m2m @ 0x55..] Using device ..." and
    // libcamera's INFO/mode-selection output say what it was doing, not why it stopped.
    const auto contains = [](const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; };
    std::vector<std::string> lines;
    std::istringstream in(log);
    for (std::string l; std::getline(in, l);)
        if (!l.empty())
            lines.push_back(l);

    std::string pick, fallback;
    for (const std::string& l : lines) {
        const bool chatter = contains(l, "INFO") || contains(l, "Score") || contains(l, "Mode selection") ||
                             contains(l, "<<<") || contains(l, "Using device") || contains(l, "driver '") ||
                             contains(l, "requesting formats") || contains(l, "Stream configuration adjusted") ||
                             contains(l, "SRGGB") || contains(l, "Available camera");
        if (!chatter)
            fallback = l;
        if (!chatter && (contains(l, "rror") || contains(l, "ail") || contains(l, "efused") || contains(l, "enied") ||
                         contains(l, "No such") || contains(l, "annot") || contains(l, "nvalid") ||
                         contains(l, "imed out") || contains(l, "onnection") || contains(l, "busy")))
            pick = l;
    }
    std::string text = !pick.empty() ? pick : fallback;
    // Drop a leading "[component @ 0x...] " tag.
    if (!text.empty() && text[0] == '[') {
        const size_t close = text.find("] ");
        if (close != std::string::npos)
            text = text.substr(close + 2);
    }
    constexpr size_t kMax = 70;
    if (text.size() > kMax)
        text = text.substr(0, kMax - 1) + "~";
    return text;
}

std::string StreamEngine::log_tail(int max_lines) const
{
    std::ifstream f(paths_.log_file);
    if (!f)
        return "";
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty())
            lines.push_back(line);
    }
    std::ostringstream out;
    size_t begin = lines.size() > static_cast<size_t>(max_lines) ? lines.size() - max_lines : 0;
    for (size_t i = begin; i < lines.size(); ++i)
        out << lines[i] << (i + 1 < lines.size() ? "\n" : "");
    std::string text = out.str();
    // ffmpeg errors echo the full RTMP URL; never show a stream key.
    for (const std::string& secret : secrets_) {
        for (size_t pos = text.find(secret); pos != std::string::npos; pos = text.find(secret, pos + 4))
            text.replace(pos, secret.size(), "****");
    }
    return text;
}

}  // namespace stream
