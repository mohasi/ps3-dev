#include "atl.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int getAtlDuration(const RbcAtl *a)
{
    if (!a) return 0;
    long total = 0;
    for (int k = 0; k < a->keyCount; k++) total += a->keys[k].durMs;
    return (int)total;
}

// Map a warper + linear fraction f (0..1) to an eased fraction. linear is exact; the splash logo
// uses only linear + pause. ease/easein/easeout are the standard cosine/quarter-sine curves
// (a small, flagged approximation of Ren'Py's warper table for the rarer cases).
static float warp(unsigned char warper, float f)
{
    if (f <= 0.0f) return 0.0f;
    if (f >= 1.0f) return 1.0f;
    switch (warper)
    {
        case ATL_WARP_LINEAR:  return f;
        case ATL_WARP_EASE:    return 0.5f - 0.5f * (float)cos(M_PI * f);
        case ATL_WARP_EASEIN:  return 1.0f - (float)cos(f * (M_PI / 2.0));
        case ATL_WARP_EASEOUT: return (float)sin(f * (M_PI / 2.0));
        default:               return f;   // INSTANT/PAUSE never interpolate props
    }
}

// Returns a pointer to the running value of property `prop` in *s, applying defaults. Also sets the
// matching "has" flag for placement props so the renderer knows ATL overrode it.
static float *slot(AtlState *s, unsigned char prop)
{
    switch (prop)
    {
        case ATL_XPOS:    s->hasXpos = 1;    return &s->xpos;
        case ATL_YPOS:    s->hasYpos = 1;    return &s->ypos;
        case ATL_XANCHOR: s->hasXanchor = 1; return &s->xanchor;
        case ATL_YANCHOR: s->hasYanchor = 1; return &s->yanchor;
        case ATL_XALIGN:  s->hasXalign = 1;  return &s->xalign;
        case ATL_YALIGN:  s->hasYalign = 1;  return &s->yalign;
        case ATL_ZOOM:    return &s->zoom;
        case ATL_XZOOM:   return &s->xzoom;
        case ATL_YZOOM:   return &s->yzoom;
        case ATL_ALPHA:   return &s->alpha;
        case ATL_ROTATE:  return &s->rotate;
        default:          return (float *)0;
    }
}

void evaluateAtl(const RbcAtl *a, int elapsedMs, AtlState *out)
{
    // Engine defaults.
    out->zoom = out->xzoom = out->yzoom = 1.0f;
    out->alpha = 1.0f;
    out->rotate = 0.0f;
    out->hasXalign = out->hasYalign = 0; out->xalign = out->yalign = 0.0f;
    out->hasXpos = out->hasYpos = 0;     out->xpos = out->ypos = 0.0f;
    out->hasXanchor = out->hasYanchor = 0; out->xanchor = out->yanchor = 0.0f;

    if (!a || a->keyCount <= 0) return;

    long total = getAtlDuration(a);

    // Resolve the effective time t within one pass, honouring repeat.
    long t = elapsedMs < 0 ? 0 : elapsedMs;
    if (total > 0)
    {
        if (a->repeatCount == -1)            t = t % total;                       // loop forever
        else if (a->repeatCount > 0)         t = (t >= total * (long)a->repeatCount) ? total : (t % total);
        else                                  t = (t >= total) ? total : t;        // play once, then hold
    }
    else t = 0;

    // Forward pass: past keyframes snap to their targets (becoming the running value); the active
    // keyframe interpolates; future keyframes are not applied.
    long acc = 0;
    for (int k = 0; k < a->keyCount; k++)
    {
        const RbcAtlKey *key = &a->keys[k];
        int d = key->durMs;
        float f;
        if (t >= acc + d)      f = 1.0f;                       // fully elapsed
        else if (t <= acc)     f = 0.0f;                       // not yet reached
        else                   f = (d > 0) ? (float)(t - acc) / (float)d : 1.0f;

        if (f > 0.0f)
        {
            float w = warp(key->warper, f);
            for (int j = 0; j < key->propCount; j++)
            {
                float *cur = slot(out, key->props[j].prop);
                if (!cur) continue;
                float target = key->props[j].milli / 1000.0f;
                *cur = *cur + (target - *cur) * w;   // f==1 -> exactly target (new running value)
            }
        }

        acc += d;
        if (t < acc) break;   // active keyframe handled; stop before future ones
    }
}
