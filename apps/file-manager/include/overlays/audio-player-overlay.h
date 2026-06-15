#pragma once

// audio-player-overlay - full-screen player for a single audio file.
// Shows the file icon and name, a live waveform, a seek bar with elapsed/remaining
// times, and a volume meter. Cross toggles play/pause, the D-pad left/right seeks
// (hold to accelerate), up/down changes volume, and Circle closes.

#include "overlay.h"
#include "gfx.h"

// One-time setup: hands the overlay the shared sprite sheet it draws the pills,
// thumb and speaker glyph from. Call once at screen init (like the other overlays).
void initAudioPlayerOverlay(GfxTexture spritesheet);

// Opens the player on the given absolute audio path. Returns 0 on success, or -1 if
// the file isn't a playable format or failed to decode (overlay stays shut).
int openAudioPlayer(const char *audioPath);

// Global overlay instance (defined in the .c; extern here so the home screen can
// register it in its update/draw/term loops).
extern Overlay audioPlayerOverlay;
