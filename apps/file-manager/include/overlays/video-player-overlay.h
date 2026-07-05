#pragma once

// video-player-overlay - full-screen player for a single video file (MKV/MP4, H.264 + AAC).
// Any video opens it; the overlay probes the file and, if the PS3 can't decode it, shows the exact
// reason. A playable file will draw its decoded frames (added in later phases). Circle closes.

#include "overlay.h"
#include "gfx.h"

// One-time setup; hands the overlay the shared sprite sheet (used by the volume meter and seek bar
// once playback lands). Call once at screen init, like the other overlays.
void initVideoPlayerOverlay(GfxTexture spritesheet);

// Opens the player on the given absolute video path. Returns 0 on success, or -1 if the path isn't
// a video file (the overlay itself reports codec support once open).
int openVideoPlayer(const char *videoPath);

// Global overlay instance (defined in the .c; extern here so the home screen can register it).
extern Overlay videoPlayerOverlay;
