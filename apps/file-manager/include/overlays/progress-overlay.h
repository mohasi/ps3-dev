#pragma once

// progress-overlay - title, subtitle, a framed progress bar with a percentage,
// a separator and a Cancel (circle) button for a background file task (see
// file-task.h). startProgress spawns the task, shows the dialog, and calls
// onDone on the main thread when it finishes. circle requests cancellation.

#include "overlay.h"
#include "gfx.h"
#include "audio.h"
#include "file-task.h"

typedef void (*ProgressDoneCallback)(int cancelled);

void initProgressOverlay(GfxTexture spritesheet, Audio *clickSfx);
void startProgress(const char *title, const char *subtitle, TaskBody run, ProgressDoneCallback onDone);

extern Overlay progressOverlay;
