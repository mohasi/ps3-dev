#pragma once

// hex-viewer-overlay - full-screen hex/ASCII viewer and byte editor. Streams
// the file a page at a time (never loads it whole), so it stays safe on huge
// files. Opens for any file type without a more specific viewer.

#include "gfx.h"
#include "overlay.h"

// One-time setup (fonts, labels, header icon sprite). Call once during home screen init.
void initHexViewerOverlay(GfxTexture sprites);

// re-applies the active theme to the pre-rendered labels, for a live theme switch.
void rethemeHexViewerOverlay(void);

// Opens the viewer on the given absolute file path. Returns 0 on success, or
// -1 if the file could not be opened (overlay stays shut).
int openHexViewer(const char *path);

// Global overlay instance (defined in the .c; extern here so the home screen
// can register it in its update/draw/term loops).
extern Overlay hexViewerOverlay;
