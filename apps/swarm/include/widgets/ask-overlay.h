#pragma once

// ask-overlay - a question in the middle of the screen with up to three answers, one per button.
//
// The file manager has one of these of its own, tied to its themes. If a third app wants one, this
// is the copy worth moving into simple-lib-app.

#include "font.h"
#include "overlay.h"

typedef enum {
   ANSWER_CROSS,
   ANSWER_SQUARE,
   ANSWER_CIRCLE
} Answer;

typedef void (*AnswerCallback)(Answer answer);

void initAskOverlay(Font *font);

// Puts the question up and calls back with whichever button was pressed. A label of NULL leaves that
// button out, so a plain yes/no question passes NULL for the square one.
void ask(const char *title, const char *crossText, const char *squareText, const char *circleText,
         AnswerCallback onAnswer);

extern Overlay askOverlay;
