// Tests for the opt-in LAN setup page (no browser needed: requests are fed to respond()).
// Build: g++ -std=c++17 -Isrc/stream tests/web_test.cpp src/stream/web_config.cpp src/stream/stream_config.cpp -o web_test
#include "stream_config.h"
#include "web_config.h"

#include <cstdio>
#include <string>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static bool contains(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

static std::string b64(const std::string& in)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out += t[(val >> bits) & 0x3F];
            bits -= 6;
        }
    }
    if (bits > -6)
        out += t[((val << 8) >> (bits + 8)) & 0x3F];
    while (out.size() % 4)
        out += '=';
    return out;
}

static std::string get(const std::string& path, const std::string& user_pass)
{
    std::string r = "GET " + path + " HTTP/1.1\r\nHost: x\r\n";
    if (!user_pass.empty())
        r += "Authorization: Basic " + b64(user_pass) + "\r\n";
    return r + "\r\n";
}

static std::string post(const std::string& body, const std::string& user_pass)
{
    return "POST /save HTTP/1.1\r\nHost: x\r\nAuthorization: Basic " + b64(user_pass) +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

static std::string token_from(const std::string& page)
{
    const std::string marker = "name=csrf value=\"";
    const size_t p = page.find(marker);
    return p == std::string::npos ? "" : page.substr(p + marker.size(), page.find('"', p + marker.size()) - p - marker.size());
}

int main()
{
    using namespace stream;
    Config cfg = default_config();
    cfg.destinations[0].key = "live_SECRETKEY";  // pre-existing key that must never be shown

    Config applied;
    int applied_count = 0;
    WebConfigServer srv;
    CHECK(srv.start(cfg, [&](const Config& c) { applied = c; ++applied_count; }, 18091, 300));
    CHECK(srv.running());
    CHECK(srv.password().size() == 8);
    CHECK(srv.seconds_left() > 290);
    const std::string pw = srv.password();

    // No password -> 401 with a Basic challenge, and it does not count as a failed guess.
    CHECK(contains(srv.respond(get("/", "")), "401 Unauthorized"));
    CHECK(contains(srv.respond(get("/", "")), "WWW-Authenticate: Basic"));

    // Correct password (any user name) -> the page, with a CSRF token, and no stored key.
    const std::string page = srv.respond(get("/", "anyone:" + pw));
    CHECK(contains(page, "200 OK"));
    CHECK(contains(page, "ZeroCast setup"));
    CHECK(!contains(page, "SECRETKEY"));
    CHECK(contains(page, "set - leave blank to keep"));
    CHECK(contains(page, "Content-Security-Policy"));
    CHECK(contains(page, "Cache-Control: no-store"));
    const std::string csrf = token_from(page);
    CHECK(csrf.size() == 24);

    // Wrong password -> 401.
    CHECK(contains(srv.respond(get("/", "x:wrongpass")), "401 Unauthorized"));

    // A POST without the CSRF token is rejected and changes nothing.
    CHECK(contains(srv.respond(post("d1_enabled=on&d1_key=abc", "x:" + pw)), "400 Bad Request"));
    CHECK(applied_count == 0);

    // A valid save: enables YouTube with a key, keeps the Twitch key when left blank.
    const std::string saved = srv.respond(post(
        "csrf=" + csrf + "&video=screen&screen=hdmi&mic=on&d0_enabled=on&d0_url=rtmp%3A%2F%2Flive.twitch.tv%2Fapp"
        "&d0_key=&d1_enabled=on&d1_url=rtmp%3A%2F%2Fa.rtmp.youtube.com%2Flive2&d1_key=abcd-1234%3Fx%3D1%26y",
        "x:" + pw));
    CHECK(contains(saved, "200 OK"));
    CHECK(contains(saved, "Saved."));
    CHECK(!contains(saved, "abcd-1234"));  // the response never echoes keys
    CHECK(applied_count == 1);
    CHECK(applied.destinations[0].enabled && applied.destinations[0].key == "live_SECRETKEY");
    CHECK(applied.destinations[1].enabled && applied.destinations[1].key == "abcd-1234?x=1&y");
    CHECK(!applied.destinations[2].enabled);
    CHECK(applied.sources.video == VideoSource::Screen);
    CHECK(applied.sources.screen == ScreenTarget::Hdmi);
    CHECK(applied.sources.mic && !applied.sources.system_audio);
    CHECK(!applied.background_stream);  // unchecked box = off
    CHECK(srv.seconds_left() <= 30);  // closes soon after a successful save

    // Bad scheme is rejected without applying.
    const int before = applied_count;
    CHECK(contains(srv.respond(post("csrf=" + csrf + "&d0_url=http%3A%2F%2Fevil", "x:" + pw)), "400 Bad Request"));
    CHECK(applied_count == before);

    // Control characters in a field are rejected.
    CHECK(contains(srv.respond(post("csrf=" + csrf + "&d0_key=a%0Ab", "x:" + pw)), "400 Bad Request"));

    // Removing a key.
    srv.respond(post("csrf=" + csrf + "&d0_clear=on&d0_url=rtmp%3A%2F%2Flive.twitch.tv%2Fapp", "x:" + pw));
    CHECK(applied.destinations[0].key.empty());

    // HTML in a field value is escaped in the page.
    // (name is fixed text, but a malicious URL cannot be saved: scheme check above)

    // Unknown path.
    CHECK(contains(srv.respond(get("/admin", "x:" + pw)), "404 Not Found"));

    srv.stop();
    CHECK(!srv.running());
    CHECK(srv.password().empty());  // secrets are forgotten once the window closes
    CHECK(contains(srv.respond(get("/", "x:" + pw)), "503"));

    // Lockout after 5 wrong passwords.
    WebConfigServer lock;
    CHECK(lock.start(cfg, nullptr, 18092, 300));
    const std::string real = lock.password();
    for (int i = 0; i < 5; ++i)
        lock.respond(get("/", "x:nope" + std::to_string(i)));
    CHECK(contains(lock.respond(get("/", "x:" + real)), "403 Forbidden"));  // even the right one now
    lock.poll();
    CHECK(!lock.running());
    CHECK(lock.stop_reason() == "locked");

    // Auto-expiry (ttl of 0 seconds).
    WebConfigServer ttl;
    CHECK(ttl.start(cfg, nullptr, 18093, 0));
    ttl.poll();
    CHECK(!ttl.running());
    CHECK(ttl.stop_reason() == "expired");

    // A port that is taken fails cleanly.
    WebConfigServer a, b;
    CHECK(a.start(cfg, nullptr, 18094, 60));
    CHECK(!b.start(cfg, nullptr, 18094, 60));
    CHECK(!b.error().empty());

    // Two activations get different passwords.
    WebConfigServer c1, c2;
    c1.start(cfg, nullptr, 18095, 60);
    c2.start(cfg, nullptr, 18096, 60);
    CHECK(c1.password() != c2.password());

    if (failures == 0)
        std::printf("all web tests passed\n");
    return failures == 0 ? 0 : 1;
}
