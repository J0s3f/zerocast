/*
 * SPDX-License-Identifier: MIT
 */

#include "web_config.h"

#include "pipeline.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <sstream>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace stream {

namespace {

constexpr int kMaxFailedAttempts = 5;
constexpr long kStopAfterSaveSec = 30;
constexpr size_t kMaxRequestBytes = 16 * 1024;
constexpr size_t kMaxFieldLen = 256;

long now_sec()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long>(ts.tv_sec);
}

std::string random_text(const char* alphabet, size_t alphabet_len, size_t count)
{
    std::string out;
    FILE* f = std::fopen("/dev/urandom", "rb");
    unsigned char b = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!f || std::fread(&b, 1, 1, f) != 1)
            b = static_cast<unsigned char>(std::rand());  // only if /dev/urandom is unavailable
        out += alphabet[b % alphabet_len];
    }
    if (f)
        std::fclose(f);
    return out;
}

// Unambiguous characters only (no 0/O, 1/I/L), easy to read off a 320x170 screen.
std::string make_password()
{
    static const char kAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    return random_text(kAlphabet, sizeof(kAlphabet) - 1, 8);
}

std::string make_token()
{
    static const char kAlphabet[] = "0123456789abcdef";
    return random_text(kAlphabet, sizeof(kAlphabet) - 1, 24);
}

// Compare without leaking the match length through timing.
bool secure_equals(const std::string& a, const std::string& b)
{
    unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

std::string base64_decode(const std::string& in)
{
    static const std::string kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (char c : in) {
        if (c == '=')
            break;
        const size_t pos = kChars.find(c);
        if (pos == std::string::npos)
            continue;
        val = (val << 6) + static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            out += static_cast<char>((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

std::string url_decode(const std::string& in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out += static_cast<char>(std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

std::map<std::string, std::string> parse_form(const std::string& body)
{
    std::map<std::string, std::string> fields;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos)
            amp = body.size();
        const std::string pair = body.substr(pos, amp - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos)
            fields[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        pos = amp + 1;
    }
    return fields;
}

std::string html_escape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

// <option> elements for the USB cameras currently plugged in (plus a saved one that is not).
std::string usb_options(const std::string& selected)
{
    std::vector<UsbCamera> cams = list_usb_cameras();
    bool known = selected == "auto";
    for (const UsbCamera& c : cams)
        known = known || c.device == selected;
    if (!known)
        cams.push_back({selected, "not connected"});
    std::string out;
    for (const UsbCamera& c : cams) {
        out += "<option value=\"" + html_escape(c.device) + "\"" + (c.device == selected ? " selected" : "") + ">" +
               html_escape(c.name.empty() ? c.device : c.name + " (" + c.device + ")") + "</option>";
    }
    return out;
}

bool clean_text(const std::string& s)
{
    if (s.size() > kMaxFieldLen)
        return false;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f)
            return false;
    }
    return true;
}

std::string http_response(int code, const char* reason, const std::string& body,
                          const std::string& extra_headers = "")
{
    std::ostringstream out;
    out << "HTTP/1.1 " << code << ' ' << reason << "\r\n"
        << "Content-Type: text/html; charset=utf-8\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "X-Frame-Options: DENY\r\n"
        << "X-Content-Type-Options: nosniff\r\n"
        << "Referrer-Policy: no-referrer\r\n"
        << "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; form-action 'self'\r\n"
        << extra_headers << "Connection: close\r\n\r\n"
        << body;
    return out.str();
}

}  // namespace

WebConfigServer::WebConfigServer() = default;

WebConfigServer::~WebConfigServer()
{
    stop();
}

bool WebConfigServer::start(const Config& current, ApplyFn apply, int port, int ttl_sec)
{
    if (running())
        stop();
    error_.clear();
    stop_reason_.clear();

    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error_ = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 4) != 0) {
        error_ = std::string("port ") + std::to_string(port) + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }

    listen_fd_ = fd;
    port_ = port;
    config_ = current;
    apply_ = std::move(apply);
    password_ = make_password();
    csrf_ = make_token();
    failed_attempts_ = 0;
    locked_ = false;
    saved_ = false;
    expires_at_ = now_sec() + ttl_sec;
    return true;
}

void WebConfigServer::stop()
{
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        if (stop_reason_.empty())
            stop_reason_ = "stopped";
    }
    // Forget the secrets as soon as the window closes.
    password_.clear();
    csrf_.clear();
}

