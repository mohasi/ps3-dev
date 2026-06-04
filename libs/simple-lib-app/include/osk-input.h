#pragma once

// osk-input - non-blocking wrapper around the system on-screen keyboard
// (cellOskDialog). oskInputBegin() shows the keyboard and returns immediately;
// the dialog lives on the system layer and is driven by the app's existing
// per-frame cellSysutilCheckCallback() (appPoll), so the rest of the UI keeps
// rendering and never blocks. when the user confirms or cancels, the dialog
// dismisses itself and the result is handed back through a completion callback.

#include <stdbool.h>

// called once when the keyboard closes. text is the confirmed UTF-8 string, or
// NULL if the user cancelled or left the field empty. the pointer is only valid
// for the duration of the call.
typedef void (*OskDoneCallback)(const char *text);

// shows the OSK with the given guide caption and initial field text (both UTF-8,
// either may be NULL) and the completion callback (may be NULL). no-op if the
// keyboard is already up. returns true once the dialog has been handed to the
// system, false if it could not be started. the text passed to onDone is raw -
// callers validate it themselves (the OSK has no in-place reject on the pad).
bool oskInputBegin(const char *caption, const char *initialText, OskDoneCallback onDone);

// true while the keyboard is on screen (from oskInputBegin until it unloads).
bool oskInputActive(void);
