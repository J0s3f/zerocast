# ZeroCast

Live-stream from a [CardputerZero](https://shop.m5stack.com/pages/m5-cardputerzero) to **Twitch**, **YouTube**, a **Restreamer**
instance or any RTMP server, several at once, from the device itself.

- **Sources:** the built-in camera + microphone (default); or a **USB webcam** (with its own microphone if it has
  one); or the built-in LCD or an HDMI desktop as the video. System audio is optional and is mixed with the microphone
  if both are on. The microphone input can be chosen on the Sources page.
  With several USB cameras plugged in, pick one on the Sources page (or leave it on *auto*). ZeroCast asks the camera
  what it can do (format, size, frame rate) and requests the best match for the stream (MJPEG preferred, raw YUYV/NV12
  for cameras without it), so ordinary UVC webcams work without setup; the picture is letterboxed into the frame.
  "Camera format" (Auto / MJPEG / Raw) lets you force one, and only lists what the camera supports.
- **Hardware H.264:** video is encoded by the SoC's hardware encoder and only copied afterwards, so the CPU stays idle.
- **Stream keys** are typed on the device, or entered from a phone through an optional setup page that you start on
  demand (see below). Keys are stored in the config file with mode 0600 (`/etc/zerocast-stream.conf` when ZeroCast runs
  as root, otherwise `~/.config/zerocast/stream.conf`) and are never shown again.

## Using it

Open **ZeroCast** from APPLaunch.

| Key | Action |
| --- | --- |
| `Tab`, `1`-`4` | Live / Sources / Destinations / Settings |
| Up/Down (or `F`/`X`, `K`/`J`) | select a row |
| Left/Right (or `Z`/`C`, `H`/`L`), `Enter`, `Space` | change the selected value |
| `G` or `Enter` on *Live* | go live / stop |
| `E` / `U` on a destination | edit its stream key / server URL |
| `Esc` | back or cancel; **hold** to exit (while streaming only if *Keep streaming when closed* is on) |
| `PrintScreen` | screenshot |

The pages are **Live**, **Sources**, **Destinations** (titled "Send to") and **Settings** (bitrate, resolution, frame
rate, *Keep streaming when closed*, theme).

First run: **Destinations** -> select Twitch/YouTube/... -> `E`, type the stream key, `Enter` -> `Enter` to switch the
destination on -> **Live** -> `Enter`.

### Streaming in the background (optional, off by default)

Streaming the LCD is only useful if the stream can keep running while you use other apps. **Settings -> Keep streaming
when closed** makes the stream survive closing or switching away from ZeroCast (including when the launcher kills it).
Open ZeroCast again and it reconnects to the running stream (stats, stop button). From SSH:

```sh
zerocast --status   # is a stream running in the background?
zerocast --stop     # stop it
```

With the setting **off** (the default), the stream always stops when ZeroCast exits or is killed. A background stream
keeps using the camera/microphone, the network and some CPU until you stop it, so the setting is shown in amber.

### Entering keys from a phone (optional)

**Destinations -> Remote setup** starts a small web page on your Wi-Fi. The device shows its address and a fresh
random password. Open the address on a phone or PC on the same network, sign in with the user name (`zerocast`) and password shown
on the device, and fill in the keys. The page switches itself off after 5 minutes, 30 seconds after you save, after 5 wrong passwords, or when you
press `Esc`. It is plain HTTP: the password and keys cross your network unencrypted, so use it on a network you trust,
or type the key on the device instead.

## Requirements

`ffmpeg`, `rpicam-apps` (the `rpicam-vid` tool) and `pulseaudio-utils` (`pactl`) are installed as dependencies. Recommended
and installed by default by most images: a sound server (`pipewire-pulse` or `pulseaudio`, needed for any audio),
`v4l-utils` (lets ZeroCast read what a USB camera offers; without it ffmpeg is asked instead) and `wf-recorder`
(HDMI/desktop capture only). The package is arm64 only.

## Notes and limits

- The camera delivers about 21 fps at 720p. The default is 720p at 2.5 Mbit/s per destination.
- Streaming the LCD streams whatever is on it, including ZeroCast while it is in front. Turn on *Keep streaming when
  closed* and switch to another app to stream that instead.
- If a stream shows no frames for 40 seconds it is stopped with "No data after 40 s: replug camera/mic". This happens
  when a camera or microphone opens but never delivers (seen with a USB microphone in a bad state). Replug the USB
  device, or pick another **Mic input** on the Sources page.
- See [DESIGN.md](DESIGN.md) for the pipeline, what was measured, and the reasons for each choice.

## Building

The project is built from the CardputerZero Template (LVGL 9.5 + CMake). A container build for an x86_64 PC:

```sh
docker build -t zerocast-build -f docker/Dockerfile.build .
docker run --rm -v <project dir>:/src -w /src zerocast-build cmake --workflow --preset cp0-cross-package
```

Tests for the streaming core (no LVGL needed; they use POSIX headers, so run them on Linux, the Pi or in the container):

```sh
g++ -std=c++17 -Isrc/stream tests/stream_test.cpp src/stream/stream_config.cpp src/stream/pipeline.cpp -o stream_test && ./stream_test
g++ -std=c++17 -Isrc/stream tests/web_test.cpp src/stream/web_config.cpp src/stream/stream_config.cpp src/stream/pipeline.cpp -o web_test && ./web_test
```

## License

MIT. Fonts and sounds under `assets/` have their own licenses, see `assets/fonts/LICENSE.txt`.