int WebConfigServer::seconds_left() const
{
    if (!running())
        return 0;
    return static_cast<int>(std::max(0L, expires_at_ - now_sec()));
}

std::string WebConfigServer::url() const
{
    std::string ip = "0.0.0.0";
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) == 0) {
        for (ifaddrs* it = list; it; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET)
                continue;
            if ((it->ifa_flags & IFF_LOOPBACK) || !(it->ifa_flags & IFF_UP))
                continue;
            char buf[INET_ADDRSTRLEN] = {0};
            const auto* sin = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
            if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
                ip = buf;
                break;
            }
        }
        ::freeifaddrs(list);
    }
    return "http://" + ip + ":" + std::to_string(port_);
}

void WebConfigServer::poll()
{
    if (!running())
        return;
    if (locked_) {
        stop_reason_ = "locked";
        stop();
        return;
    }
    if (now_sec() >= expires_at_) {
        stop_reason_ = saved_ ? "saved" : "expired";
        stop();
        return;
    }

    for (int served = 0; served < 4; ++served) {
        const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0)
            break;
        // A stalled client must not freeze the UI thread: short timeouts.
        timeval tv{1, 0};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        std::string raw;
        char buf[2048];
        size_t header_end = std::string::npos;
        size_t want = 0;
        while (raw.size() < kMaxRequestBytes) {
            const ssize_t n = ::recv(client, buf, sizeof(buf), 0);
            if (n <= 0)
                break;
            raw.append(buf, static_cast<size_t>(n));
            if (header_end == std::string::npos) {
                header_end = raw.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    std::string head = raw.substr(0, header_end);
                    std::transform(head.begin(), head.end(), head.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    const size_t cl = head.find("content-length:");
                    const size_t len = cl == std::string::npos
                                           ? 0
                                           : static_cast<size_t>(std::atol(head.c_str() + cl + 15));
                    want = header_end + 4 + std::min(len, kMaxRequestBytes);
                }
            }
            if (header_end != std::string::npos && raw.size() >= want)
                break;
        }

        if (!raw.empty()) {
            const std::string response = respond(raw);
            size_t off = 0;
            while (off < response.size()) {
                const ssize_t n = ::send(client, response.data() + off, response.size() - off, MSG_NOSIGNAL);
                if (n <= 0)
                    break;
                off += static_cast<size_t>(n);
            }
        }
        ::close(client);
        if (locked_)
            break;
    }
}

