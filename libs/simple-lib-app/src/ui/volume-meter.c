// volume-meter - shared volume pill column (see ui/volume-meter.h).

#include "ui/volume-meter.h"
#include <stdio.h>              // snprintf (level number)
#include <sys/sys_time.h>       // sys_time_get_system_time (auto-hide)

#define VISIBLE_US    2500000ULL   // meter auto-hides this long after the last change
#define NUMBER_SIZE   22
#define PILL_W        31
#define PILL_H        10
#define PILL_PITCH    19           // pill height + vertical gap
#define SPEAKER_W     32
#define SPEAKER_H     29
#define SPEAKER_GAP   22           // gap below the lowest pill to the speaker glyph
#define COLOR_EMPTY   0x33FFFFFFu  // unfilled pill
#define COLOR_NUMBER  0xFFFFFFFFu

static void setNumberText(VolumeMeter *meter)
{
   char number[8];
   snprintf(number, sizeof number, "%d", meter->level);
   setLabelText(&meter->numberLabel, number);
}

void initVolumeMeter(VolumeMeter *meter, Font *font, GfxTexture sprites, SpriteRegion speakerSprite, uint32_t fillColor, int level)
{
   meter->fillColor = fillColor;
   meter->level     = level;
   meter->shownUs   = 0;
   meter->hasSpeaker = sprites.w > 0;
   if (meter->hasSpeaker) initImage(&meter->speaker, sprites, 0, 0, SPEAKER_W, SPEAKER_H, speakerSprite, GFX_FILTER_LINEAR);
   initLabel(&meter->numberLabel, font, 0, 0, 80, AUTO, NUMBER_SIZE, COLOR_NUMBER, TEXT_NOWRAP, "");
   setNumberText(meter);
}

void layoutVolumeMeter(VolumeMeter *meter, int screenW, int screenH)
{
   meter->pillX = (int)(screenW * 0.07f);
   int stackHeight = VOLUME_METER_PILLS * PILL_PITCH - (PILL_PITCH - PILL_H);
   meter->bottomY  = screenH / 2 - stackHeight / 2 + stackHeight - PILL_H;   // top-y of the lowest pill
}

int handleVolumeMeterInput(VolumeMeter *meter)
{
   if (isRepeatDue(&meter->repeat, getPadButtonState(PAD_BTN_UP)))   { stepVolumeMeter(meter, +1); return 1; }
   if (isRepeatDue(&meter->repeat, getPadButtonState(PAD_BTN_DOWN))) { stepVolumeMeter(meter, -1); return 1; }
   return 0;
}

int stepVolumeMeter(VolumeMeter *meter, int delta)
{
   int level = meter->level + delta;
   if (level < 0) level = 0;
   if (level > VOLUME_METER_PILLS) level = VOLUME_METER_PILLS;
   meter->level = level;
   setNumberText(meter);
   meter->shownUs = sys_time_get_system_time();
   return level;
}

void drawVolumeMeter(VolumeMeter *meter)
{
   if (meter->shownUs == 0 || sys_time_get_system_time() - meter->shownUs >= VISIBLE_US) return;

   // pills bottom-up: the lowest `level` are filled in the accent colour, the rest dim
   for (int i = 0; i < VOLUME_METER_PILLS; i++)
      fillGfxRectangle(meter->pillX, meter->bottomY - i * PILL_PITCH, PILL_W, PILL_H, i < meter->level ? meter->fillColor : COLOR_EMPTY);

   int columnCenterX = meter->pillX + PILL_W / 2;
   int topPillY      = meter->bottomY - (VOLUME_METER_PILLS - 1) * PILL_PITCH;
   drawLabelAt(&meter->numberLabel, columnCenterX - meter->numberLabel.tt.tex.w / 2, topPillY - NUMBER_SIZE - 14);
   if (meter->hasSpeaker) drawImageAt(&meter->speaker, columnCenterX - SPEAKER_W / 2, meter->bottomY + PILL_H + SPEAKER_GAP);
}

void freeVolumeMeter(VolumeMeter *meter)
{
   freeLabel(&meter->numberLabel);
}
