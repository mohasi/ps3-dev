# cell-stream

Live streaming from a Windows PC to the PS3: picture and sound down, the controller back up.
Steam-Remote-Play style. PC counterpart: `dev/tools/cell-stream-server`.

**Working today (WiFi, 720p60): 60fps, ~25ms from encoder to screen, sound, and the pad driving the
PC — either as a mouse and keyboard, or as a real Xbox gamepad that games see as a plugged-in
controller.** Games are playable.

## Using it

Start the server (`cell-stream-server.exe`), then launch the app. The server is an appliance with
baked-in settings (720p60, 10 Mbit/s, intra refresh) and no arguments. **There is nothing to press.**
The app finds the server by itself (the server broadcasts a beacon), connects, and streams. If the
server goes away it says "waiting for server ..." and reconnects on its own when it comes back. Either
side can be started first.

While streaming **every button goes to the PC** — a game needs all of them — so the app uses SELECT as
a modifier for its own, and holds a combo's buttons back so the game never sees them:

| Combo | Does |
|---|---|
| SELECT + Cross | input mode: mouse only → mouse+keyboard → controller |
| SELECT + Square | stream mode: 720p/60 vsync off → vsync → vsync + one-frame buffer |
| SELECT + R3 | show/hide the stats panel (hidden by default) |
| SELECT + Triangle/Circle/L1/R1 | custom 1-4 (PC actions set in the server's Custom Commands tab) |
| START (not streaming) | exit |

In mouse+keyboard mode an on-screen keyboard docks bottom-right: d-pad moves, Cross types, Circle
backspaces, Triangle is space; the keys are injected on the PC. The combos are editable in
`/dev_hdd0/tmp/cell-stream/settings.txt` (created with defaults on first launch). A shortcut list shows
briefly on connect, and the waiting screen shows a QR code to the server's download page.

**Both sides hang up cleanly.** The PS3 treats 2s of no video as "the server is gone". The server treats
3s of no pad (it arrives 60x/second while streaming) as "the PS3 is gone" — without that it would encode
to nobody forever *and leave the desktop stuck at the streaming resolution*.

`sendRateKbps` caps how fast packets leave, independent of the video bitrate. It must stay under what
the link can carry — **the PS3's WiFi radio tops out at ~22Mbps** (the console's limit, not the AP's).

## Where the 25ms goes

The stats panel (and the log, every 5s) breaks the journey into its stages, each timed against the
server's clock: `network + decode + present + display = total`.

| Stage | Typical | What it is |
|---|---|---|
| network | 3-9ms | encoder exit → the frame's last fragment arrives |
| **decode** | **20ms** | fed to cellVdec → picture handed back |
| present | 0.6ms | picture → handed to the RSX |
| display | **0ms** | → the screen takes it (8ms if you wait for vsync) |

**Decode owns it, and there is no knob.** 20ms is longer than the 16.7ms between frames, yet 60fps is
still sustained — that is only possible because cellVdec is pipelined: a frame takes ~20ms to travel
through while the decoder still keeps up with the rate. It is latency baked into Sony's decoder, not a
queue we can drain. The only lever left is fewer pixels.

*Tried and failed: `-max_dec_frame_buffering 1` on the encoder, to stop the PS3 holding a frame back.
It does nothing. The PS3 picks its own buffer count (`decode-h264.c` floors at refs+1 = 2) and never
reads that field from the stream. Don't retry it.*

**Three presentation modes (SELECT + Square); immediate flip is the default.** *Vsync off* puts a decoded
picture on the screen the moment it is ready instead of waiting for the TV's next refresh — `display` 0.0ms
vs ~8ms — at the cost of a little tearing, and the render loop is paced by the arriving video. *Vsync*
locks to the refresh so nothing tears (~8ms back). *Vsync + one-frame buffer* presents on the refresh one
frame behind, keeping a decoded frame in reserve so a late one is covered by the spare — it rides out the
occasional hitch for ~one frame (16.7ms) of delay. All three are 720p/60; the choice is saved and restored.

*Measuring trap, in case anyone touches this:* time the display wait INSIDE the flip
(`getGfxFlipWaitUs`). Do NOT stamp the frame after `endGfxFrame` — that call only QUEUES the flip, so
stamping there tacks a bogus ~16.7ms onto every mode and looks exactly like a regression. The `display`
figure is still reported (it should stay 0) so that a regression would show up.

## Wire protocol (UDP; server :38310, PS3 :38311)

| Packet | Direction | Meaning |
|---|---|---|
| `CELLSTREAM 1` | server → broadcast | discovery beacon (1/s, per-adapter directed broadcast) |
| `TIME <us>` | both ways | clock sync, so the PS3 can measure each stage's latency |
| `PLAY` / `STOP` | PS3 → server | start/stop. PLAY is repeated until answered, so repeats must be ignored, not restarted |
| `SINFO <w> <h> <level> <refs> <fps> <intraRefresh>` | server → PS3 | frame rate + which loss recovery the stream uses |
| `VF` fragment | server → PS3 | one slice of a video frame (20-byte header, ≤1300B payload) |
| `AINFO <rate> 2` / `AF` | server → PS3 | audio rate (repeated 1/s), then 5ms PCM packets |
| `CP` | PS3 → server | controller state, 60/s. doubles as the server's proof the PS3 is still alive |
| `PADMODE gamepad\|mouse` | PS3 → server | which PC device the pad drives (repeated 1/s, so a lost one is harmless) |
| `KEY <char>` | PS3 → server | one on-screen-keyboard character, injected on the PC |
| `CUSTOM <1-4>` | PS3 → server | run the PC action bound to that Custom Commands slot |

## The three rules this app was built on

**1. Never trust a claimed video size — read it from the stream.** cellVdec must be built for the
stream's CODED size, which only the bitstream's own SPS knows. A container (or a sender) reports the
DISPLAY size, and encoders pad past it: Intel QuickSync codes 720 as **736** and crops it back. Told a
size too small, the decoder silently decodes NOTHING (black screen, no error). Told one too large, it
**hard-locks the console**. So the first keyframe's SPS configures the decoder (`readH264StreamInfo`).

**2. The receive thread must never wait for the decoder.** When both ran on one thread, a decode stall
stopped us draining the socket, packets were lost, and every loss froze the picture. The receive thread
now only reassembles frames into a queue; a separate decode thread drains it.

**3. Never free RSX video memory from a worker thread.** The RSX may still be scanning the last frame.
The draw thread frees the buffers (`releaseStreamBuffers`) a few rendered frames after the stream ends.
Freeing them promptly hard-froze the console.

## Loss recovery: intra refresh, and why the sweep speed is everything

Default. Instead of a keyframe every second, every frame redraws a thin strip, sweeping across the
picture. It removes the keyframe burst — several times the size of a normal frame, and itself the cause
of most of the WiFi packet loss. Measured: **59ms → 39ms end-to-end, frozen frames 287 → 0.**

**The first attempt failed, and the trap is worth writing down.** With a 0.5s sweep under tight CBR, a
blur bar was plainly visible wiping across the picture — even on a *still* desktop, with zero packet
loss. A fast sweep means a FAT intra strip in every frame; under CBR the encoder can't afford it, codes
it coarsely, and you see it.

Measured off the encoder's own output rather than by eye — capture 8s to a file, decode it on the PC,
and difference each frame against the last per vertical strip (on a still desktop the refreshed strip
is the only thing that changes, so it stands out):

| | strip redraw strength | marching frames |
|---|---|---|
| 0.5s sweep, CBR, 8-frame buffer | 0.75 grey levels (worst 10%: 2.57) | 228 / 480 |
| 1s sweep, VBR, 250ms buffer (**shipped**) | 0.06 | 72 / 480 |
| no intra refresh (baseline) | 0.02 | 58 / 480 |

So the **rate control** was most of the fix, not the sweep length: given headroom to borrow bits, even a
1s sweep sits ~10x below the bar we could see. 1s is chosen because the sweep is also what repairs a
lost frame — a 4s sweep left visible damage on screen for 4 seconds.

Consequence on the PS3: an intra-refresh stream repairs itself, so it decodes straight through a lost
frame instead of freezing. `SINFO` says which kind of stream it is and the PS3 picks its behaviour.

There is no periodic keyframe: intra refresh repairs the picture continuously, and the PS3 gets the one
keyframe it needs when it connects.

## The encoder holds far less than it looks like — beware the measurement

An early test said the PC held each frame **172ms** before sending it. It was **an artifact**: Windows'
screen capture (ddagrab) only produces a frame when the screen actually CHANGES, so on a near-still
desktop it ran at ~18fps, not 60, and the timing measured the idle desktop rather than the encoder. Any
measurement here MUST keep the screen changing continuously.

Measured properly (synthetic 60fps source, marker frame, no screen involved), every encoder-side tweak
moved latency by **less than 10ms — inside the run-to-run noise**:

| Change | Effect |
|---|---|
| scaler queue 4 → 1 (`vpp_qsv:async_depth=1`, **shipped**) | ~7ms — small but free |
| reader emitting a frame sooner | ~2ms, and risks a corrupt frame — not worth it |
| `-low_delay_brc`, `-max_dec_frame_buffering` | nothing measurable |
| `-preset veryfast` | not a delay knob on QuickSync (unlike x264) |

The one real find: **`vpp_qsv` has its own `async_depth`, defaulting to 4** — the `-async_depth` you
pass to the encoder does not touch the scaler's queue. Worst case that is 4 frames (~67ms); in practice
at a steady 60fps the queue never backs up, so it was worth only ~7ms. Shipped anyway; it costs nothing.

## Sharpness: it was never the bitrate

Raising 10 → 16Mbps made things WORSE (the decoder can't keep up: decode 20 → 45ms, one window fell to
8fps). 1080p tripled the latency (80-120ms, 27fps) — the PS3's decoder is the wall. Encoder presets and
scaler quality changed nothing visible.

The cause was **resizing**: a 1080p desktop squashed to 720p, then stretched back up by the PS3. So the
server now switches the PC's desktop TO the streaming resolution while streaming and restores it after —
1:1 pixels, no resize anywhere. Restoring is defended several ways: when the stream stops, when the
encoder dies on its own, on exit and Ctrl+C and an unhandled crash, and the mode is registered as
temporary so Windows undoes it even if we are killed.

## Audio

The PC's speaker output is captured (WASAPI loopback, straight COM interop — no library, no driver) and
sent **uncompressed**: 5ms packets, 16-bit stereo, ~1.5Mbps. No encoder and no decoder is exactly the
latency we are trying not to spend. The PS3 pushes it into the audio mixer's PCM feed with a 60ms
backlog.

The two machines' clocks tick at slightly different speeds, so the backlog — which IS the audio delay —
creeps in one direction for as long as the stream runs (measured: 54ms → 122ms and still climbing). The
PS3 drops or repeats a single 5ms chunk when it strays too far, which holds it steady. That fires about
once every 25 seconds: far too little to hear.

## Controller

The PS3 sends its pad 60x/second; measured **4-5ms** PS3 → PC, no loss. The PC replays it two ways, and
SELECT + Cross cycles input modes mid-stream (use the mouse to launch a game, then switch to the gamepad):

**Gamepad** (default). A virtual Xbox 360 controller: games and Windows see a real one plugged in.
Windows has no way to fake a controller from a normal program, so this needs the **ViGEmBus** driver —
the only thing here that installs anything. `ViGEmBusSetup.exe` sits next to the server and is run
silently the first time a gamepad is asked for (Windows asks for permission once, on the PC, ever). The
driver normally comes with a client DLL, but that DLL is a wrapper around three driver calls, so we make
them ourselves (`VirtualGamepad.cs`) and ship nothing but the driver. Without it we log why and stay on
the mouse.

**Mouse and keyboard** (`SendInput` — nothing to install). Left stick = pointer, right = scroll,
X/O = clicks, d-pad = arrows, L1/R1 = page up/down.

## Known rough edges

- **Bitrate spikes on WiFi.** `network` jumps from ~3ms to ~9ms whenever the bitrate reaches 11-12Mbps,
  which is close to the PS3's ~22Mbps radio ceiling. Busier video means bigger frames and a longer
  delivery time on the link; a wired connection would flatten it.

## Credits

- **miniz / tinfl** (public domain) — the inflate used to decode the console's button glyphs.
- **segno** (BSD) — generated the waiting-screen QR code data at build time.
- On-screen button glyphs are the PS3's own system font art, decoded at runtime — not shipped by us.
