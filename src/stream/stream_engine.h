/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "pipeline.h"

#include <string>
#include <vector>
#include <sys/types.h>

namespace stream {

enum class EngineState { Idle, Starting, Live, Stopping, Failed };

struct EngineStats {
    double fps = 0;
    int bitrate_kbps = 0;       // derived from bytes sent / media time
    double speed = 0;           // 1.0 = real time
    long uptime_sec = 0;        // seconds since start
    long dropped_frames = 0;
};

const char* state_name(EngineState state);

// Runs the stream pipeline and supervises it.
//
// Each stream is a small supervisor process (its own session) that runs the shell pipeline
// `producer | ffmpeg` as its child. The supervisor is what makes two behaviours deterministic:
//
//  * Default: when the app exits or is killed (the launcher does that when you switch apps),
//    the kernel signals the supervisor, which stops the whole pipeline. No orphans.
//  * Keep running (set_keep_running(true)): the supervisor ignores the app's death and the stream
//    continues in the background. A state file lets the next app instance attach() to it, show
//    its stats and stop it. This is what makes streaming the LCD useful: the stream can outlive
//    the ZeroCast screen and show whatever else you use.
//
// Not thread-safe: call from the UI thread.
// One short line explaining why a pipeline stopped, from the tail of its log ("" if there is nothing useful).
std::string summarize_error(const std::string& log);

// Entry point of `zerocast --supervise <job file>`: the supervisor of one stream, started by systemd. Never returns
// on success (exits with the pipeline's exit code); returns non-zero if the job file is unusable.
int run_supervisor_job(const std::string& job_path);

class StreamEngine {
public:
    explicit StreamEngine(PipelinePaths paths = PipelinePaths());
    // Stops a stream this instance still owns (graceful, then forced). detach() first to leave it.
    ~StreamEngine();

    StreamEngine(const StreamEngine&) = delete;
    StreamEngine& operator=(const StreamEngine&) = delete;

    // Returns false and sets error() when the config is invalid, a stream is already running,
    // or fork fails.
    bool start(const Config& cfg);
    // SIGINT to the pipeline's process group, SIGKILL after a grace period (checked in poll()).
    void stop();
    // Call periodically (about once per second): reaps the child and refreshes stats.
    void poll();

    // "Keep streaming when the app closes". Takes effect even for a stream that is already
    // running, because the supervisor looks at this flag only when the app goes away.
    void set_keep_running(bool keep);
    // Re-attach to a stream left running by an earlier app instance. `cfg` is only used to mask
    // stream keys in log excerpts. Returns true if a live stream was found.
    bool attach(const Config& cfg);
    // Leave the running stream alone when this object is destroyed (the app is closing).
    void detach();

    EngineState state() const { return state_; }
    const EngineStats& stats() const { return stats_; }
    const std::string& error() const { return error_; }
    // Last lines of the pipeline log, for the UI error screen.
    std::string log_tail(int max_lines = 6) const;

private:
    void read_progress();
    void finish(int wait_status);
    bool process_alive() const;
    // Mic + system audio mixing: create the temporary PipeWire mix sink before the stream
    // starts and remove every module we loaded when it ends (also after a crash).
    bool setup_audio(const std::vector<std::string>& commands);
    void teardown_audio();

    PipelinePaths paths_;
    EngineState state_ = EngineState::Idle;
    EngineStats stats_;
    std::string error_;
    std::vector<std::string> secrets_;        // stream keys, masked out of log_tail()
    std::vector<std::string> audio_modules_;  // module ids to unload, in load order
    pid_t pid_ = -1;                          // the supervisor (also the process group id)
    unsigned long long start_ticks_ = 0;      // /proc start time, guards against pid reuse
    bool attached_ = false;                   // supervisor is not our child (attach())
    long started_at_ = 0;
    long stop_requested_at_ = 0;
    bool start_timed_out_ = false;  // gave up because no frames ever arrived
};

}  // namespace stream
