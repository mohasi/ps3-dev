#pragma once

// volume-meter - the shared left-edge volume pill column: a stack of pills filled bottom-up in an
// accent colour, the level number above, a speaker glyph below. Stepping (re)shows it and it
// auto-hides a moment after the last change. The meter owns only the UI - the caller applies the
// level to its own audio path (a mixer stream, the video PCM feed, ...) and persists it.

#include "gfx.h"
#include "font.h"
#include "ui/label.h"
#include "ui/image.h"
#include "button-repeat.h"

#define VOLUME_METER_PILLS 15   // meter height in pills; also the max volume level

typedef struct {
   Label    numberLabel;
   Image    speaker;
   int      hasSpeaker;       // 0 when no sprite sheet was available; the glyph is skipped
   uint32_t fillColor;
   int      level;            // 0..VOLUME_METER_PILLS
   uint64_t shownUs;          // last change, for the auto-hide (0 = hidden)
   int      pillX, bottomY;   // left edge of the pills; top-y of the lowest pill
   ButtonRepeat repeat;       // up/down auto-repeat state (handleVolumeMeterInput)
} VolumeMeter;

// speakerSprite comes from the app's sprite sheet; pass a zero-width `sprites` texture to skip the glyph.
void initVolumeMeter(VolumeMeter *meter, Font *font, GfxTexture sprites, SpriteRegion speakerSprite, uint32_t fillColor, int level);

// standard placement: pills at 7% of the screen width, the column centred vertically.
void layoutVolumeMeter(VolumeMeter *meter, int screenW, int screenH);

// clamps level + delta, re-rasterises the number (off the draw path) and (re)shows the meter.
// returns the new level; getVolumeMeterFraction converts it for an audio api.
int stepVolumeMeter(VolumeMeter *meter, int delta);

// steps the meter on the pad's up/down (with hold auto-repeat); returns 1 when a step fired, so the
// caller applies meter->level to its audio path and persists it.
int handleVolumeMeterInput(VolumeMeter *meter);

static inline float getVolumeMeterFraction(const VolumeMeter *meter) { return (float)meter->level / VOLUME_METER_PILLS; }

static inline void hideVolumeMeter(VolumeMeter *meter) { meter->shownUs = 0; }

// draws nothing once the auto-hide delay has passed.
void drawVolumeMeter(VolumeMeter *meter);

// releases the number label's texture; the meter can be re-initialised afterwards.
void freeVolumeMeter(VolumeMeter *meter);
