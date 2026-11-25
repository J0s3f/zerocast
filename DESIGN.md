# ZeroCast design

ZeroCast turns a CardputerZero into a pocket live-streaming encoder. It captures
sources, encodes with the SoC's hardware H.264 encoder, and pushes RTMP(S) to up
to several destinations at once (Twitch, YouTube, a Restreamer/datarhei
instance, or any custom RTMP URL).

## Sources

| Kind | Default | Implementation (verified on the device) |
| --- | --- | --- |
| Camera video | **on** | `rpicam-vid --codec libav --libav-format mpegts --libav-video-codec h264_v4l2m2m` (hardware H.264, real sensor timestamps, see Pipeline) |
| Microphone | **on** | captured inside the producer (`--libav-audio` for the camera, `-f pulse` for ffmpeg producers); PipeWire-pulse, default `@DEFAULT_SOURCE@`, selectable under Sources -> Mic input |
| USB webcam | off, optional | `ffmpeg -f v4l2` on a UVC capture node, scaled/letterboxed to the frame size, hardware H.264. See "USB cameras" below. Its own microphone is a normal audio input (Sources -> Mic input). Measured with an AUKEY PC-W3S: 720p MJPEG -> 25 fps steady, ~1.2 CPU cores for decode. |
| Screen: built-in LCD | off, optional (the default *screen* target) | `ffmpeg -f fbdev` on the 320x170 panel (`panel-mipi-dbid`, found via `/sys/class/graphics`), hardware H.264. Default: exact 4x nearest-neighbour upscale (lossless) letterboxed to 720p, which Twitch/YouTube players show larger. Optional: `lcd_native = yes` sends the panel's own 320x170. |
| Screen: HDMI / desktop | off, explicit option | `wf-recorder` (Wayland, socket auto-detected, scaled to the video size), piped as MPEG-TS |
| System audio | off, optional | `ffmpeg -f pulse -i @DEFAULT_MONITOR@` (monitor of the default output) |

Rules:

- Exactly one *video* source is active: the built-in camera (default), a USB camera or the screen. "Screen" means
  the Cardputer's own LCD unless the user picks HDMI/desktop.
- Streaming the LCD shows whatever is on it, including ZeroCast's own UI while it is in
  the foreground. With the *Keep streaming when closed* setting the stream keeps running after the user
  switches to another app and ZeroCast re-attaches when reopened (see Background streaming).
- Audio is any combination of mic and system audio; both are mixed in a temporary PipeWire sink.
  With no audio selected a silent track is added, because Twitch and YouTube
  expect an audio track.
- Video is **never software-encoded**: the Pi Zero-class CPU cannot do 720p in
  software. It is encoded once by the hardware encoder and then only copied. Only audio is
  encoded in software (AAC, negligible CPU).

## Pipeline

```
 producer (video + audio, MPEG-TS) ──▶ ffmpeg -c copy ─┬─▶ rtmp://destination 1
 rpicam-vid | ffmpeg -f fbdev | wf-recorder            └─▶ rtmp://destination 2 ...
```

- The **producer** captures the video, encodes it with the hardware H.264 encoder (`h264_v4l2m2m`,
  `/dev/video11`) and captures + encodes the audio (AAC) itself, muxing both with one clock into MPEG-TS.
- The **consumer** ffmpeg only copies packets to one RTMP output per destination (no tee muxer, no re-encoding).
- Audio is captured in the producer on purpose: with a real-time microphone in the consumer, the
  muxer waits for audio, the video queue fills, the pipe blocks and the camera drops about two thirds of its
  frames (measured). With audio inside `rpicam-vid` the camera holds its full 20.8 fps.
- Mic + system audio: the engine loads a temporary PipeWire null sink with two loopbacks
  (`zerocast_mix`) and records its monitor; the modules are unloaded when the stream ends.
- The camera uses `rpicam-vid --codec libav --libav-format mpegts` (same hardware encoder) so frames carry real
  sensor timestamps. Raw H.264 has none; faking them with a fixed frame rate drifted (the IMX219 delivers ~21 fps
  at 25 requested) and made YouTube report "not enough video data".
- The LCD path runs `ffmpeg -f fbdev ... -fps_mode cfr` so a static screen still produces a steady frame rate.
- No audio selected: a silent AAC track is generated in the consumer (not real time, so it cannot stall video).

## USB cameras

