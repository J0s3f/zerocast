/*
 * SPDX-License-Identifier: MIT
 */

#include "pipeline.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace stream {

std::string detect_lcd_framebuffer()
{
    for (int i = 0; i < 8; ++i) {
        std::string name_path = "/sys/class/graphics/fb" + std::to_string(i) + "/name";
        FILE* f = std::fopen(name_path.c_str(), "r");
        if (!f)
            continue;
        char buf[64] = {0};
        const bool read_ok = std::fgets(buf, sizeof(buf), f) != nullptr;
        std::fclose(f);
        if (!read_ok)
            continue;
        const std::string name(buf);
        if (name.find("panel-mipi-dbid") != std::string::npos || name.find("st7789") != std::string::npos)
            return "/dev/fb" + std::to_string(i);
    }
    return "";
}

std::vector<UsbCamera> list_usb_cameras()
{
    std::vector<UsbCamera> found;
    std::vector<int> nodes;
    if (DIR* dir = ::opendir("/sys/class/video4linux")) {
        while (dirent* e = ::readdir(dir)) {
            const std::string n = e->d_name;
            if (n.rfind("video", 0) == 0 && n.size() > 5 && std::all_of(n.begin() + 5, n.end(), ::isdigit))
                nodes.push_back(std::atoi(n.c_str() + 5));
        }
        ::closedir(dir);
    }
    std::sort(nodes.begin(), nodes.end());

    auto read_line = [](const std::string& path) {
        std::string out;
        if (FILE* f = std::fopen(path.c_str(), "r")) {
            char buf[128] = {0};
            if (std::fgets(buf, sizeof(buf), f))
                out = buf;
            std::fclose(f);
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        return out;
    };

    for (int n : nodes) {
        const std::string base = "/sys/class/video4linux/video" + std::to_string(n);
        char resolved[PATH_MAX] = {0};
        // The camera is a USB device if its sysfs parent chain goes through the USB bus; the
        // built-in CSI camera and the codec/ISP nodes are on the platform bus.
        if (!::realpath((base + "/device").c_str(), resolved) || std::string(resolved).find("/usb") == std::string::npos)
            continue;
        if (read_line(base + "/index") != "0")
            continue;  // UVC cameras also expose a metadata node (index 1)
        found.push_back({"/dev/video" + std::to_string(n), read_line(base + "/name")});
    }
    return found;
}

std::string detect_usb_camera(std::string* name)
{
    const std::vector<UsbCamera> cams = list_usb_cameras();
    if (cams.empty())
        return "";
    if (name)
        *name = cams.front().name;
    return cams.front().device;
}

namespace {

// V4L2 fourcc -> ffmpeg pixel format / codec name. Formats not listed (H.264 elementary streams,
// Bayer, depth, grey, ...) are ignored: we only pick formats ffmpeg can turn into video reliably.
const char* fourcc_to_ffmpeg(const std::string& fourcc)
{
    if (fourcc == "MJPG" || fourcc == "JPEG") return "mjpeg";
    if (fourcc == "YUYV") return "yuyv422";
    if (fourcc == "UYVY") return "uyvy422";
    if (fourcc == "NV12") return "nv12";
    if (fourcc == "YU12" || fourcc == "I420") return "yuv420p";
    return nullptr;
}

// Lower is better. MJPEG first: a USB 2.0 camera only reaches 720p30 or 1080p30 compressed.
int format_rank(const std::string& f)
{
    if (f == "mjpeg") return 0;
    if (f == "yuyv422") return 1;
    if (f == "nv12") return 2;
    if (f == "yuv420p") return 3;
    return 4;
}

}  // namespace

std::vector<CaptureMode> parse_v4l2_modes(const std::string& text)
{
    std::vector<CaptureMode> modes;
    std::string format;  // current format section; "" = skip
    int w = 0, h = 0;
    bool have_size = false;
    long current = -1;  // index of the mode the next Interval lines belong to

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t q = line.find('\'');
        if (line.find("]: '") != std::string::npos && q != std::string::npos && line.size() >= q + 5) {
            const char* f = fourcc_to_ffmpeg(line.substr(q + 1, 4));
            format = f ? f : "";
            have_size = false;
            current = -1;
            continue;
        }
        const size_t sz = line.find("Size:");
        if (sz != std::string::npos) {
            have_size = false;
            current = -1;
            if (format.empty())
                continue;
            // "Size: Discrete 1280x720"  or  "Size: Stepwise 160x120 - 1920x1080 with step 16/16"
            const bool stepwise = line.find("Stepwise") != std::string::npos || line.find("Continuous") != std::string::npos;
            int a = 0, b = 0, c = 0, d = 0;
            const char* p = line.c_str() + sz;
            while (*p && !std::isdigit(static_cast<unsigned char>(*p)))
                ++p;
            if (stepwise) {
                if (std::sscanf(p, "%dx%d - %dx%d", &a, &b, &c, &d) == 4) {
                    w = c;
                    h = d;
                    have_size = true;
                }
            } else if (std::sscanf(p, "%dx%d", &a, &b) == 2) {
                w = a;
                h = b;
                have_size = true;
            }
            if (have_size && w > 0 && h > 0) {
                modes.push_back({format, w, h, 0.0});
                current = static_cast<long>(modes.size()) - 1;
            } else {
                have_size = false;
            }
            continue;
        }
        const size_t iv = line.find("Interval:");
        if (iv != std::string::npos && current >= 0) {
            // "Interval: Discrete 0.033s (30.000 fps)"  or  "Interval: Stepwise 0.033s - 1.000s with step ..."
            double fps = 0.0;
            const size_t paren = line.find('(');
            if (paren != std::string::npos && std::sscanf(line.c_str() + paren + 1, "%lf", &fps) == 1) {
                // discrete: keep the best
            } else {
                double shortest = 0.0;
                const char* p = line.c_str() + iv;
                while (*p && !std::isdigit(static_cast<unsigned char>(*p)))
                    ++p;
                if (std::sscanf(p, "%lf", &shortest) == 1 && shortest > 0.0)
                    fps = 1.0 / shortest;
            }
            if (fps > modes[current].max_fps)
                modes[current].max_fps = fps;
        }
    }
    // A mode without any interval line (some drivers) is kept with an unknown rate (0): the
    // chooser treats it as "assume it can do the requested rate".
    return modes;
}