std::string WebConfigServer::respond(const std::string& raw)
{
    if (!running() && password_.empty())
        return http_response(503, "Service Unavailable", "<p>Setup window closed.</p>");

    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos)
        return http_response(400, "Bad Request", "<p>Bad request.</p>");

    std::istringstream head(raw.substr(0, header_end));
    std::string method, path, version;
    head >> method >> path >> version;
    std::map<std::string, std::string> headers;
    std::string line;
    std::getline(head, line);  // rest of request line
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        size_t v = colon + 1;
        while (v < line.size() && line[v] == ' ')
            ++v;
        headers[name] = line.substr(v);
    }

    if (locked_)
        return http_response(403, "Forbidden", "<p>Too many wrong passwords. Setup locked.</p>");

    // Authentication: HTTP Basic, any user name, the on-screen password.
    bool authed = false;
    const auto auth = headers.find("authorization");
    if (auth != headers.end() && auth->second.rfind("Basic ", 0) == 0) {
        const std::string decoded = base64_decode(auth->second.substr(6));
        const size_t colon = decoded.find(':');
        const std::string given = colon == std::string::npos ? decoded : decoded.substr(colon + 1);
        authed = secure_equals(given, password_);
        if (!authed && ++failed_attempts_ >= kMaxFailedAttempts)
            locked_ = true;
    }
    if (!authed) {
        return http_response(401, "Unauthorized",
                             "<p>Sign in with the user name and password shown on the Cardputer screen.</p>",
                             "WWW-Authenticate: Basic realm=\"ZeroCast setup\"\r\n");
    }

    if (method == "GET" && (path == "/" || path == "/index.html"))
        return http_response(200, "OK", render_page(""));

    if (method == "POST" && path == "/save") {
        const std::string body = raw.substr(header_end + 4);
        std::string notice;
        const bool ok = apply_form(body, &notice);
        return http_response(ok ? 200 : 400, ok ? "OK" : "Bad Request", render_page(notice));
    }
    return http_response(404, "Not Found", "<p>Not found.</p>");
}

bool WebConfigServer::apply_form(const std::string& body, std::string* notice)
{
    const auto f = parse_form(body);
    const auto get = [&](const std::string& k) {
        const auto it = f.find(k);
        return it == f.end() ? std::string() : it->second;
    };
    const auto has = [&](const std::string& k) { return f.find(k) != f.end(); };

    if (!secure_equals(get("csrf"), csrf_)) {
        *notice = "Rejected: the page expired. Reload it.";
        return false;
    }

    Config next = config_;
    for (size_t i = 0; i < next.destinations.size(); ++i) {
        Destination& d = next.destinations[i];
        const std::string p = "d" + std::to_string(i) + "_";
        d.enabled = has(p + "enabled");
        const std::string url = get(p + "url");
        const std::string key = get(p + "key");
        if (!clean_text(url) || !clean_text(key)) {
            *notice = "Rejected: invalid characters in " + d.name + ".";
            return false;
        }
        if (!url.empty()) {
            if (url.rfind("rtmp://", 0) != 0 && url.rfind("rtmps://", 0) != 0) {
                *notice = "Rejected: " + d.name + " URL must start with rtmp:// or rtmps://.";
                return false;
            }
            d.url = url;
        }
        if (has(p + "clear"))
            d.key.clear();
        else if (!key.empty())
            d.key = key;  // blank means "unchanged"
    }

    next.sources.video = get("video") == "screen" ? VideoSource::Screen
                         : get("video") == "usb"  ? VideoSource::Usb
                                                  : VideoSource::Camera;
    next.sources.screen = get("screen") == "hdmi" ? ScreenTarget::Hdmi : ScreenTarget::Lcd;
    next.sources.mic = has("mic");
    next.sources.system_audio = has("system");
    // One "flip" box: it applies to whichever camera is selected.
    if (next.sources.video == VideoSource::Usb)
        next.sources.usb_rotate_180 = has("flip");
    else
        next.sources.camera_rotate_180 = has("flip");
    if (get("usb_device") == "auto" || get("usb_device").rfind("/dev/video", 0) == 0)
        next.sources.usb_device = get("usb_device");
    next.sources.lcd_native = has("lcd_native");
    next.background_stream = has("bg");

    config_ = next;
    if (apply_)
        apply_(config_);
    saved_ = true;
    expires_at_ = std::min(expires_at_, now_sec() + kStopAfterSaveSec);
    *notice = "Saved. This setup page closes in " + std::to_string(kStopAfterSaveSec) + " seconds.";
    return true;
}

