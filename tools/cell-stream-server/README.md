# cell-stream-server

Windows-side counterpart of the `cell-stream` PS3 app (`dev/apps/cell-stream`). It captures the
desktop, encodes it to H.264 on the GPU, streams it to the PS3 over UDP, and turns the pad packets
coming back into a virtual Xbox 360 controller (ViGEmBus) — or into mouse and keyboard.

WPF, .NET Framework 4.0 (same as the other tools, builds in the VM).

## Running it

Double-click `cell-stream-server.exe`. There are no arguments: the settings are baked in
(1280x720, 60fps, 10 Mbit/s, intra refresh) because this is an appliance, not a command line.
The one switch, `-minimized`, is what the Start-with-Windows option registers so log-in goes
straight to the tray.

The window is only a view onto the server. Minimising or closing it leaves everything running in
the notification area; **Exit** from the tray icon is the only thing that stops it. The dot is grey
while it waits and green while a PS3 is streaming, with a popup either way.

No installer. To update, replace the exe and restart it. The log lives in
`%LOCALAPPDATA%\cell-stream-server\server.log`.

Windows Firewall will prompt on first run — allow it, or nothing leaves the PC.

## What ships

The build output folder (`bin/`): the exe, `ffmpeg.exe` and `ViGEmBusSetup.exe`. The bundled ffmpeg is a
trimmed build (~8MB — only the encoders and filters this needs, not the full ~90MB distribution), and
falls back to the one on `PATH` if it is not next to the exe. The ViGEmBus driver is installed on demand,
the first time a PS3 asks for gamepad mode.

### Rebuilding the bundled ffmpeg

`build-ffmpeg.sh` (next to the source) cross-builds `ffmpeg.exe` from scratch in WSL — run it, then copy
its output over `ffmpeg.exe`. It has the full step-by-step in its header comment. The one thing that isn't
obvious and cost a real user their GPU: **nvenc and amf refuse to run if the bundled ffmpeg is newer than
the user's GPU driver.** ffmpeg's NVIDIA header (`nv-codec-headers`) sets the minimum driver a user must
have; the newest header demands a driver only weeks old, so most NVIDIA cards — even recently updated
ones — silently fall back to CPU. The script therefore pins that header to the *oldest* tag ffmpeg still
accepts (`n12.1.14.0`, min NVIDIA driver 531.61 from early 2023) and does the same for AMD's AMF. Bump
ffmpeg only with that trade-off in mind.

## How it works

- **Video** — ffmpeg `ddagrab` (Windows' GPU screen capture) → GPU scaler → H.264, tuned for latency:
  no B-frames, intra refresh instead of keyframes, one slice, one frame of encoder queue. It prefers Intel
  (Quick Sync) because the PS3 decodes its stream most reliably, then NVIDIA (nvenc) or AMD (amf), and falls
  back to the CPU (x264). You can pick a specific encoder in the window, which is then remembered — the
  dropdown greys out while a PS3 is streaming, since the encoder cannot change mid-stream. Each frame
  goes out the moment it leaves the encoder — no file, no pacing, no buffering.
- **Audio** — the desktop mix, alongside the picture.
- **Input** — pad packets from the PS3 drive either the virtual gamepad or the mouse/keyboard (button
  layout in `DesktopInput.cs`); the PS3 cycles input modes with SELECT + Cross. On-screen-keyboard
  characters arrive as `KEY` packets
  and are typed via `SendInput`. **Custom Commands** (a tab in the window) map `CUSTOM 1-4` to a
  program or URI; the PS3 only sends the slot number, so the PC never runs anything not set up there.
- **Discovery** — a `CELLSTREAM` beacon on :38311 every second, so the PS3 finds the PC on its own.
  Both sides hang up cleanly: 3 seconds of silence from the PS3 and the server stops encoding and
  puts the desktop resolution back.

## Credits

- **FFmpeg** (LGPL/GPL) — the bundled `ffmpeg.exe` does the screen capture, scaling and H.264 encode.
  Source and build steps: `build-ffmpeg.sh`.
- **ViGEmBus** by Nefarius Software Solutions (BSD-3-Clause) — the virtual-gamepad driver
  (`ViGEmBusSetup.exe`), installed on demand.
