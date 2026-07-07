#pragma once

// screenshot - full-screen PNG capture of the gfx front buffer.
//
// Opt-in per app: call enableScreenshot() once at startup, then
// handleScreenshot() once per frame (after updatePad()). Holding L3+R3
// captures the last presented frame and writes it to
//   /dev_hdd0/tmp/screenshots/<YYYYMMDD-HHMMSS>.png
// at native display resolution (1920x1080 when the console outputs 1080p).
//
// The encode is synchronous and briefly hitches the frame it runs on -- fine
// for a screenshot. Linking an app that uses this requires -lpngenc_stub.

// Arm the L3+R3 hotkey. No-op until called; safe to call more than once.
void enableScreenshot(void);

// Poll the hotkey once per frame, after updatePad(). Captures + saves when
// L3+R3 first becomes fully held. Returns 1 if a screenshot was taken this
// frame, 0 otherwise (including when not enabled).
int handleScreenshot(void);

// Capture the current front buffer to a timestamped PNG now, regardless of the
// hotkey. Returns 0 on success. When outPath is non-NULL it receives the saved
// file path (truncated to cap).
int takeScreenshot(char *outPath, int cap);
