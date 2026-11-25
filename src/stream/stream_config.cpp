/*
 * SPDX-License-Identifier: MIT
 */

#include "stream_config.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace stream {

namespace {

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool to_bool(const std::string& v, bool fallback)
{
    if (v == "1" || v == "yes" || v == "true" || v == "on")
        return true;
    if (v == "0" || v == "no" || v == "false" || v == "off")
        return false;
    return fallback;
}

int to_int(const std::string& v, int fallback, int lo, int hi)
{
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0')
        return fallback;
    return static_cast<int>(std::min<long>(hi, std::max<long>(lo, n)));
}

Destination preset(const char* name, DestinationKind kind)
{
    Destination d;
    d.name = name;
    d.kind = kind;
    d.url = default_url(kind);
    return d;
}

}  // namespace

const char* kind_id(DestinationKind kind)
{
    switch (kind) {
    case DestinationKind::Twitch: return "twitch";
    case DestinationKind::YouTube: return "youtube";
    case DestinationKind::Restreamer: return "restreamer";
    case DestinationKind::Custom: break;
    }
    return "custom";
}

DestinationKind kind_from_id(const std::string& id)
{
    if (id == "twitch") return DestinationKind::Twitch;
    if (id == "youtube") return DestinationKind::YouTube;
    if (id == "restreamer") return DestinationKind::Restreamer;
    return DestinationKind::Custom;
}

const char* default_url(DestinationKind kind)
{
    switch (kind) {
    case DestinationKind::Twitch: return "rtmp://live.twitch.tv/app";
    case DestinationKind::YouTube: return "rtmp://a.rtmp.youtube.com/live2";
    case DestinationKind::Restreamer: return "rtmp://restreamer.local:1935/live";
    case DestinationKind::Custom: break;
    }
    return "rtmp://example.invalid/live";
}

Config default_config()
{
    Config cfg;
    cfg.destinations = {
        preset("Twitch", DestinationKind::Twitch),
        preset("YouTube", DestinationKind::YouTube),
        preset("Restreamer", DestinationKind::Restreamer),
        preset("Custom", DestinationKind::Custom),
    };
    return cfg;
}

Config parse_config(const std::string& text)
{
    Config cfg = default_config();
    // Section name -> key/value pairs, order preserved for destinations.
    std::string section;
    std::map<std::string, Destination*> by_id;
    std::vector<std::pair<std::string, Destination>> extra;

    auto dest_for = [&](const std::string& id) -> Destination* {
        auto it = by_id.find(id);
        if (it != by_id.end())
            return it->second;
        for (auto& d : cfg.destinations) {
            if (id == kind_id(d.kind) || id == d.name) {
                by_id[id] = &d;
                return &d;
            }
        }
        // Unknown id: add a custom destination so nothing the user wrote is lost.
        Destination d;
        d.name = id;
        d.kind = DestinationKind::Custom;
        cfg.destinations.push_back(d);
        // Pointers into the vector may be invalidated by push_back.
        by_id.clear();
        return &cfg.destinations.back();
    };

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));

        if (section == "sources") {
            if (k == "video") cfg.sources.video = v == "screen" ? VideoSource::Screen : v == "usb" ? VideoSource::Usb : VideoSource::Camera;
            else if (k == "usb_device" && !v.empty()) cfg.sources.usb_device = v;
            else if (k == "usb_format" && !v.empty()) cfg.sources.usb_format = v;
            else if (k == "usb_size" && !v.empty()) cfg.sources.usb_size = v;
            else if (k == "usb_fps") cfg.sources.usb_fps = to_int(v, 0, 0, 120);
            else if (k == "usb_rotate_180") cfg.sources.usb_rotate_180 = to_bool(v, cfg.sources.usb_rotate_180);
            else if (k == "mic") cfg.sources.mic = to_bool(v, cfg.sources.mic);
            else if (k == "system_audio") cfg.sources.system_audio = to_bool(v, cfg.sources.system_audio);
            else if (k == "mic_device" && !v.empty()) cfg.sources.mic_device = v;
            else if (k == "system_device" && !v.empty()) cfg.sources.system_device = v;
            else if (k == "camera_rotate_180") cfg.sources.camera_rotate_180 = to_bool(v, cfg.sources.camera_rotate_180);
            else if (k == "screen") cfg.sources.screen = (v == "hdmi") ? ScreenTarget::Hdmi : ScreenTarget::Lcd;
            else if (k == "lcd_device" && !v.empty()) cfg.sources.lcd_device = v;
            else if (k == "lcd_native") cfg.sources.lcd_native = to_bool(v, cfg.sources.lcd_native);
            else if (k == "wayland_display" && !v.empty()) cfg.sources.wayland_display = v;
            else if (k == "runtime_dir" && !v.empty()) cfg.sources.runtime_dir = v;
        } else if (section == "app") {
            if (k == "background_stream") cfg.background_stream = to_bool(v, cfg.background_stream);
        } else if (section == "video") {
            if (k == "width") cfg.video.width = to_int(v, cfg.video.width, 320, 1920);
            else if (k == "height") cfg.video.height = to_int(v, cfg.video.height, 240, 1080);
            else if (k == "fps") cfg.video.fps = to_int(v, cfg.video.fps, 5, 60);
            else if (k == "bitrate_kbps") cfg.video.bitrate_kbps = to_int(v, cfg.video.bitrate_kbps, 300, 12000);
            else if (k == "audio_bitrate_kbps") cfg.video.audio_bitrate_kbps = to_int(v, cfg.video.audio_bitrate_kbps, 32, 320);
        } else if (section.rfind("destination.", 0) == 0) {
            std::string id = section.substr(std::string("destination.").size());
            Destination* d = dest_for(id);
            if (k == "name" && !v.empty()) d->name = v;
            else if (k == "kind") d->kind = kind_from_id(v);
            else if (k == "url") d->url = v;
            else if (k == "key") d->key = v;
            else if (k == "enabled") d->enabled = to_bool(v, d->enabled);
        }
    }
    return cfg;
}

