// Starts the opt-in setup page for a few seconds so it can be tried with curl (development aid).
//   web_demo [seconds]   prints the password on stdout and serves until it stops itself
#include "stream_config.h"
#include "web_config.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

int main(int argc, char** argv)
{
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 60;
    stream::Config cfg = stream::default_config();
    stream::WebConfigServer web;
    if (!web.start(cfg, [](const stream::Config& c) {
            std::printf("APPLY: %zu destinations, twitch enabled=%d\n", c.destinations.size(),
                        static_cast<int>(c.destinations[0].enabled));
            std::fflush(stdout);
        }, 8080, seconds)) {
        std::printf("start failed: %s\n", web.error().c_str());
        return 1;
    }
    std::printf("PASSWORD %s\nURL %s\n", web.password().c_str(), web.url().c_str());
    std::fflush(stdout);
    while (web.running()) {
        web.poll();
        ::usleep(100000);
    }
    std::printf("STOPPED %s\n", web.stop_reason().c_str());
    return 0;
}
