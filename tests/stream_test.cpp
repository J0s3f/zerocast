// Plain-assert tests for the streaming core (no LVGL, no hardware).
// Build: g++ -std=c++17 -Isrc/stream tests/stream_test.cpp src/stream/stream_config.cpp src/stream/pipeline.cpp -o stream_test
#include "pipeline.h"
#include "stream_config.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

static stream::Config with_twitch()
{
    stream::Config c = stream::default_config();
    c.destinations[0].enabled = true;
    c.destinations[0].key = "live_123_SECRET";
    return c;
}

int main()
{
    using namespace stream;

    // Defaults: camera + mic on, screen + system audio off, camera flipped upright,
    // screen means the built-in LCD (scaled), not HDMI.
    {
        Config c = default_config();
        CHECK(c.sources.video == VideoSource::Camera);
        CHECK(c.sources.mic);
        CHECK(!c.sources.system_audio);
        CHECK(c.sources.camera_rotate_180);
        CHECK(c.sources.screen == ScreenTarget::Lcd);
        CHECK(!c.sources.lcd_native);
        CHECK(!c.background_stream);  // background streaming is opt-in
        CHECK(c.destinations.size() == 4);
    }

    // Nothing enabled -> error.
    {
        Pipeline p = build_pipeline(default_config());
        CHECK(!p.ok());
        CHECK(p.errors.size() == 1);
    }

    // Enabled but no key -> error naming the destination.
    {
        Config c = default_config();
        c.destinations[1].enabled = true;
        Pipeline p = build_pipeline(c);
        CHECK(!p.ok());
        CHECK(contains(p.errors.front(), "YouTube"));
    }

    // Bad scheme -> error.
    {
        Config c = with_twitch();
        c.destinations[0].url = "http://live.twitch.tv/app";
        CHECK(!build_pipeline(c).ok());
    }

    // Default: camera + mic. The camera process captures the mic itself (libav MPEG-TS with
    // sensor timestamps); the consumer ffmpeg only copies packets.
    {
        Pipeline p = build_pipeline(with_twitch());
        CHECK(p.ok());
        CHECK(p.destination_count == 1);
        CHECK(contains(p.shell, "rpicam-vid"));
        CHECK(contains(p.shell, "--codec libav --libav-format mpegts"));
        CHECK(contains(p.shell, "--libav-video-codec h264_v4l2m2m"));  // hardware encoder
        CHECK(contains(p.shell, "--hflip --vflip"));
        CHECK(contains(p.shell, "--libav-audio --audio-codec aac --audio-source pulse"));
        CHECK(contains(p.shell, "--audio-device '@DEFAULT_SOURCE@'"));
        CHECK(!contains(p.shell, "wf-recorder"));
        CHECK(!contains(p.shell, "libx264"));  // never software-encode video
        CHECK(!contains(p.shell, "-f pulse"));  // no real-time audio input in the consumer
        CHECK(!contains(p.shell, "amix"));
        CHECK(contains(p.shell, "-f mpegts -i -"));
        CHECK(contains(p.shell, "-map 0:v -map 0:a -c copy"));
        CHECK(contains(p.shell, "-probesize 200000 -analyzeduration 500000"));
        CHECK(!contains(p.shell, " -framerate "));  // ffmpeg's option; rpicam-vid's is --framerate
        CHECK(!contains(p.shell, "use_wallclock"));
        CHECK(p.audio_setup.empty());
        CHECK(contains(p.shell, "rtmp://live.twitch.tv/app/live_123_SECRET"));
        // Keys must be masked in the loggable form.
        CHECK(!contains(p.shell_masked, "SECRET"));
        CHECK(contains(p.shell_masked, "rtmp://live.twitch.tv/app/****"));
    }

    // Camera flip can be turned off.
    {
        Config c = with_twitch();
        c.sources.camera_rotate_180 = false;
        CHECK(!contains(build_pipeline(c).shell, "--hflip"));
    }

    // Camera with system audio only (mic off): record the monitor source directly.
    {
        Config c = with_twitch();
        c.sources.mic = false;
        c.sources.system_audio = true;
        Pipeline p = build_pipeline(c);
        CHECK(p.ok());
        CHECK(contains(p.shell, "--audio-device '@DEFAULT_MONITOR@'"));
        CHECK(!contains(p.shell, "'@DEFAULT_SOURCE@'"));
        CHECK(p.audio_setup.empty());
    }

    // Mic + system audio: mixed in a temporary PipeWire sink, and the camera records its monitor.
    {
        Config c = with_twitch();
        c.sources.system_audio = true;
        Pipeline p = build_pipeline(c);
        CHECK(p.ok());
        CHECK(contains(p.shell, "--audio-device 'zerocast_mix.monitor'"));
        CHECK(p.audio_setup.size() == 3);
        CHECK(contains(p.audio_setup[0], "module-null-sink sink_name=zerocast_mix"));
        CHECK(contains(p.audio_setup[1], "module-loopback source='@DEFAULT_SOURCE@' sink=zerocast_mix"));
        CHECK(contains(p.audio_setup[2], "module-loopback source='@DEFAULT_MONITOR@' sink=zerocast_mix"));
    }

    // No audio at all -> silent track in the consumer (not real time, so it cannot stall) + warning.
    {
        Config c = with_twitch();
        c.sources.mic = false;
        Pipeline p = build_pipeline(c);
        CHECK(p.ok());
        CHECK(contains(p.shell, "anullsrc"));
        CHECK(contains(p.shell, "-map 1:a -c:v copy -c:a aac"));
        CHECK(!contains(p.shell, "--libav-audio"));
        CHECK(!p.warnings.empty());
    }

    // Screen defaults to the built-in LCD (framebuffer), letterboxed 320x170 -> 1280x720,
    // encoded with the hardware encoder; the mic is captured inside the LCD ffmpeg.
    {
        Config c = with_twitch();
        c.sources.video = VideoSource::Screen;
        c.sources.lcd_device = "/dev/fb0";
        Pipeline p = build_pipeline(c);
        CHECK(p.ok());
        CHECK(contains(p.shell, "-f fbdev"));
        CHECK(contains(p.shell, "'/dev/fb0'"));
        CHECK(contains(p.shell, "scale=1280:680:flags=neighbor,pad=1280:720:0:20"));
        CHECK(contains(p.shell, "-c:v h264_v4l2m2m"));
        CHECK(contains(p.shell, "-fps_mode cfr -r 25"));  // static screens still need a steady rate
        CHECK(contains(p.shell, "-f pulse -i '@DEFAULT_SOURCE@'"));
        CHECK(contains(p.shell, "-c:a aac"));
        CHECK(!contains(p.shell, "wf-recorder"));
        CHECK(!contains(p.shell, "rpicam-vid"));
        CHECK(!contains(p.shell, "--hflip"));
        CHECK(contains(p.shell, "-map 0:v -map 0:a -c copy"));
    }

    // USB webcam: V4L2 capture (MJPEG), hardware H.264, mic captured inside the producer.
    {
        Config c = with_twitch();
        c.sources.video = VideoSource::Usb;
        c.sources.usb_device = "/dev/video2";
        c.sources.usb_format = "mjpeg";
        Pipeline p = build_pipeline(c);
        CHECK(p.ok());
        CHECK(contains(p.shell, "-f v4l2 -input_format 'mjpeg'"));
        CHECK(contains(p.shell, "-framerate 25 -video_size 1280x720 -i '/dev/video2'"));
        CHECK(contains(p.shell, "force_original_aspect_ratio=decrease,pad=1280:720"));
        CHECK(contains(p.shell, "-c:v h264_v4l2m2m"));  // hardware, never software
        CHECK(!contains(p.shell, "libx264"));
        CHECK(contains(p.shell, "-fps_mode cfr -r 25"));
        CHECK(contains(p.shell, "-f pulse -i '@DEFAULT_SOURCE@'"));  // mic inside the producer
        CHECK(!contains(p.shell, "rpicam-vid"));
        CHECK(!contains(p.shell, "hflip"));  // USB cameras are not flipped by default
        CHECK(contains(p.shell, "-map 0:v -map 0:a -c copy"));
        c.sources.usb_rotate_180 = true;
        CHECK(contains(build_pipeline(c).shell, "hflip,vflip,scale="));
        c.sources.usb_format = "auto";  // unresolved auto: let ffmpeg choose, no -input_format
        CHECK(!contains(build_pipeline(c).shell, "-input_format"));
        Config r = parse_config(serialize_config(c));
        CHECK(r.sources.video == VideoSource::Usb);
        CHECK(r.sources.usb_device == "/dev/video2");
        CHECK(r.sources.usb_rotate_180);
    }

    // Different cameras: parse what v4l2-ctl lists and pick a sensible mode for each.
    {
        // Camera A: MJPEG up to 1080p30, raw YUYV only slow at 720p (like the AUKEY PC-W3S).
        const std::string a = R"(ioctl: VIDIOC_ENUM_FMT
	Type: Video Capture

	[0]: 'YUYV' (YUYV 4:2:2)
		Size: Discrete 640x480
			Interval: Discrete 0.033s (30.000 fps)
		Size: Discrete 1280x720
			Interval: Discrete 0.100s (10.000 fps)
	[1]: 'MJPG' (Motion-JPEG, compressed)
		Size: Discrete 640x480
			Interval: Discrete 0.033s (30.000 fps)
		Size: Discrete 1280x720
			Interval: Discrete 0.033s (30.000 fps)
			Interval: Discrete 0.040s (25.000 fps)
		Size: Discrete 1920x1080
			Interval: Discrete 0.033s (30.000 fps)
)";
        auto ma = parse_v4l2_modes(a);
        CHECK(ma.size() == 5);
        CHECK(ma[0].format == "yuyv422" && ma[0].width == 640 && ma[0].max_fps > 29.0);
        CHECK(ma[4].format == "mjpeg" && ma[4].height == 1080);
        CaptureChoice ca = choose_capture_mode(ma, 1280, 720, 25);
        CHECK(ca.found && ca.format == "mjpeg" && ca.width == 1280 && ca.height == 720 && ca.fps == 25);
        // Output 640x360: the smallest picture that covers it at the rate is 640x480 MJPEG (raw ties lose).
        CaptureChoice cs = choose_capture_mode(ma, 640, 360, 25);
        CHECK(cs.format == "mjpeg" && cs.width == 640 && cs.height == 480);
        // Forcing a raw format picks from that format only; 720p raw is too slow, so the 640x480 one wins.
        CaptureChoice cr = choose_capture_mode(ma, 1280, 720, 25, "yuyv422");
        CHECK(cr.format == "yuyv422" && cr.width == 640 && cr.fps == 25);

        // "raw" means any uncompressed format; "mjpeg" forces compressed.
        CaptureChoice cw = choose_capture_mode(ma, 1280, 720, 25, "raw");
        CHECK(cw.format == "yuyv422" && cw.width == 640);
        CHECK(choose_capture_mode(ma, 1280, 720, 25, "mjpeg").format == "mjpeg");

        // Camera B: a cheap raw-only 640x480 camera. It cannot cover 720p, so use its largest picture.
        const std::string b = R"(	[0]: 'YUYV' (YUYV 4:2:2)
		Size: Discrete 640x480
			Interval: Discrete 0.033s (30.000 fps)
		Size: Discrete 320x240
			Interval: Discrete 0.033s (30.000 fps)
	[1]: 'H264' (H.264, compressed)
		Size: Discrete 1920x1080
			Interval: Discrete 0.033s (30.000 fps)
)";
        auto mb = parse_v4l2_modes(b);
        CHECK(mb.size() == 2);  // the H.264 format is skipped
        CaptureChoice cb = choose_capture_mode(mb, 1280, 720, 25);
        CHECK(cb.found && cb.format == "yuyv422" && cb.width == 640 && cb.height == 480 && cb.fps == 25);

        // Camera C: cannot reach the requested rate: use the camera's own rate, never an invalid one.
        const std::string c = R"(	[0]: 'MJPG' (Motion-JPEG, compressed)
		Size: Discrete 1280x720
			Interval: Discrete 0.067s (15.000 fps)
)";
        CaptureChoice cc = choose_capture_mode(parse_v4l2_modes(c), 1280, 720, 30);
        CHECK(cc.found && cc.fps == 15);

        // Stepwise sizes (some drivers): treated as the largest size; no modes finds nothing.
        auto md = parse_v4l2_modes(R"(	[0]: 'MJPG' (Motion-JPEG)
		Size: Stepwise 160x120 - 1920x1080 with step 16/16
			Interval: Stepwise 0.033s - 1.000s with step 0.033s
)");
        CHECK(md.size() == 1 && md[0].width == 1920 && md[0].max_fps > 29.0);
        CHECK(!choose_capture_mode({}, 1280, 720, 25).found);

        // The chosen mode ends up in the pipeline and survives the config file.
        Config cfg = with_twitch();
        cfg.sources.video = VideoSource::Usb;
        cfg.sources.usb_device = "/dev/video4";
        cfg.sources.usb_format = cb.format;
        cfg.sources.usb_size = "640x480";
        cfg.sources.usb_fps = cb.fps;
        Pipeline p = build_pipeline(cfg);
        CHECK(contains(p.shell, "-input_format 'yuyv422' -framerate 25 -video_size 640x480 -i '/dev/video4'"));
        CHECK(contains(p.shell, "scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720"));
        Config r = parse_config(serialize_config(cfg));
        CHECK(r.sources.usb_size == "640x480" && r.sources.usb_fps == 25 && r.sources.usb_format == "yuyv422");
    }

    // Native LCD resolution: no scaling or padding.
    {
        Config c = with_twitch();
        c.sources.video = VideoSource::Screen;
        c.sources.lcd_device = "/dev/fb0";
        c.sources.lcd_native = true;
        Pipeline p = build_pipeline(c);
        CHECK(p.ok());
        CHECK(contains(p.shell, "-vf 'format=yuv420p'"));
        CHECK(!contains(p.shell, "pad="));
        Config r = parse_config(serialize_config(c));
        CHECK(r.sources.lcd_native);
    }

    // HDMI/desktop is the explicit option; with system audio and no mic.
    {
        Config c = with_twitch();
        c.sources.video = VideoSource::Screen;
        c.sources.screen = ScreenTarget::Hdmi;
        c.sources.mic = false;
        c.sources.system_audio = true;
        c.sources.wayland_display = "wayland-1";
        Pipeline p = build_pipeline(c);
        CHECK(p.ok());
        CHECK(contains(p.shell, "WAYLAND_DISPLAY='wayland-1'"));
        CHECK(contains(p.shell, "-F 'scale=1280:720'"));
        CHECK(contains(p.shell, "wf-recorder"));
        CHECK(contains(p.shell, "--audio='@DEFAULT_MONITOR@' -C aac"));
        CHECK(!contains(p.shell, "rpicam-vid"));
        CHECK(!contains(p.shell, "-f fbdev"));
        CHECK(contains(p.shell, "-f mpegts -i -"));
    }

    // Three destinations -> one copy output each.
    {
        Config c = with_twitch();
        c.destinations[1].enabled = true;
        c.destinations[1].key = "yt-key";
        c.destinations[2].enabled = true;
        c.destinations[2].url = "rtmp://192.168.1.50:1935/live";
        c.destinations[2].key = "restreamer-key";
        Pipeline p = build_pipeline(c);
        CHECK(p.ok());
        CHECK(p.destination_count == 3);
        CHECK(contains(p.shell, "rtmp://live.twitch.tv/app/live_123_SECRET"));
        CHECK(contains(p.shell, "rtmp://a.rtmp.youtube.com/live2/yt-key"));
        CHECK(contains(p.shell, "rtmp://192.168.1.50:1935/live/restreamer-key"));
        size_t outputs = 0;
        for (size_t pos = p.shell.find("-f flv "); pos != std::string::npos; pos = p.shell.find("-f flv ", pos + 1))
            ++outputs;
        CHECK(outputs == 3);
    }

    // A key containing a quote must not break out of the shell string.
    {
        Config c = with_twitch();
        c.destinations[0].key = "a'b;rm -rf /";
        Pipeline p = build_pipeline(c);
        CHECK(p.ok());
        CHECK(contains(p.shell, "'rtmp://live.twitch.tv/app/a'\\''b;rm -rf /'"));
    }

    // Config round trip keeps everything, including keys and source choices.
    {
        Config c = with_twitch();
        c.sources.video = VideoSource::Screen;
        c.sources.screen = ScreenTarget::Hdmi;
        c.sources.system_audio = true;
        c.sources.camera_rotate_180 = false;
        c.video.bitrate_kbps = 3500;
        c.background_stream = true;
        Config r = parse_config(serialize_config(c));
        CHECK(r.background_stream);
        CHECK(r.sources.video == VideoSource::Screen);
        CHECK(r.sources.screen == ScreenTarget::Hdmi);
        CHECK(r.sources.system_audio);
        CHECK(r.sources.mic);
        CHECK(!r.sources.camera_rotate_180);
        CHECK(r.video.bitrate_kbps == 3500);
        CHECK(r.destinations[0].key == "live_123_SECRET");
        CHECK(r.destinations[0].enabled);
        CHECK(!r.destinations[1].enabled);
    }

    // Out-of-range numbers are clamped, junk is ignored.
    {
        Config r = parse_config("[video]\nfps = 9999\nbitrate_kbps = abc\n");
        CHECK(r.video.fps == 60);
        CHECK(r.video.bitrate_kbps == 2500);
    }

    // Unknown destination ids are kept as custom entries.
    {
        Config r = parse_config("[destination.kick]\nurl = rtmps://x.example/app\nkey = k\nenabled = yes\n");
        bool found = false;
        for (const auto& d : r.destinations)
            if (d.name == "kick" && d.enabled && d.key == "k")
                found = true;
        CHECK(found);
    }

    if (failures == 0)
        std::printf("all stream tests passed\n");
    return failures == 0 ? 0 : 1;
}
