// stats - on-screen diagnostics overlay (FPS, VRAM usage).
// Multi-line readout toggled with L3 + R3; off by default.
#include "ui/stats.h"
#include "font.h"
#include "gfx.h"
#include "pad.h"
#include "colors.h"
#include "string-utilities.h"
#include <string.h>
#include <sys/sys_time.h>

// Wrap width for the readout. The text rasterizer only sizes a multi-line
// surface (and so only honours '\n') when maxWidth > 0, so passing AUTO would
// collapse everything onto a single clipped line. This is comfortably wider
// than any line we draw; content is cropped to its true width on upload.
#define STATS_WRAP_WIDTH 400

static Font font;
static int posX, posY, fontSize;
static uint32_t color;
static TextTexture tt;
static int fps;
static int frameCount;
static uint64_t lastTime;
static char buf[256];
static char lastBuf[256];
static int visible;  // off by default

// appends an unsigned value with thousands separators (e.g. 1234567 -> 1,234,567).
static void appendInt(char *dst, int *pos, size_t v)
{
   char tmp[24];
   int len = 0;

   if (v == 0) { dst[(*pos)++] = '0'; return; }

   while (v > 0) { tmp[len++] = (char)('0' + (v % 10)); v /= 10; }

   for (int i = len - 1; i >= 0; i--) {
      dst[(*pos)++] = tmp[i];
      if (i > 0 && i % 3 == 0) dst[(*pos)++] = ',';
   }
}

static void appendLabel(char *dst, int *pos, const char *s)
{
   while (*s) dst[(*pos)++] = *s++;
}

static void buildBuf(void)
{
   int pos = 0;
   appendLabel(buf, &pos, "FPS: ");
   appendInt(buf, &pos, (size_t)fps);
   appendLabel(buf, &pos, "\nUsed: ");
   appendInt(buf, &pos, getUsedVram());
   appendLabel(buf, &pos, "\nFree: ");
   appendInt(buf, &pos, getFreeVram());
   appendLabel(buf, &pos, "\nLargest: ");
   appendInt(buf, &pos, getLargestFreeBlock());
   buf[pos] = 0;
}

void initStats(int x, int y, int size, uint32_t clr)
{
   font = openSystemFont(FONT_POP);
   posX = x;
   posY = y;
   fontSize = size;
   color = clr;
   fps = 0;
   frameCount = 0;
   lastTime = 0;
   visible = 0;
   buf[0] = 0;
   lastBuf[0] = 0;
   tt.valid = 0;
}

void updateStats(void)
{
   // Toggle on the frame either stick is clicked while both are down. Using
   // "pressed this frame AND both currently down" handles a simultaneous press
   // and a staggered one, and fires exactly once (not every held frame).
   int l3Down = isPadButtonPressed(PAD_BTN_L3) || isPadButtonHeld(PAD_BTN_L3);
   int r3Down = isPadButtonPressed(PAD_BTN_R3) || isPadButtonHeld(PAD_BTN_R3);
   int pressedThisFrame = isPadButtonPressed(PAD_BTN_L3) || isPadButtonPressed(PAD_BTN_R3);

   if (pressedThisFrame && l3Down && r3Down) {
      visible = !visible;
      if (visible) { frameCount = 0; lastTime = 0; }  // start a fresh FPS window
   }
}

void drawStats(void)
{
   if (!visible) return;

   uint64_t now = sys_time_get_system_time();
   frameCount++;

   if (lastTime == 0) {
      lastTime = now;
      return;
   }

   if (now - lastTime >= 1000000) {  // refresh once per second
      fps = frameCount;
      frameCount = 0;
      lastTime = now;

      buildBuf();
      if (strcmp(buf, lastBuf) != 0) {
         renderFont(&tt, &font, fontSize, buf, color, STATS_WRAP_WIDTH, TEXT_WRAP);
         strCopy(lastBuf, sizeof lastBuf, buf);
      }
   }

   if (tt.tex.w > 0)
      drawGfxTexture(posX, posY, tt.tex.w, tt.tex.h, tt.tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
}

void termStats(void)
{
   freeTextTexture(&tt);
   closeFont(&font);
}
