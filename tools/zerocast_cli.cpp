// Headless driver for the streaming core. Used for development and testing on the device.
//   zerocast-cli dry-run [config]               print the masked pipeline and any errors
//   zerocast-cli run     [config] [seconds]     start streaming, print stats, stop (stream dies with this process)
//   zerocast-cli start   [config]               start a stream that KEEPS RUNNING after this process exits
//   zerocast-cli status  [config]               attach to a running stream and print its stats
//   zerocast-cli stop                           stop a running stream
#include "stream_config.h"
#include "stream_engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static void print_stats(const stream::StreamEngine& engine, int index)
{
    const auto& s = engine.stats();
    std::printf("[%2ds] %-8s fps=%.1f bitrate=%dkbps speed=%.2fx dropped=%ld uptime=%lds\n", index,
                stream::state_name(engine.state()), s.fps, s.bitrate_kbps, s.speed, s.dropped_frames,
                s.uptime_sec);
    std::fflush(stdout);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s dry-run|run|start|status|stop [config] [seconds]\n", argv[0]);
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "--supervise" && argc > 2)
        return stream::run_supervisor_job(argv[2]);  // started by systemd-run for each stream
    const std::string path = argc > 2 ? argv[2] : "/etc/zerocast-stream.conf";

    stream::Config cfg = stream::default_config();
    if (!stream::load_config(path, &cfg) && (cmd == "dry-run" || cmd == "run" || cmd == "start"))
        std::fprintf(stderr, "note: %s not readable, using defaults\n", path.c_str());

    if (cmd == "dry-run") {
        stream::Pipeline p = stream::build_pipeline(cfg);
        for (const auto& e : p.errors)
            std::printf("ERROR: %s\n", e.c_str());
        for (const auto& w : p.warnings)
            std::printf("warning: %s\n", w.c_str());
        if (p.ok())
            std::printf("%s\n", p.shell_masked.c_str());
        return p.ok() ? 0 : 1;
    }

    if (cmd == "run") {
        const int seconds = argc > 3 ? std::atoi(argv[3]) : 15;
        stream::StreamEngine engine;
        if (!engine.start(cfg)) {
            std::fprintf(stderr, "start failed: %s\n", engine.error().c_str());
            return 1;
        }
        for (int i = 0; i < seconds; ++i) {
            ::sleep(1);
            engine.poll();
            print_stats(engine, i + 1);
            if (engine.state() == stream::EngineState::Failed)
                break;
        }
        engine.stop();
        for (int i = 0; i < 8 && engine.state() == stream::EngineState::Stopping; ++i) {
            ::sleep(1);
            engine.poll();
        }
        std::printf("final: %s\n", stream::state_name(engine.state()));
        if (engine.state() == stream::EngineState::Failed)
            std::printf("error: %s\n", engine.error().c_str());
        return engine.state() == stream::EngineState::Failed ? 1 : 0;
    }

    if (cmd == "start") {
        stream::StreamEngine engine;
        engine.set_keep_running(true);
        if (!engine.start(cfg)) {
            std::fprintf(stderr, "start failed: %s\n", engine.error().c_str());
            return 1;
        }
        engine.detach();  // leave it running when this process exits
        std::printf("started in the background\n");
        return 0;
    }

    if (cmd == "status" || cmd == "stop") {
        stream::StreamEngine engine;
        if (!engine.attach(cfg)) {
            std::printf("no stream is running\n");
            return cmd == "stop" ? 0 : 1;
        }
        engine.poll();
        if (cmd == "status") {
            print_stats(engine, 0);
            engine.detach();
            return 0;
        }
        engine.stop();
        for (int i = 0; i < 12 && engine.state() == stream::EngineState::Stopping; ++i) {
            ::usleep(500000);
            engine.poll();
        }
        std::printf("final: %s\n", stream::state_name(engine.state()));
        return engine.state() == stream::EngineState::Idle ? 0 : 1;
    }

    std::fprintf(stderr, "unknown command\n");
    return 2;
}