std::vector<CaptureMode> probe_usb_modes(const std::string& device)
{
    std::string out;
    char buf[1024];
    if (FILE* p = ::popen(("v4l2-ctl -d " + shell_quote(device) + " --list-formats-ext 2>/dev/null").c_str(), "r")) {
        while (std::fgets(buf, sizeof(buf), p))
            out += buf;
        ::pclose(p);
    }
    std::vector<CaptureMode> modes = parse_v4l2_modes(out);
    if (!modes.empty())
        return modes;

    // v4l2-ctl missing or the node does not answer: ask ffmpeg, which lists sizes but not rates.
    out.clear();
    if (FILE* p = ::popen(("ffmpeg -nostdin -hide_banner -f v4l2 -list_formats all -i " + shell_quote(device) + " 2>&1").c_str(), "r")) {
        while (std::fgets(buf, sizeof(buf), p))
            out += buf;
        ::pclose(p);
    }
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        // "... Compressed:  mjpeg : Motion-JPEG : 1920x1080 1280x720 640x480"
        for (const char* fmt : {"mjpeg", "yuyv422", "uyvy422", "nv12", "yuv420p"}) {
            const std::string needle = std::string(" ") + fmt + " :";
            const size_t pos = line.find(needle);
            if (pos == std::string::npos)
                continue;
            const size_t colon = line.find(" : ", pos + needle.size() - 1);
            if (colon == std::string::npos)
                continue;
            std::istringstream sizes(line.substr(colon + 3));
            std::string tok;
            while (sizes >> tok) {
                int w = 0, h = 0;
                if (std::sscanf(tok.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0)
                    modes.push_back({fmt, w, h, 0.0});
            }
        }
    }
    return modes;
}

