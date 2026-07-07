#pragma once

// image-loader - decodes PNG/JPEG stills for the image viewer.
//
// Decoding (file -> heap ARGB8888 buffer) runs on a background worker so a large
// image never freezes the UI; the caller turns the buffer into a VRAM texture on
// the main thread via uploadImageBuffer(). Also provides the format check and a
// sorted directory listing used to populate the viewer's navigation set.

#include "gfx.h"

// Returns 1 if the filename's extension is a viewer-supported image format
// (png, jpg, jpeg), 0 otherwise. Case-insensitive.
int isSupportedImageFormat(const char *filename);

// Max bytes (incl. NUL) for a filename returned by listSupportedImages().
#define IMAGE_NAME_MAX 256

// Lists the supported image files directly inside dir, copying their names into
// names[] sorted case-insensitively. Writes at most maxCount entries and returns
// how many were found (capped at maxCount). Names only, not full paths.
int listSupportedImages(const char *dir, char names[][IMAGE_NAME_MAX], int maxCount);

// --- asynchronous decode ---------------------------------------------------
// Decodes happen on a background worker so the UI never freezes on a big image.
// The worker only produces a heap ARGB8888 buffer; the caller turns it into a
// VRAM texture on the main thread (uploadImageBuffer). All these functions are
// to be called from the main thread only.

typedef struct {
   void *pixels;   // ARGB8888 top-to-bottom; free with freeImageBuffer()
   int   w, h;
   int   pitch;    // source bytes per row (w * 4)
} ImageBuffer;

// Requests an async decode of path, superseding any in-flight request (the old
// one's result is discarded when it finishes). Lazily starts the worker.
void requestImageAsync(const char *path);

// Polls the most recent request. Returns 1 and fills out (caller then owns
// out->pixels) when it finished decoding; 0 while still decoding; -1 once if it
// failed. Results from superseded requests are never reported.
int pollImageAsync(ImageBuffer *out);

// Frees a buffer handed back by pollImageAsync().
void freeImageBuffer(ImageBuffer *buf);

// Uploads a decoded buffer into a VRAM texture (main thread). Returns a
// zero-initialised texture on failure (check .offset). Does not free buf.
GfxTexture uploadImageBuffer(const ImageBuffer *buf);

// --- PNG encode (image-encoder.c) ------------------------------------------
// Encodes a w*h ARGB8888 image (A8R8G8B8 byte order, the lib's native surface layout) to a PNG
// file via the SDK codec (cellPngEnc); alpha is forced opaque. Returns 0 on success. Synchronous.
// NOTE: linking an app that calls this requires -lpngenc_stub.
int savePngArgb(const char *path, const void *argb, int w, int h);

// As savePngArgb, but the source may have padded rows (srcPitch bytes per row, >= w*4). Lets a
// caller pass a framebuffer straight through without first repacking away the pitch padding.
int savePngArgbPitch(const char *path, const void *argb, int w, int h, int srcPitch);
