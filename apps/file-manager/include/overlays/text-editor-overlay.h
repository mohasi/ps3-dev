#pragma once

// text-editor-overlay - full-screen text viewer with line numbers.
// Slice 1: read-only. Loads the whole file into memory, splits it on '\n' /
// '\r\n', and shows a scrollable, line-numbered view in a bordered panel.
// No editing, no search, no top/footer chrome yet - just the text box.

#include "gfx.h"
#include "overlay.h"

// One-time setup (fonts, labels, panel sprite). Call once during home screen init.
void initTextEditorOverlay(GfxTexture sprites);

// Opens the viewer on the given absolute text file path. Returns 0 on
// success, or -1 if the file could not be read (overlay stays shut).
int openTextEditor(const char *path);

// Global overlay instance (defined in the .c; extern here so the home screen
// can register it in its update/draw/term loops).
extern Overlay textEditorOverlay;
