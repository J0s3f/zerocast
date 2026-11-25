/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "stream_config.h"

#include <string>
#include <vector>

namespace stream {

struct Pipeline {
    // Full command for `/bin/sh -c`. Contains stream keys: never log it directly.
    std::string shell;
    // Same command with keys masked, safe for logs and the UI.
    std::string shell_masked;
    // Problems that make the config unstartable (no destination, missing key, ...).
    std::vector<std::string> errors;
    // Non-fatal notes (e.g. "no audio selected, using silence").
    std::vector<std::string> warnings;
    // Commands to run before starting (mic + system audio mixing). Each prints a PipeWire
    // module id; the engine unloads those modules when the stream stops.
    std::vector<std::string> audio_setup;
    int destination_count = 0;

    bool ok() const { return errors.empty() && !shell.empty(); }
};

// A private (mode 0700, owned by us) directory for runtime files: /run/zerocast as root,
// $XDG_RUNTIME_DIR/zerocast otherwise, else /tmp/zerocast-<uid>. Created on demand. The state
// file decides what the engine kills or re-attaches to, so it must not be in a shared directory.
std::string default_runtime_dir();

// Files used by a running stream. The default constructor puts them in default_runtime_dir().
struct PipelinePaths {
    std::string progress_file;  // ffmpeg -progress output (stats)
    std::string log_file;       // pipeline stderr (may echo URLs: kept 0600, keys masked in the UI)
    std::string state_file;     // pid, start time, audio modules: lets a new app instance re-attach
    std::string keep_file;      // exists while "keep streaming when the app closes" is on

    PipelinePaths();
    explicit PipelinePaths(const std::string& dir);
};

Pipeline build_pipeline(const Config& cfg, const PipelinePaths& paths = PipelinePaths());

// Returns the first wayland-N socket in runtime_dir that accepts a connection
// (stale sockets are skipped), or "" if none. Used to resolve wayland_display=auto.
std::string detect_wayland_display(const std::string& runtime_dir);

// Finds the first USB (UVC) camera capture node, e.g. "/dev/video2", by looking at /sys/class/video4linux
// for nodes that sit on the USB bus and are the capture node (index 0; a UVC camera also exposes a
// metadata node). "" if none. `name` receives the product name when found.
std::string detect_usb_camera(std::string* name = nullptr);

// Every USB (UVC) camera capture node that is plugged in, in /dev/videoN order.
struct UsbCamera {
    std::string device;  // "/dev/video2"
    std::string name;    // product name, may be empty
};
std::vector<UsbCamera> list_usb_cameras();

// One thing a camera can deliver: a pixel format (ffmpeg name: mjpeg, yuyv422, nv12, ...), a frame
// size and its best frame rate (0 = unknown).
struct CaptureMode {
    std::string format;
    int width = 0;
    int height = 0;
    double max_fps = 0.0;
};

// Parses `v4l2-ctl --list-formats-ext` output. Formats ffmpeg cannot use reliably are skipped.
std::vector<CaptureMode> parse_v4l2_modes(const std::string& v4l2ctl_output);

// Asks the camera what it can do (v4l2-ctl, falling back to ffmpeg's format list without rates).
std::vector<CaptureMode> probe_usb_modes(const std::string& device);

struct CaptureChoice {
    bool found = false;
    std::string format;
    int width = 0;
    int height = 0;
    int fps = 0;
};

// Chooses what to request from the camera for an output of out_w x out_h at fps. Prefers a mode that
// reaches the frame rate at a size that covers the output (smallest such picture, MJPEG before raw);
// otherwise the largest picture that reaches the rate; otherwise the fastest the camera has.
// only_format restricts the choice: "" or "auto" = any, "raw" = any uncompressed format, or a specific
// ffmpeg format name such as "mjpeg" or "yuyv422".
CaptureChoice choose_capture_mode(const std::vector<CaptureMode>& modes, int out_w, int out_h, int fps,
                                  const std::string& only_format = "auto");

// PulseAudio/PipeWire input sources that can be used as a microphone (monitors excluded), by name.
std::vector<std::string> list_audio_inputs();

// Finds the built-in LCD framebuffer (/dev/fbN) by driver name in /sys/class/graphics
// (panel-mipi-dbid on the CardputerZero, fb_st7789v on older images). "" if not found.
std::string detect_lcd_framebuffer();

// POSIX single-quote escaping: abc'd -> 'abc'\''d'
std::string shell_quote(const std::string& s);

}  // namespace stream
