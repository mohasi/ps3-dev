#pragma once

// stream - receives an H.264 video stream from cell-stream-server over UDP and
// decodes + publishes frames for render-on-arrival drawing (no buffering, no clock).

// the app connects by itself and keeps trying: no button to press, and a server that goes away just
// puts us back to WAITING until it comes back.
typedef enum {
   STREAM_STATE_WAITING,      // looking for a server's beacon
   STREAM_STATE_CONNECTING,   // found one, shaking hands
   STREAM_STATE_STREAMING,
   STREAM_STATE_ERROR         // something we cannot retry our way out of
} StreamState;

typedef struct {
   StreamState state;
   char error[64];          // what to show the user for the current state ("waiting for server ...", an error)
   int width, height;       // coded stream size (0 until SINFO arrives)
   int sourceFps;
   int framesComplete;      // fully reassembled frames
   int framesIncomplete;    // frames dropped due to lost fragments
   int framesDroppedBehind; // frames dropped because the decoder fell a whole queue behind
   int framesSkipped;       // complete frames not decoded while waiting for a keyframe after a loss
   int bitrateKbps;         // received video bytes over the last second
   int receivedFps;         // frames actually arriving per second (vs the source's advertised fps)
   int pipelineDepth;       // access units fed minus pictures returned (decoder hold-back)

   // per-stage latency over the last second, in 0.1ms. all measured against the server's clock
   // (synced at startup), so they add up to the real journey from encoder exit to on-screen.
   int networkMsTenths;     // encoder exit -> last fragment of the frame arrived
   int assembleMsTenths;    // first fragment -> frame complete (fragment spread + loss waits)
   int decodeMsTenths;      // fed to the decoder -> picture handed back
   int presentMsTenths;     // picture ready -> handed to the RSX (render loop)
   int displayWaitMsTenths; // ... then waiting for the display to take it (a whole refresh with vsync on, ~0 without)
   int totalMsTenths;       // encoder exit -> drawn on screen (everything we can measure)
} StreamStats;

// initStream starts the connect loop straight away: it finds the server by itself, streams, and goes
// back to looking if the server disappears. stopStream ends the loop for good (app exit).
void initStream(void);
void stopStream(void);           // async; safe to call from the UI thread
int  isStreamRunning(void);      // the connect loop is alive (not necessarily streaming)
int  isStreamLive(void);         // a stream is actually running: video is arriving
void getStreamStats(StreamStats *out);

// sends the controller to the PC, which replays it on a virtual gamepad or on the mouse and
// keyboard. call once per rendered frame while streaming; sticks are -128..127, buttons a bitmask
// of (1 << PadButton). no-op when not streaming, and never blocks the render loop.
void sendPadState(unsigned buttons, int leftX, int leftY, int rightX, int rightY);
void sendPadMode(int useGamepad);   // 1 = virtual gamepad (for games), 0 = mouse and keyboard
void sendCustomCommand(int slot);   // 1..4 - asks the PC to run the command bound to that slot
void sendKeystroke(char key);       // one character typed on the on-screen keyboard, for the PC to inject

// one-frame buffer: off shows each picture the instant it decodes (paced by the arriving video); on
// presents on the display's refresh one frame behind, keeping a reserve to ride out late frames.
void setStreamBuffered(int on);

// true when a decoded picture is waiting that has not been drawn yet. the render loop draws only
// then, so it is paced by the arriving video rather than by the display's refresh.
int isNewStreamPictureReady(void);

// draws the latest decoded frame scaled to the screen; no-op when none is ready yet.
// call from the render loop between beginGfxFrame/endGfxFrame.
void drawStreamFrame(void);

// call after endGfxFrame with getGfxFlipWaitUs(): how long it blocked waiting for the display to take
// the previous frame. that wait is the cost of vsync, and the immediate-flip mode is what removes it.
void noteStreamFlipWait(uint64_t microseconds);

// frees the picture buffers of an ended stream. must be called from the draw thread, a few
// rendered frames AFTER isStreamLive() went false (the RSX must be done with the last frame). the next
// session waits for this before allocating its own pictures.
void releaseStreamBuffers(void);
