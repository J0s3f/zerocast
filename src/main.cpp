/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"
#include "stream_config.h"
#include "stream_engine.h"

#include <cstdio>
#include <string>
#include <unistd.h>

namespace {

// `zerocast --status` / `zerocast --stop` talk to a stream that was left running in the background
// ("Keep streaming when closed"), for example over SSH. They never open the UI.
int control_background_stream(const std::string& command) {
    stream::StreamEngine engine;
    stream::Config config;  // only used to mask stream keys in log excerpts
    if (!engine.attach(config)) {
        std::printf("No stream is running.\n");
        return command == "--stop" ? 0 : 1;
    }
    engine.poll();

    if (command == "--status") {
        const auto& s = engine.stats();
        std::printf("%s: %.0f fps, %.1f Mbit/s, up %ld s, dropped %ld\n", stream::state_name(engine.state()), s.fps,
                    s.bitrate_kbps / 1000.0, s.uptime_sec, s.dropped_frames);
        engine.detach();  // look, do not touch
        return 0;
    }

    engine.stop();
    for (int i = 0; i < 16 && engine.state() == stream::EngineState::Stopping; ++i) {
        ::usleep(500000);
        engine.poll();
    }
    const bool stopped = engine.state() == stream::EngineState::Idle;
    std::printf(stopped ? "Stream stopped.\n" : "Stream did not stop cleanly.\n");
    return stopped ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        const std::string arg = argv[1];
        if (arg == "--status" || arg == "--stop") {
            return control_background_stream(arg);
        }
        if (arg == "--supervise" && argc > 2) {
            // Internal: started by systemd-run for each stream; not meant to be typed.
            return stream::run_supervisor_job(argv[2]);
        }
        if (arg == "--help" || arg == "-h") {
            std::printf("zerocast            open the app\n"
                        "zerocast --status   show a stream left running in the background\n"
                        "zerocast --stop     stop a stream left running in the background\n");
            return 0;
        }
    }

    app::Application application;
    return application.run();
}