std::string WebConfigServer::render_page(const std::string& notice) const
{
    std::ostringstream h;
    h << "<!doctype html><html><head><meta charset=utf-8>"
         "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
         "<title>ZeroCast setup</title><style>"
         "body{font:16px system-ui,sans-serif;max-width:34em;margin:1em auto;padding:0 1em;background:#111;color:#eee}"
         "fieldset{border:1px solid #444;border-radius:8px;margin:1em 0}legend{padding:0 .4em}"
         "input[type=text],input[type=password],select{width:100%;box-sizing:border-box;padding:.5em;margin:.2em 0;"
         "background:#222;color:#eee;border:1px solid #555;border-radius:4px}"
         "label{display:block;margin:.3em 0}button{padding:.7em 1.4em;font-size:1em;border:0;border-radius:6px;"
         "background:#63e2b7;color:#000}.n{background:#333;padding:.6em;border-radius:6px}"
         "small{color:#aaa}</style></head><body><h1>ZeroCast setup</h1>";
    if (!notice.empty())
        h << "<p class=n>" << html_escape(notice) << "</p>";
    h << "<form method=post action=/save><input type=hidden name=csrf value=\"" << html_escape(csrf_) << "\">";

    h << "<fieldset><legend>Sources</legend>"
         "<label>Video <select name=video><option value=camera"
      << (config_.sources.video == VideoSource::Camera ? " selected" : "")
      << ">Camera</option><option value=usb"
      << (config_.sources.video == VideoSource::Usb ? " selected" : "") << ">USB camera</option><option value=screen"
      << (config_.sources.video == VideoSource::Screen ? " selected" : "") << ">Screen</option></select></label>"
      << "<label>USB camera <select name=usb_device><option value=auto"
      << (config_.sources.usb_device == "auto" ? " selected" : "") << ">Automatic (first one found)</option>"
      << usb_options(config_.sources.usb_device)
      << "</select></label>"
      << "<label>Screen <select name=screen><option value=lcd"
      << (config_.sources.screen == ScreenTarget::Lcd ? " selected" : "")
      << ">Built-in LCD</option><option value=hdmi"
      << (config_.sources.screen == ScreenTarget::Hdmi ? " selected" : "") << ">HDMI desktop</option></select></label>"
      << "<label><input type=checkbox name=mic" << (config_.sources.mic ? " checked" : "") << "> Microphone</label>"
      << "<label><input type=checkbox name=system" << (config_.sources.system_audio ? " checked" : "")
      << "> System audio</label>"
      << "<label><input type=checkbox name=flip" << ((config_.sources.video == VideoSource::Usb ? config_.sources.usb_rotate_180 : config_.sources.camera_rotate_180) ? " checked" : "")
      << "> Flip camera 180&deg;</label>"
      << "<label><input type=checkbox name=lcd_native" << (config_.sources.lcd_native ? " checked" : "")
      << "> LCD at native 320x170</label>"
      << "<label><input type=checkbox name=bg" << (config_.background_stream ? " checked" : "")
      << "> Keep streaming after ZeroCast is closed</label></fieldset>";

    for (size_t i = 0; i < config_.destinations.size(); ++i) {
        const Destination& d = config_.destinations[i];
        const std::string p = "d" + std::to_string(i) + "_";
        h << "<fieldset><legend>" << html_escape(d.name) << "</legend>"
          << "<label><input type=checkbox name=" << p << "enabled" << (d.enabled ? " checked" : "")
          << "> Stream to " << html_escape(d.name) << "</label>"
          << "<label>Server URL <input type=text name=" << p << "url value=\"" << html_escape(d.url) << "\"></label>"
          << "<label>Stream key <input type=password name=" << p << "key autocomplete=off placeholder=\""
          << (d.key.empty() ? "not set" : "set - leave blank to keep") << "\"></label>";
        if (!d.key.empty())
            h << "<label><small><input type=checkbox name=" << p << "clear> remove saved key</small></label>";
        h << "</fieldset>";
    }
    h << "<button type=submit>Save</button></form>"
         "<p><small>Saved keys are never shown here. This page switches itself off soon.</small></p>"
         "</body></html>";
    return h.str();
}

}  // namespace stream