CaptureChoice choose_capture_mode(const std::vector<CaptureMode>& modes, int out_w, int out_h, int fps,
                                  const std::string& only_format)
{
    CaptureChoice best;
    const auto usable = [&](const CaptureMode& m) {
        if (only_format.empty() || only_format == "auto")
            return true;
        if (only_format == "raw")
            return m.format != "mjpeg";  // any uncompressed format
        return m.format == only_format;
    };
    const auto reaches = [&](const CaptureMode& m) { return m.max_fps <= 0.0 || m.max_fps + 0.5 >= fps; };

    // Tier 1: delivers the requested rate at a size that covers the output, so nothing is upscaled.
    // Tier 2: delivers the requested rate at any size (largest). Tier 3: anything, fastest first.
    int best_tier = 4;
    const CaptureMode* pick = nullptr;
    const auto better = [&](const CaptureMode& m, int tier) {
        if (!pick || tier < best_tier)
            return true;
        if (tier > best_tier)
            return false;
        const long area = static_cast<long>(m.width) * m.height, parea = static_cast<long>(pick->width) * pick->height;
        if (tier == 1) {
            // Smallest picture that covers the output (least decode work); then the better format.
            if (area != parea)
                return area < parea;
            return format_rank(m.format) < format_rank(pick->format);
        }
        if (tier == 2) {
            if (area != parea)
                return area > parea;
            return format_rank(m.format) < format_rank(pick->format);
        }
        if (m.max_fps != pick->max_fps)
            return m.max_fps > pick->max_fps;
        return area > parea;
    };
    for (const CaptureMode& m : modes) {
        if (!usable(m))
            continue;
        const bool covers = m.width >= out_w && m.height >= out_h;
        const int tier = reaches(m) ? (covers ? 1 : 2) : 3;
        // Within tier 1 a compressed 720p beats a raw 1080p, but a raw format at the same size is
        // only chosen when the camera has no MJPEG at that size (rank breaks the tie above).
        if (better(m, tier)) {
            pick = &m;
            best_tier = tier;
        }
    }
    if (!pick)
        return best;
    best.found = true;
    best.format = pick->format;
    best.width = pick->width;
    best.height = pick->height;
    best.fps = (pick->max_fps <= 0.0 || pick->max_fps + 0.5 >= fps) ? fps : std::max(1, static_cast<int>(pick->max_fps + 0.5));
    return best;
}

std::vector<std::string> list_audio_inputs()
{
    std::vector<std::string> names;
    FILE* p = ::popen("pactl list short sources 2>/dev/null", "r");
    if (!p)
        return names;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) {
        std::istringstream line(buf);
        std::string id, name;
        if (!(line >> id >> name))
            continue;
        if (name.size() > 8 && name.compare(name.size() - 8, 8, ".monitor") == 0)
            continue;  // monitors capture playback (system audio), not a microphone
        names.push_back(name);
    }
    ::pclose(p);
    return names;
}

std::string detect_wayland_display(const std::string& runtime_dir)
{
    DIR* dir = ::opendir(runtime_dir.c_str());
    if (!dir)
        return "";
    std::vector<std::string> names;
    while (dirent* e = ::readdir(dir)) {
        const std::string n = e->d_name;
        if (n.rfind("wayland-", 0) == 0 && n.find(".lock") == std::string::npos)
            names.push_back(n);
    }
    ::closedir(dir);
    // Highest number first: after a compositor restart the older sockets are stale.
    std::sort(names.rbegin(), names.rend());
    for (const std::string& n : names) {
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const std::string path = runtime_dir + "/" + n;
        if (path.size() >= sizeof(addr.sun_path))
            continue;
        std::copy(path.begin(), path.end(), addr.sun_path);
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            continue;
        const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        ::close(fd);
        if (ok)
            return n;
    }
    return "";
}

