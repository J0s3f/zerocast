/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>
#include <vector>

namespace stream {

// Camera = the CardputerZero's own (CSI) camera, Usb = a UVC webcam on the USB port.
enum class VideoSource { Camera, Screen, Usb };
// Which screen "Screen" means: the Cardputer's built-in LCD (default) or the HDMI/Wayland desktop.
enum class ScreenTarget { Lcd, Hdmi };
enum class DestinationKind { Twitch, YouTube, Restreamer, Custom };

struct Destination {
    std::string name;            // label shown in the UI
    DestinationKind kind = DestinationKind::Custom;
    std::string url;             // RTMP(S) base URL, no key
    std::string key;             // stream key (secret)
    bool enabled = false;
};

struct Sources {
    VideoSource video = VideoSource::Camera;  // default: camera
    bool mic = true;                          // default: microphone on
    bool system_audio = false;                // optional
    std::string mic_device = "@DEFAULT_SOURCE@";
    std::string system_device = "@DEFAULT_MONITOR@";
    // The CardputerZero camera is mounted upside down; flip it to an upright picture.
    bool camera_rotate_180 = true;
    // USB webcam: "auto" picks the first UVC capture node; the format is negotiated ("auto" prefers
    // MJPEG, which is what lets a USB 2.0 camera reach 720p30). USB cameras are mounted upright.
    std::string usb_device = "auto";
    std::string usb_format = "auto";  // mjpeg, yuyv422, nv12, yuv420p, uyvy422 or auto
    std::string usb_size = "auto";    // capture size "WxH", or auto (chosen from what the camera offers)
    int usb_fps = 0;                  // capture frame rate, 0 = auto
    bool usb_rotate_180 = false;
    ScreenTarget screen = ScreenTarget::Lcd;
    // LCD capture: framebuffer of the built-in display. "auto" finds it by driver name.
    std::string lcd_device = "auto";
    // false (default): exact 4x nearest-neighbour upscale (lossless), letterboxed into the
    // configured video size, which Twitch/YouTube players handle better.
    // true: send the LCD's own 320x170.
    bool lcd_native = false;
    // HDMI/desktop capture: Wayland socket of the desktop session. "auto" probes the
    // sockets in runtime_dir and picks one that accepts connections.
    std::string wayland_display = "auto";
    std::string runtime_dir = "/run/user/1000";
};

struct VideoSettings {
    int width = 1280;
    int height = 720;
    int fps = 25;
    int bitrate_kbps = 2500;
    int audio_bitrate_kbps = 128;
};

struct Config {
    Sources sources;
    VideoSettings video;
    // Keep streaming after the app is closed or killed (default off). Useful for streaming the
    // LCD while using other apps; reopening ZeroCast re-attaches to the running stream.
    bool background_stream = false;
    std::vector<Destination> destinations;
};

// Built-in presets: Twitch, YouTube, Restreamer and one Custom slot, all disabled.
Config default_config();

const char* kind_id(DestinationKind kind);
DestinationKind kind_from_id(const std::string& id);
const char* default_url(DestinationKind kind);

// Parse/serialize the INI format documented in DESIGN.md. Unknown keys are ignored.
Config parse_config(const std::string& text);
std::string serialize_config(const Config& cfg);

bool load_config(const std::string& path, Config* out);
// Writes atomically with mode 0600 (the file holds stream keys).
bool save_config(const std::string& path, const Config& cfg);

// "rtmp://host/app/key" -> "rtmp://host/app/****" for logs and UI.
std::string mask_key(const std::string& url, const std::string& key);

}  // namespace stream