std::string serialize_config(const Config& cfg)
{
    std::ostringstream out;
    out << "# ZeroCast configuration. Contains stream keys: keep this file private (mode 0600).\n";
    out << "[sources]\n";
    out << "video = "
        << (cfg.sources.video == VideoSource::Screen ? "screen"
            : cfg.sources.video == VideoSource::Usb  ? "usb"
                                                     : "camera")
        << "\n";
    out << "usb_device = " << cfg.sources.usb_device << "\n";
    out << "usb_format = " << cfg.sources.usb_format << "\n";
    out << "usb_size = " << cfg.sources.usb_size << "\n";
    out << "usb_fps = " << cfg.sources.usb_fps << "\n";
    out << "usb_rotate_180 = " << (cfg.sources.usb_rotate_180 ? "yes" : "no") << "\n";
    out << "mic = " << (cfg.sources.mic ? "yes" : "no") << "\n";
    out << "system_audio = " << (cfg.sources.system_audio ? "yes" : "no") << "\n";
    out << "mic_device = " << cfg.sources.mic_device << "\n";
    out << "system_device = " << cfg.sources.system_device << "\n";
    out << "camera_rotate_180 = " << (cfg.sources.camera_rotate_180 ? "yes" : "no") << "\n";
    out << "screen = " << (cfg.sources.screen == ScreenTarget::Hdmi ? "hdmi" : "lcd") << "\n";
    out << "lcd_device = " << cfg.sources.lcd_device << "\n";
    out << "lcd_native = " << (cfg.sources.lcd_native ? "yes" : "no") << "\n";
    out << "wayland_display = " << cfg.sources.wayland_display << "\n";
    out << "runtime_dir = " << cfg.sources.runtime_dir << "\n\n";
    out << "[video]\n";
    out << "width = " << cfg.video.width << "\n";
    out << "height = " << cfg.video.height << "\n";
    out << "fps = " << cfg.video.fps << "\n";
    out << "bitrate_kbps = " << cfg.video.bitrate_kbps << "\n";
    out << "audio_bitrate_kbps = " << cfg.video.audio_bitrate_kbps << "\n\n";
    out << "[app]\n";
    out << "background_stream = " << (cfg.background_stream ? "yes" : "no") << "\n\n";
    int custom_index = 0;
    for (const auto& d : cfg.destinations) {
        std::string id = kind_id(d.kind);
        if (d.kind == DestinationKind::Custom)
            id = custom_index++ == 0 ? "custom" : d.name;
        out << "[destination." << id << "]\n";
        out << "name = " << d.name << "\n";
        out << "kind = " << kind_id(d.kind) << "\n";
        out << "url = " << d.url << "\n";
        out << "key = " << d.key << "\n";
        out << "enabled = " << (d.enabled ? "yes" : "no") << "\n\n";
    }
    return out.str();
}

bool load_config(const std::string& path, Config* out)
{
    std::ifstream f(path);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *out = parse_config(ss.str());
    return true;
}

bool save_config(const std::string& path, const Config& cfg)
{
    const std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    const std::string text = serialize_config(cfg);
    size_t off = 0;
    while (off < text.size()) {
        ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        off += static_cast<size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
    ::chmod(tmp.c_str(), 0600);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::string mask_key(const std::string& url, const std::string& key)
{
    std::string base = url;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    return key.empty() ? base : base + "/****";
}

}  // namespace stream
