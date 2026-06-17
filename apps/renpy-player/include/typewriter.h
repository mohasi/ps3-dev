#pragma once

// Typewriter reveal for a single line of dialogue (config.default_text_cps). The line is
// rendered ONCE with renderFontTyped(... &tw.reveal); this then reveals it character by
// character over time by clipping the rendered texture at draw time -- no per-frame
// re-rasterising. Faithful to Ren'Py text.py: a fully laid-out line, revealing the first
// int(st * cps) characters in place.

#include <stdint.h>
#include "font.h"   // TextReveal, TextTexture

typedef struct {
   TextReveal reveal;     // per-item positions; fill via renderFontTyped(... &tw->reveal)
   int        cps;        // characters per second (0 = instant)
   int        total;      // reveal.count
   int        shown;      // characters currently revealed
   int        done;       // line fully revealed
   uint64_t   startUs;
} Typewriter;

// Begins the reveal. Call after rendering the line into its texture with renderFontTyped.
// `instant` (no cps, or a rollback re-show) shows the whole line immediately.
void startTypewriter(Typewriter *tw, int cps, int instant);

void tickTypewriter(Typewriter *tw);      // advance the reveal by elapsed time (call each frame)
void completeTypewriter(Typewriter *tw);  // reveal the whole line now (first advance press)
int  isTypewriterDone(const Typewriter *tw);

// Draws `tex` (the line's texture) revealed up to the current count, at (x, y).
void drawTypewriter(const Typewriter *tw, TextTexture *tex, int x, int y);
