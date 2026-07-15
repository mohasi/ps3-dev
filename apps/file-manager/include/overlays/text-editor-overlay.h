#pragma once

// text-editor-overlay - full-screen text editor with line numbers. Loads the
// whole file into memory and shows a scrollable, line-numbered view in a
// bordered panel, with its own Edit/Save/Exit footer row. No search yet.

#include "overlay.h"

// One-time setup (fonts, labels, header icon). Call once during home screen init.
void initTextEditorOverlay(void);

// re-applies the active theme to the pre-rendered labels, for a live theme switch.
void rethemeTextEditorOverlay(void);

// Opens the viewer on the given absolute text file path. Returns 0 on success,
// or -1 if the file could not be read or is larger than MAX_EDITABLE_FILE_SIZE
// (overlay stays shut).
int openTextEditor(const char *path);

// Global overlay instance (defined in the .c; extern here so the home screen
// can register it in its update/draw/term loops).
extern Overlay textEditorOverlay;