- **Finding them:** `/sys/class/video4linux/videoN` nodes whose parent chain goes through the USB bus and whose `index` is 0
  (a UVC camera also exposes a metadata node at index 1). Sources -> *USB camera* picks one or leaves it on *auto* (the first).
  A chosen camera that is no longer present falls back to the first one. Names are tidied for the 320 px screen
  ("Product: Product" -> "Product", company words and serial numbers dropped) and identical names get a number ("1 Name", "2 Name").
- **Probing:** at stream start the camera is asked what it offers (`v4l2-ctl --list-formats-ext`; ffmpeg's format list, without
  frame rates, if `v4l2-ctl` is missing). Only formats ffmpeg handles reliably are considered: MJPEG, YUYV, UYVY, NV12, YU12;
  camera-side H.264 is ignored.
- **Choosing:** the best mode for the output size and frame rate. Tier 1: reaches the frame rate at a size that covers the output
  (smallest picture, then MJPEG before raw: less decoding and bandwidth; a USB 2.0 camera only reaches 720p30 compressed).
  Tier 2: reaches the rate at any size (largest). Tier 3: fastest available. A camera that cannot reach the rate is asked for its own.
  The picture is letterboxed into the frame and `-fps_mode cfr` keeps the output rate steady.
- **Overrides:** Sources -> *Camera format* (Auto / MJPEG / Raw), which only lists what the camera supports; `usb_format`,
  `usb_size` (`WxH`) and `usb_fps` in the config file. A saved format the camera does not have is treated as Auto.

## Failures and the start watchdog

- A pipeline that ends on its own is a failure; the Live page shows one short line from its log. Camera and encoder chatter
  (`[h264_v4l2m2m @ 0x..] Using device ...`, libcamera `INFO`/mode-selection lines) is skipped in favour of the last line that
  looks like an error.
- A stream that has produced no frames after 40 s is stopped and reported ("No data after 40 s: replug camera/mic"). The usual
  cause is a device that opens but never delivers: a USB microphone left in a bad state made `ffmpeg -f pulse` block indefinitely.

## Layers

| Layer | Path | Notes |
| --- | --- | --- |
| Config | `src/stream/stream_config.*` | INI file (mode 0600, holds stream keys): `/etc/zerocast-stream.conf` as root, else `~/.config/zerocast/stream.conf`, or `ZEROCAST_STREAM_CONFIG` |
| Command builder | `src/stream/pipeline.*` | Config in, shell pipeline + validation errors out (unit-tested without hardware); also device detection (LCD, Wayland, USB cameras) and camera mode parsing/choosing. |
| Engine | `src/stream/stream_engine.*` | Starts/stops the pipeline under a supervisor in its own session, tracks state and ffmpeg `-progress` stats, start watchdog, attach/detach, sets up and removes the temporary audio mix sink. |
| Remote setup | `src/stream/web_config.*` | Opt-in LAN web page for entering keys (see below). |
| UI | `src/view/screens/zerocast_ui.*` | LVGL 320x170: Live, Sources, Destinations, Settings pages, key-entry overlay, remote-setup overlay. |
| Input | `src/platform/linux_input.cpp` | evdev -> LVGL keys; full keyboard with Shift so stream keys can be typed. |

## Background streaming (setting, off by default)

Each stream runs under a small **supervisor** process in its own session, which is the parent of the shell pipeline.
The supervisor is started as a transient **systemd service** (`systemd-run --user --collect ... zerocast --supervise <job>`), so
systemd is its parent. This matters for background streaming: when the app exits, a forked supervisor would be adopted by the
APPLaunch launcher, which never reaps it (a `[zerocast] <defunct>` zombie, and the app then believed the dead stream was still
running, so "stop" hung). A service is also outside the launcher's process tree. The pipeline (it holds the stream keys) is
handed over in a 0600 job file that the supervisor deletes as soon as it has read it, never on a command line. Without
`systemd-run` the app forks the supervisor itself (the old behaviour). A service cannot use the parent-death signal, so it watches
the app's pid instead; it writes the state file and, when the pipeline ends, an `.exit` file with the exit code for the app.
Stopping never hangs: SIGINT, SIGKILL to the whole group after 4 s, and after 3 s more the stream is reported stopped anyway.

