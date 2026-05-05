// animation - tween values over time with easing
#include "anim.h"
#include <sys/sys_time.h>
#include <math.h>

// ---------------------------------------------------------------------
// easing curves
// ---------------------------------------------------------------------

float applyEasing(float t, Easing e)
{
    switch (e) {
    case EASE_LINEAR:
        return t;
    case EASE_IN_QUAD:
        return t * t;
    case EASE_OUT_QUAD:
        return t * (2.0f - t);
    case EASE_IN_OUT_QUAD:
        if (t < 0.5f) return 2.0f * t * t;
        return -1.0f + (4.0f - 2.0f * t) * t;
    case EASE_IN_CUBIC:
        return t * t * t;
    case EASE_OUT_CUBIC: {
        float u = t - 1.0f;
        return u * u * u + 1.0f;
    }
    case EASE_IN_OUT_CUBIC:
        if (t < 0.5f) return 4.0f * t * t * t;
        return (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
    case EASE_OUT_BACK: {
        float s = 1.70158f;
        float u = t - 1.0f;
        return u * u * ((s + 1.0f) * u + s) + 1.0f;
    }
    case EASE_OUT_BOUNCE: {
        if (t < 1.0f / 2.75f)
            return 7.5625f * t * t;
        if (t < 2.0f / 2.75f) {
            float u = t - 1.5f / 2.75f;
            return 7.5625f * u * u + 0.75f;
        }
        if (t < 2.5f / 2.75f) {
            float u = t - 2.25f / 2.75f;
            return 7.5625f * u * u + 0.9375f;
        }
        float u = t - 2.625f / 2.75f;
        return 7.5625f * u * u + 0.984375f;
    }
    case EASE_OUT_ELASTIC: {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        float p = 0.3f;
        float s = p / 4.0f;
        return powf(2.0f, -10.0f * t) * sinf((t - s) * (2.0f * 3.14159265f) / p) + 1.0f;
    }
    }
    return t;
}

// ---------------------------------------------------------------------
// color interpolation
// ---------------------------------------------------------------------

uint32_t interpolateColor(uint32_t from, uint32_t to, float t)
{
    if (t <= 0.0f) return from;
    if (t >= 1.0f) return to;
    float fa = (float)((from >> 24) & 0xFF);
    float fr = (float)((from >> 16) & 0xFF);
    float fg = (float)((from >>  8) & 0xFF);
    float fb = (float)( from        & 0xFF);
    float ta = (float)((to >> 24) & 0xFF);
    float tr = (float)((to >> 16) & 0xFF);
    float tg = (float)((to >>  8) & 0xFF);
    float tb = (float)( to        & 0xFF);
    uint8_t a = (uint8_t)(fa + (ta - fa) * t);
    uint8_t r = (uint8_t)(fr + (tr - fr) * t);
    uint8_t g = (uint8_t)(fg + (tg - fg) * t);
    uint8_t b = (uint8_t)(fb + (tb - fb) * t);
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// ---------------------------------------------------------------------
// animation container
// ---------------------------------------------------------------------

AnimHandle animSet(Anims *a, float *target, float from, float to,
                   uint64_t ms, Easing easing, AnimRepeat repeat,
                   AnimCallback onDone)
{
    for (int i = 0; i < ANIM_MAX; i++) {
        if (!a->slots[i].active) {
            a->slots[i].active = 1;
            a->slots[i].paused = 0;
            a->slots[i].id = a->nextId;
            a->slots[i].target = target;
            a->slots[i].from = from;
            a->slots[i].to = to;
            a->slots[i].startUs = sys_time_get_system_time();
            a->slots[i].durationUs = ms * 1000;
            a->slots[i].pausedElapsed = 0;
            a->slots[i].easing = easing;
            a->slots[i].repeat = repeat;
            a->slots[i].onDone = onDone;
            a->slots[i].forward = 1;
            *target = from;
            AnimHandle h = { a->nextId };
            a->nextId++;
            return h;
        }
    }
    return ANIM_INVALID;
}

void animCancel(Anims *a, AnimHandle handle)
{
    for (int i = 0; i < ANIM_MAX; i++) {
        if (a->slots[i].active && a->slots[i].id == handle.id) {
            a->slots[i].active = 0;
            return;
        }
    }
}

int animDone(Anims *a, AnimHandle handle)
{
    for (int i = 0; i < ANIM_MAX; i++) {
        if (a->slots[i].active && a->slots[i].id == handle.id)
            return 0;
    }
    return 1;
}

void animCancelAll(Anims *a)
{
    for (int i = 0; i < ANIM_MAX; i++)
        a->slots[i].active = 0;
}

void animPause(Anims *a)
{
    uint64_t now = sys_time_get_system_time();
    for (int i = 0; i < ANIM_MAX; i++) {
        if (a->slots[i].active && !a->slots[i].paused) {
            a->slots[i].paused = 1;
            a->slots[i].pausedElapsed = now - a->slots[i].startUs;
        }
    }
}

void animResume(Anims *a)
{
    uint64_t now = sys_time_get_system_time();
    for (int i = 0; i < ANIM_MAX; i++) {
        if (a->slots[i].active && a->slots[i].paused) {
            a->slots[i].paused = 0;
            a->slots[i].startUs = now - a->slots[i].pausedElapsed;
        }
    }
}

void animUpdate(Anims *a)
{
    uint64_t now = sys_time_get_system_time();
    for (int i = 0; i < ANIM_MAX; i++) {
        if (!a->slots[i].active || a->slots[i].paused) continue;

        uint64_t elapsed = now - a->slots[i].startUs;
        float t = (float)elapsed / (float)a->slots[i].durationUs;

        if (t >= 1.0f) {
            switch (a->slots[i].repeat) {
            case ANIM_ONCE:
                *a->slots[i].target = a->slots[i].to;
                a->slots[i].active = 0;
                if (a->slots[i].onDone) {
                    AnimHandle h = { a->slots[i].id };
                    a->slots[i].onDone(h);
                }
                continue;
            case ANIM_LOOP:
                a->slots[i].startUs = now;
                t = 0.0f;
                break;
            case ANIM_PINGPONG:
                a->slots[i].startUs = now;
                a->slots[i].forward = !a->slots[i].forward;
                t = 0.0f;
                break;
            }
        }

        float eased = applyEasing(t, a->slots[i].easing);
        if (!a->slots[i].forward) eased = 1.0f - eased;

        float from = a->slots[i].from;
        float to = a->slots[i].to;
        *a->slots[i].target = from + (to - from) * eased;
    }
}
