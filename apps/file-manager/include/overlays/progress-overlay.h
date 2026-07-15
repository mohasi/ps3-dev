#pragma once

// progress-overlay - flat/metro modal for a background file task (see file-task.h): title, a big
// percentage, a themed progress bar, live stats (size done/total, speed, elapsed, estimated remaining)
// and a Cancel (circle) hint. startProgress spawns the task, shows the dialog, and calls onDone on the
// main thread when it finishes. circle requests cancellation.

#include "overlay.h"
#include "audio.h"
#include "file-task.h"

typedef void (*ProgressDoneCallback)(int cancelled);

void initProgressOverlay(Audio *clickSfx);
void startProgress(const char *title, const char *subtitle, TaskBody run, ProgressDoneCallback onDone);

// indeterminate "busy" variant for work that isn't a byte-tracked file task (e.g. a search walk with
// its own worker thread). Shows the same themed dialog with a title, a live status line and a Cancel
// (circle) hint - no percentage or bar. The overlay polls status() for the status line and done() for
// completion each frame, then hides itself and calls onDone(cancelled); circle calls onCancel() so the
// owner can stop its own work (done() should then report finished).
typedef const char *(*BusyStatusFn)(void);
typedef int         (*BusyDoneFn)(void);
void startBusyProgress(const char *title, BusyStatusFn status, BusyDoneFn done,
                       void (*onCancel)(void), ProgressDoneCallback onDone);

void rethemeProgressOverlay(void);   // recolour the dialog text for a live theme switch

extern Overlay progressOverlay;