- Default: the supervisor asks the kernel to signal it when the app dies (`PR_SET_PDEATHSIG`). On that signal it stops the
  whole pipeline gracefully (SIGINT to the process group, SIGKILL after 4 s) and unloads the temporary audio mix. So exiting
  *or being killed by the launcher* never leaves an orphaned stream. (Before this, a killed app left the pipeline running
  with no way to stop it.)
- With **Keep streaming when closed**: a `keep-running` flag file exists. When the app goes away the supervisor sees the flag
  and carries on. The flag is checked at that moment, so toggling the setting while live takes effect immediately.
- A state file (pid, start time, audio module ids) lets the next app instance `attach()`: it verifies the pid and its
  `/proc` start time (pid reuse), then shows stats and can stop it. `zerocast --status` / `--stop` do the same from SSH.
- Runtime files live in a private directory (`/run/zerocast`, `$XDG_RUNTIME_DIR/zerocast` or `/tmp/zerocast-<uid>`, mode 0700),
  not in world-writable `/tmp`. Audio modules are only unloaded when their arguments name our `zerocast_mix` sink.
- The supervisor, not the app, unloads the audio mix when the pipeline ends, so it is cleaned up even if the app is gone.

## UI and key map

Pages (Tab or `1`-`4`): **Live** (status, uptime, fps/bitrate, source and destination summary, Enter = go live / stop),
**Sources** (video camera / USB camera / screen, USB camera picker and format, LCD or HDMI, flip, mic, mic input, system audio,
LCD native size), **Destinations** (titled "Send to"; per destination on/off + key state, `E` edit key, `U` edit URL,
"Remote setup"), **Settings** (bitrate, resolution, frame rate, keep streaming when closed, theme).

| Key | Action |
| --- | --- |
| Up/Down or F/X or K/J | select row |
| Left/Right or Z/C or H/L, Enter, Space | change the selected value (Left/Right switch pages on Live) |
| G | go live / stop from any page |
| Esc | back (cancel in overlays); **hold Esc** exits the app (while streaming only with *Keep streaming when closed*) |
| PrintScreen | screenshot |
| Edit overlay | type, Backspace, Enter saves, Esc cancels, Tab shows/hides the text |

A typed stream key is masked (only the last character shows) and is never pre-filled when editing,
so an existing key cannot be read off the screen. Leaving the key blank keeps the saved one.

## Remote setup (LAN web page): opt-in, short-lived, password-protected

Typing a 40-character key on the device is slow, so there is an optional web page, but it is never on by default:

- Starts **only** when the user chooses "Remote setup" on the Destinations page.
- Every activation generates a fresh random 8-character password, shown **only on the device screen**, together
  with the URL (`http://<ip>:8080`), a user name (`zerocast`, though any name works) and a countdown. HTTP Basic auth.
- Stops by itself after 5 minutes, 30 seconds after a successful save, after 5 wrong passwords (lockout), or when the
  user presses Esc. The password and CSRF token are wiped when it stops.
- Saved keys are never sent back to the browser (a blank key field means "unchanged"); form posts need a
  per-activation CSRF token; responses carry `no-store` and a strict Content-Security-Policy.
- It is plain HTTP, so the password and a new key cross the LAN unencrypted. That is the reason for the short window
  and the single-use password. Use it on a trusted network, or type the key on the device.

## Stream keys

Keys never leave the device except to the RTMP endpoint the user configured.
They are stored only in the config file (mode 0600, see Layers) and are masked in the
UI and in logs (`rtmp://host/app/****`). They can be typed on the device, pasted through the opt-in
remote setup page, or edited over SSH.

## Known limits

- Hardware: 4 cores, ~350 MB RAM. 720p at 2.5 Mbit/s per destination is the default; every extra
  destination adds upload bandwidth (the Pi's Wi-Fi measured ~24 Mbit/s up, plenty for several).
- The camera delivers ~21 fps at 720p, whatever frame rate is requested above that.
- HDMI/desktop capture needs a running Wayland session (labwc) and the `wf-recorder` package.
  Only one video source is active at a time.
- Streaming the LCD shows whatever is on it, including ZeroCast itself while it is in front. Without *Keep streaming when
  closed*, leaving the app stops the stream.
- The main loop sleeps 5 ms per iteration. LVGL's `lv_delay_ms()` busy-waits when no delay callback is registered (about 80% of
  a core while idle, measured), which competes with the encoder.
- Keyframes arrive every ~2.4 s at the camera's real frame rate; Twitch and YouTube accept up to 4 s.
