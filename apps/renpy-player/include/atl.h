#pragma once

#include "rbc.h"

// ATL playback: replays a compiled ATL keyframe program (rbc.h RbcAtl) at a given elapsed time.
// Pure + stateless -- the caller tracks each animated displayable's start time and calls evaluateAtl
// every frame. Faithful to Ren'Py ATL: each keyframe interpolates its listed properties from their
// running value to the target over the keyframe's duration (via its warper); unlisted properties
// persist; `pause` only advances time; `repeat` loops. Properties not used by a keyframe keep their
// engine defaults (zoom/xzoom/yzoom/alpha = 1, rotate = 0, placement unset).

typedef struct {
    float zoom, xzoom, yzoom;        // size multipliers (default 1.0)
    float alpha;                     // 0..1 (default 1.0)
    float rotate;                    // degrees (default 0; not yet applied by the renderer)
    int   hasXalign, hasYalign;  float xalign, yalign;     // fractional placement (Ren'Py align)
    int   hasXpos,  hasYpos;     float xpos,  ypos;        // fractional position (best-effort)
    int   hasXanchor, hasYanchor; float xanchor, yanchor;
} AtlState;

// Evaluate program `a` at `elapsedMs` into *out (engine defaults when a is NULL/empty).
void evaluateAtl(const RbcAtl *a, int elapsedMs, AtlState *out);

// Total duration (ms) of one pass through the program (0 if none).
int  getAtlDuration(const RbcAtl *a);
