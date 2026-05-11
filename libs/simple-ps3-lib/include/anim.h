#pragma once

// animation - tween values over time with easing

#include <stdint.h>

// ---------------------------------------------------------------------
// easing curves
// ---------------------------------------------------------------------

typedef enum {
    EASE_LINEAR,
    EASE_IN_QUAD,
    EASE_OUT_QUAD,
    EASE_IN_OUT_QUAD,
    EASE_IN_CUBIC,
    EASE_OUT_CUBIC,
    EASE_IN_OUT_CUBIC,
    EASE_OUT_BACK,
    EASE_OUT_BOUNCE,
    EASE_OUT_ELASTIC
} Easing;

float applyEasing(float t, Easing e);

// ---------------------------------------------------------------------
// color interpolation
// ---------------------------------------------------------------------

uint32_t interpolateColor(uint32_t from, uint32_t to, float t);

// ---------------------------------------------------------------------
// animation container
// ---------------------------------------------------------------------

#define ANIM_MAX 16

typedef enum {
    ANIM_ONCE,
    ANIM_LOOP,
    ANIM_PINGPONG
} AnimRepeat;

typedef struct { int id; } AnimHandle;
typedef void (*AnimCallback)(AnimHandle self);

static const AnimHandle ANIM_INVALID = { -1 };

typedef struct {
    int active;
    int paused;
    int id;
    float *target;
    float from;
    float to;
    uint64_t startUs;
    uint64_t durationUs;
    uint64_t pausedElapsed;
    Easing easing;
    AnimRepeat repeat;
    AnimCallback onDone;
    int forward;
} AnimSlot;

typedef struct {
    AnimSlot slots[ANIM_MAX];
    int nextId;
} Anims;

AnimHandle setAnim(Anims *a, float *target, float from, float to,
                   uint64_t ms, Easing easing, AnimRepeat repeat,
                   AnimCallback onDone);
void cancelAnim(Anims *a, AnimHandle handle);
int  isAnimDone(Anims *a, AnimHandle handle);
void cancelAllAnims(Anims *a);
void pauseAnim(Anims *a);
void resumeAnim(Anims *a);
void updateAnim(Anims *a);