std::string default_runtime_dir()
{
    const uid_t uid = ::geteuid();
    std::vector<std::string> candidates;
    if (uid == 0)
        candidates.push_back("/run/zerocast");
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && xdg[0] == '/')
        candidates.push_back(std::string(xdg) + "/zerocast");
    candidates.push_back("/tmp/zerocast-" + std::to_string(static_cast<unsigned>(uid)));

    for (const std::string& dir : candidates) {
        if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
            continue;
        struct stat st {};
        // lstat, not stat: never follow a symlink somebody planted at this name.
        if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != uid)
            continue;
        if ((st.st_mode & 077) != 0 && ::chmod(dir.c_str(), 0700) != 0)
            continue;
        return dir;
    }
    return "/tmp";  // last resort; should not happen
}

PipelinePaths::PipelinePaths() : PipelinePaths(default_runtime_dir()) {}

PipelinePaths::PipelinePaths(const std::string& dir)
    : progress_file(dir + "/stream.progress"),
      log_file(dir + "/stream.log"),
      state_file(dir + "/stream.state"),
      keep_file(dir + "/keep-running")
{
}

std::string shell_quote(const std::string& s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

namespace {

constexpr int kLcdWidth = 320;
constexpr int kLcdHeight = 170;
constexpr const char* kMixSink = "zerocast_mix";

bool is_rtmp_url(const std::string& url)
{
    return url.rfind("rtmp://", 0) == 0 || url.rfind("rtmps://", 0) == 0;
}

std::string join_url(const std::string& base, const std::string& key)
{
    std::string b = base;
    while (!b.empty() && b.back() == '/')
        b.pop_back();
    return key.empty() ? b : b + "/" + key;
}

// Which audio the *producer* captures, and any PipeWire/pulse setup it needs.
//
// Audio is captured inside the producer (rpicam-vid, the LCD ffmpeg or wf-recorder), which
// muxes it with the video using the same clock. Mixing a real-time microphone into the
// *consumer* ffmpeg was measured to stall the video: the muxer waits for audio, the video
// input queue fills, the pipe blocks and the camera drops frames (about 2 of 3 frames lost).
struct AudioPlan {
    bool any = false;
    std::string device;              // pulse source name for the producer
    std::vector<std::string> setup;  // commands run before start; each prints a module id
};

AudioPlan plan_audio(const Sources& s)
{
    AudioPlan a;
    if (s.mic && s.system_audio) {
        // Mic + system audio: mix them in a temporary null sink and record its monitor.
        a.any = true;
        a.device = std::string(kMixSink) + ".monitor";
        a.setup = {
            std::string("pactl load-module module-null-sink sink_name=") + kMixSink +
                " sink_properties=device.description=ZeroCastMix",
            "pactl load-module module-loopback source=" + shell_quote(s.mic_device) +
                " sink=" + kMixSink + " latency_msec=40",
            "pactl load-module module-loopback source=" + shell_quote(s.system_device) +
                " sink=" + kMixSink + " latency_msec=40",
        };
    } else if (s.mic) {
        a.any = true;
        a.device = s.mic_device;
    } else if (s.system_audio) {
        a.any = true;
        a.device = s.system_device;
    }
    return a;
}

// Producer: writes MPEG-TS (H.264 video, plus AAC audio when selected) to stdout.
std::string producer(const Config& cfg, const AudioPlan& audio, const PipelinePaths& paths)
{
    const VideoSettings& v = cfg.video;
    const Sources& s = cfg.sources;
    std::ostringstream cmd;

    if (s.video == VideoSource::Screen && s.screen == ScreenTarget::Lcd) {
        // Built-in LCD: ffmpeg reads the framebuffer and encodes once with the hardware
        // H.264 encoder. The 320x170 panel scales exactly 4x at 1280 wide; letterbox it.
        const int inner_h = std::min(v.height, (v.width * kLcdHeight / kLcdWidth) & ~1);
        const int pad_y = ((v.height - inner_h) / 2) & ~1;
        const std::string filter =
            s.lcd_native
                ? "format=yuv420p"  // the panel's own 320x170, untouched
                : "scale=" + std::to_string(v.width) + ":" + std::to_string(inner_h) +
                      ":flags=neighbor,pad=" + std::to_string(v.width) + ":" + std::to_string(v.height) +
                      ":0:" + std::to_string(pad_y) + ",format=yuv420p";
        cmd << "ffmpeg -nostdin -hide_banner -loglevel error -f fbdev -framerate " << v.fps
            << " -i " << shell_quote(s.lcd_device);
        if (audio.any)
            cmd << " -thread_queue_size 512 -f pulse -i " << shell_quote(audio.device);
        cmd << " -map 0:v";
        if (audio.any)
            cmd << " -map 1:a -c:a aac -b:a " << v.audio_bitrate_kbps << "k";
        // -fps_mode cfr: a static screen yields identical frames and sparse encoder output
        // (measured ~5 fps with gaps over 1 s); a constant rate keeps YouTube/Twitch happy.
        cmd << " -vf " << shell_quote(filter) << " -fps_mode cfr -r " << v.fps
            << " -c:v h264_v4l2m2m -b:v " << v.bitrate_kbps << "k -g " << v.fps * 2
            << " -f mpegts -";
    } else if (s.video == VideoSource::Screen) {
        // HDMI/desktop (Wayland) capture; hardware H.264 so the CPU is not the bottleneck.
        cmd << "XDG_RUNTIME_DIR=" << shell_quote(s.runtime_dir)
            << " WAYLAND_DISPLAY=" << shell_quote(s.wayland_display)
            << " wf-recorder -y -c h264_v4l2m2m -x yuv420p -r " << v.fps
            << " -p b=" << v.bitrate_kbps * 1000
            << " -F " << shell_quote("scale=" + std::to_string(v.width) + ":" + std::to_string(v.height));
        if (audio.any)
            cmd << " --audio=" << shell_quote(audio.device) << " -C aac";
        cmd << " -m mpegts -f /dev/stdout";
    } else if (s.video == VideoSource::Usb) {
        // USB webcam (UVC): ffmpeg captures through V4L2 and encodes once with the hardware H.264
        // encoder, like the LCD path. MJPEG input keeps a USB 2.0 camera at 720p30; the scale+pad
        // letterboxes whatever size the camera actually delivers into the configured frame, and
        // -fps_mode cfr keeps the output at a steady frame rate even if the camera varies.
        std::string filter = "scale=" + std::to_string(v.width) + ":" + std::to_string(v.height) +
                             ":force_original_aspect_ratio=decrease,pad=" + std::to_string(v.width) + ":" +
                             std::to_string(v.height) + ":(ow-iw)/2:(oh-ih)/2,format=yuv420p";
        if (s.usb_rotate_180)
            filter = "hflip,vflip," + filter;
        cmd << "ffmpeg -nostdin -hide_banner -loglevel error -f v4l2";
        if (!s.usb_format.empty() && s.usb_format != "auto")
            cmd << " -input_format " << shell_quote(s.usb_format);
        // The camera is asked for the mode chosen at start (usb_size/usb_fps); when that is unresolved it
        // gets the output size and rate and ffmpeg negotiates the nearest one.
        const std::string capture_size =
            (s.usb_size.empty() || s.usb_size == "auto") ? std::to_string(v.width) + "x" + std::to_string(v.height) : s.usb_size;
        cmd << " -framerate " << (s.usb_fps > 0 ? s.usb_fps : v.fps) << " -video_size " << capture_size << " -i "
            << shell_quote(s.usb_device);
        if (audio.any)
            cmd << " -thread_queue_size 512 -f pulse -i " << shell_quote(audio.device);
        cmd << " -map 0:v";
        if (audio.any)
            cmd << " -map 1:a -c:a aac -b:a " << v.audio_bitrate_kbps << "k";
        cmd << " -vf " << shell_quote(filter) << " -fps_mode cfr -r " << v.fps
            << " -c:v h264_v4l2m2m -b:v " << v.bitrate_kbps << "k -g " << v.fps * 2 << " -f mpegts -";
    } else {
        // Camera: the libav path runs the same hardware H.264 encoder (/dev/video11) but muxes
        // MPEG-TS with the camera's real sensor timestamps. Raw --codec h264 has none, and faking
        // them (fixed -framerate or wall-clock stamps) drifts against real time (the IMX219
        // delivers ~21 fps at 25 requested, which made YouTube report "not enough video data").
        // --intra: a keyframe every ~2 seconds, as Twitch and YouTube require.
        cmd << "rpicam-vid -t 0 -n --width " << v.width << " --height " << v.height
            << " --framerate " << v.fps << " --codec libav --libav-format mpegts"
            << " --libav-video-codec h264_v4l2m2m --intra " << v.fps * 2
            << " --bitrate " << v.bitrate_kbps * 1000;
        if (s.camera_rotate_180)
            cmd << " --hflip --vflip";  // the CardputerZero camera is mounted upside down
        if (audio.any)
            cmd << " --libav-audio --audio-codec aac --audio-source pulse --audio-device "
                << shell_quote(audio.device) << " --audio-bitrate " << v.audio_bitrate_kbps * 1000;
        cmd << " -o -";
    }
    cmd << " 2>>" << shell_quote(paths.log_file);
    return cmd.str();
}

}  // namespace

Pipeline build_pipeline(const Config& cfg, const PipelinePaths& paths)
{
    Pipeline p;

    struct Out {
        std::string url;
        std::string masked;
    };
    std::vector<Out> outs;
    for (const Destination& d : cfg.destinations) {
        if (!d.enabled)
            continue;
        if (!is_rtmp_url(d.url)) {
            p.errors.push_back(d.name + ": URL must start with rtmp:// or rtmps://");
            continue;
        }
        if (d.key.empty()) {
            p.errors.push_back(d.name + ": stream key is empty");
            continue;
        }
        outs.push_back({join_url(d.url, d.key), mask_key(d.url, d.key)});
    }
    p.destination_count = static_cast<int>(outs.size());
    if (outs.empty()) {
        if (p.errors.empty())
            p.errors.push_back("No destination enabled");
        return p;
    }

    const AudioPlan audio = plan_audio(cfg.sources);
    p.audio_setup = audio.setup;
    const std::string prod = producer(cfg, audio, paths);

    auto build = [&](bool masked) {
        std::ostringstream cmd;
        // Short probing: the default 5 s / 5 MB delayed the first frame by ~9 s.
        cmd << prod << " | ffmpeg -nostdin -hide_banner -loglevel warning -y"
            << " -progress " << shell_quote(paths.progress_file) << " -nostats"
            << " -probesize 200000 -analyzeduration 500000 -fflags +genpts -f mpegts -i -";
        // No audio selected: Twitch and YouTube still expect an audio track, so add silence.
        // (A generated silent input is not real time, so it cannot stall the video.)
        if (!audio.any)
            cmd << " -f lavfi -i anullsrc=channel_layout=stereo:sample_rate=48000";
        for (const Out& o : outs) {
            cmd << " -map 0:v";
            if (audio.any)
                cmd << " -map 0:a -c copy";
            else
                cmd << " -map 1:a -c:v copy -c:a aac -b:a " << cfg.video.audio_bitrate_kbps << "k";
            cmd << " -flvflags no_duration_filesize -f flv " << shell_quote(masked ? o.masked : o.url);
        }
        cmd << " 2>>" << shell_quote(paths.log_file);
        return cmd.str();
    };

    p.shell = build(false);
    p.shell_masked = build(true);

    if (!audio.any)
        p.warnings.push_back("No audio source selected: streaming a silent track");
    if (cfg.sources.video == VideoSource::Screen && cfg.sources.screen == ScreenTarget::Hdmi)
        p.warnings.push_back("HDMI/desktop capture needs a Wayland session and wf-recorder");
    return p;
}

}  // namespace stream
