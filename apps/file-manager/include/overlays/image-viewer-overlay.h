#pragma once

// image-viewer-overlay - full-screen still-image viewer.
// Opens on a supported image (PNG/JPEG). When the image's directory holds more
// supported images, L1/R1 navigate among them (wrapping at the ends). L2/R2
// zoom, the D-pad pans, and Circle closes.

#include "overlay.h"

// Opens the viewer on the given absolute image path. If the path's directory
// contains other supported images, L1/R1 will navigate among them.
// Returns 0 on success, or -1 if the image failed to decode (overlay stays shut).
int openImageViewer(const char *imagePath);

// Global overlay instance (defined in the .c; extern here so the home screen
// can register it in its update/draw/term loops).
extern Overlay imageViewerOverlay;
