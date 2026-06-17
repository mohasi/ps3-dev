#include "scene-draw.h"

#include <string.h>           // strcmp
#include <sys/sys_time.h>

#include "gfx.h"
#include "colors.h"
#include "gui.h"              // gui.nativeW/H, getGuiAssetScale
#include "assets.h"          // getBg, getBgSolid, getSpriteCount, getSpriteAt, Sprite
#include "atl.h"             // evaluateAtl (sprite ATL animation)

// The scene is rendered into a double-buffered offscreen target each frame and blitted to the
// screen, so the previously presented buffer is always available as the "old" scene a `with`
// transition dissolves from.
static GfxRenderTarget sceneRT[2];
static int rtCur;          // buffer the current frame renders into
static int rtOld;          // last presented buffer (the outgoing scene)
static int rtReady;        // both targets allocated
static int presentedOnce;  // a scene has been shown (so rtOld is valid)

// Active `with` transition. Durations are Ren'Py engine defaults (dissolve = Dissolve(0.5);
// fade = Fade(0.5,0,0.5) = 1.0s through black) -- universal constants, so they live here.
static int      transitionActive;
static uint64_t transStartUs;
static uint64_t transDurUs;
static int      transFade;        // 1 = fade through black, 0 = cross-fade (dissolve)
static int      transOldBlack;    // no prior frame: dissolve/fade FROM black
// Screen shake (with vpunch / hpunch). TRANSLATED from the engine's universal definitions
// (renpy/common/00definitions.rpy): vpunch = Move((0,10),(0,-10),.10,bounce,repeat,delay=.275);
// hpunch = Move((15,0),(-15,0),...). So the whole frame oscillates between +A and -A native px,
// each leg 0.10s, for 0.275s. shakeKind: 0 none, 1 vpunch (±10 vertical), 2 hpunch (±15 horizontal).
static int      shakeKind;

// `renpy.pause(N)` hold. pauseDurUs 0 = no duration -> wait for input only.
static uint64_t pauseStartUs;
static uint64_t pauseDurUs;

void initScene(void)
{
   rtCur = 0; rtOld = 1; presentedOnce = 0; transitionActive = 0;   // distinct buffers; first transition from black
   rtReady = (createGfxRenderTarget(&sceneRT[0], getGfxScreenWidth(), getGfxScreenHeight()) == 0 &&
              createGfxRenderTarget(&sceneRT[1], getGfxScreenWidth(), getGfxScreenHeight()) == 0);
}

void freeScene(void)
{
   if (!rtReady) return;
   finishGfx();
   freeGfxRenderTarget(&sceneRT[0]);
   freeGfxRenderTarget(&sceneRT[1]);
   rtReady = 0;
}

void getSceneContentRect(int *cx, int *cy, int *cw, int *ch)
{
   int screenW = getGfxScreenWidth();
   int screenH = getGfxScreenHeight();
   int nativeW = gui.nativeW, nativeH = gui.nativeH;
   if (nativeW <= 0 || nativeH <= 0)
   {
      GfxTexture bg;                              // no manifest native res: use the bg aspect
      if (getBg(&bg) && bg.w > 0 && bg.h > 0) { nativeW = bg.w; nativeH = bg.h; }
      else { *cx = 0; *cy = 0; *cw = screenW; *ch = screenH; return; }
   }
   float scaleX = (float)screenW / (float)nativeW;
   float scaleY = (float)screenH / (float)nativeH;
   float scale  = scaleX < scaleY ? scaleX : scaleY;
   *cw = (int)(nativeW * scale + 0.5f);
   *ch = (int)(nativeH * scale + 0.5f);
   *cx = (screenW - *cw) / 2;
   *cy = (screenH - *ch) / 2;
}

// Maps a `with` transition name to its kind + duration. Only the two engine defaults are
// translated (dissolve, fade); anything else returns 0 = play it instantly. `name` is the raw
// expression the converter baked into the With op.
static int transitionFor(const char *name, uint64_t *durUs, int *fade)
{
   if (!name) return 0;
   if (strcmp(name, "dissolve") == 0) { *durUs = 500000;  *fade = 0; return 1; }
   if (strcmp(name, "fade") == 0)     { *durUs = 1000000; *fade = 1; return 1; }
   return 0;
}

// Blits a full-screen render target over the content area at the given alpha.
static void drawSceneRT(const GfxRenderTarget *rt, uint32_t alpha)
{
   drawGfxTexture(0, 0, getGfxScreenWidth(), getGfxScreenHeight(), rt->tex,
                  0.0f, 0.0f, 1.0f, 1.0f, (alpha << 24) | 0x00FFFFFFu, GFX_FILTER_LINEAR);
}

// The transition's outgoing scene: the previous frame, or solid black when there isn't one yet
// (so an opening `scene ... with dissolve/fade` fades up from black).
static void drawOldScene(void)
{
   if (transOldBlack) fillGfxRectangle(0, 0, getGfxScreenWidth(), getGfxScreenHeight(), 0xFF000000u);
   else               drawSceneRT(&sceneRT[rtOld], 255);
}

// Draw the whole frame's content: letterbox, background, sprites, then the UI overlay on top.
static void drawSceneToTarget(SceneOverlay overlay)
{
   // Letterbox: black bars where the aspect-fit background doesn't reach.
   GfxTexture bg;
   uint32_t solid;
   int haveBg = getBg(&bg);
   int haveSolid = !haveBg && getBgSolid(&solid);   // `scene black` etc.
   clearGfx(0xFF000000);   // Ren'Py's root layer clears to black (config.default_background); no tint

   int cx, cy, cw, ch; getSceneContentRect(&cx, &cy, &cw, &ch);
   if (haveBg)         drawGfxTexture(cx, cy, cw, ch, bg, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
   else if (haveSolid) fillGfxRectangle(cx, cy, cw, ch, solid);

   // Character sprites at their NATIVE size (scaled with the rest of the game surface; assetScale
   // corrects for converter pre-scaling). Placement translated in assets.c: x = xalign*(cw-sw),
   // bottom-anchored. Drawn before the UI so text sits on top.
   float assetScale = getGuiAssetScale(cw);
   for (int i = 0; i < getSpriteCount(); i++)
   {
      const Sprite *sprite = getSpriteAt(i);
      if (sprite->tex.h <= 0) continue;

      // ATL animation (inline `show x:` block): time-varying zoom/align/alpha on top of placement.
      AtlState st;
      int elapsedMs = sprite->atl ? (int)((sys_time_get_system_time() - sprite->atlStartUs) / 1000) : 0;
      evaluateAtl(sprite->atl, elapsedMs, &st);

      float zx = st.xzoom * st.zoom, zy = st.yzoom * st.zoom;     // zoom multiplies the rendered size
      int sw = (int)(sprite->tex.w * assetScale * zx + 0.5f);
      int sh = (int)(sprite->tex.h * assetScale * zy + 0.5f);
      float xa = st.hasXalign ? st.xalign : sprite->xalign;       // ATL align overrides the `at` placement
      float ya = st.hasYalign ? st.yalign : sprite->yalign;       // yalign: 1.0 bottom / 0.5 centre / 0.0 top (or <0/>1 off-screen)
      int sx = cx + (int)(xa * (float)(cw - sw) + 0.5f);
      int sy = cy + (int)(ya * (float)(ch - sh) + 0.5f);
      uint32_t tint = COLOR_WHITE;
      if (st.alpha < 0.999f)
      {
         int aa = (int)(st.alpha * 255.0f + 0.5f);
         if (aa < 0) aa = 0; else if (aa > 255) aa = 255;
         tint = ((uint32_t)aa << 24) | 0x00FFFFFFu;
      }
      drawGfxTexture(sx, sy, sw, sh, sprite->tex, 0.0f, 0.0f, 1.0f, 1.0f, tint, GFX_FILTER_LINEAR);
   }

   overlay(cx, cy, cw, ch);   // the dialogue/menu UI (or nothing, during a transition/pause)
}

void renderSceneFrame(SceneOverlay overlay)
{
   if (!rtReady) { drawSceneToTarget(overlay); return; }   // no targets: draw straight to screen

   beginGfxRenderTarget(&sceneRT[rtCur]);
   drawSceneToTarget(overlay);
   endGfxRenderTarget();

   if (transitionActive && shakeKind)
   {
      // Screen shake: redraw the just-rendered frame offset by an oscillation between +A and -A
      // native px (each 0.10s leg), exposing black at the edges (the layer beneath). Holds rtCur.
      uint64_t el = sys_time_get_system_time() - transStartUs;
      uint64_t m = el % 200000ull;   // full bounce cycle = 2 * 0.10s leg
      float v = (m < 100000ull) ? (1.0f - 2.0f * (m / 100000.0f))
                          : (-1.0f + 2.0f * ((float)(m - 100000ull) / 100000.0f));
      int cx, cy, cw, ch; getSceneContentRect(&cx, &cy, &cw, &ch);
      float s = getGuiScale(cw);
      int amp = (shakeKind == 1) ? 10 : 15;   // vpunch ±10 vertical, hpunch ±15 horizontal
      int off = (int)(v * amp * s + (v >= 0 ? 0.5f : -0.5f));
      int dx = (shakeKind == 2) ? off : 0;
      int dy = (shakeKind == 1) ? off : 0;
      int W = getGfxScreenWidth(), H = getGfxScreenHeight();
      fillGfxRectangle(0, 0, W, H, 0xFF000000u);
      drawGfxTexture(dx, dy, W, H, sceneRT[rtCur].tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
      return;   // hold rtCur stable while the shake plays
   }
   if (transitionActive)
   {
      // Dissolve old -> new, or fade old -> black -> new, over the transition's duration.
      uint64_t elapsed = sys_time_get_system_time() - transStartUs;
      float progress = transDurUs ? (float)elapsed / (float)transDurUs : 1.0f;
      if (progress > 1.0f) progress = 1.0f;
      int screenW = getGfxScreenWidth(), screenH = getGfxScreenHeight();
      if (transFade)
      {
         if (progress < 0.5f) { drawOldScene();                  fillGfxRectangle(0, 0, screenW, screenH, (uint32_t)(progress * 2.0f * 255.0f) << 24); }
         else                 { drawSceneRT(&sceneRT[rtCur], 255); fillGfxRectangle(0, 0, screenW, screenH, (uint32_t)((1.0f - progress) * 2.0f * 255.0f) << 24); }
      }
      else
      {
         drawOldScene();
         drawSceneRT(&sceneRT[rtCur], (uint32_t)(progress * 255.0f + 0.5f));
      }
      return;   // hold rtCur/rtOld stable while the transition plays
   }

   // Normal present: show this scene and remember it as the next "old" frame.
   drawSceneRT(&sceneRT[rtCur], 255);
   rtOld = rtCur;
   rtCur ^= 1;
   presentedOnce = 1;
}

int beginSceneTransition(const char *name)
{
   if (!rtReady) return 0;
   shakeKind = 0;
   if (name && strcmp(name, "vpunch") == 0) shakeKind = 1;
   else if (name && strcmp(name, "hpunch") == 0) shakeKind = 2;
   if (shakeKind)   // vpunch/hpunch: shake the live frame for delay=0.275s (no scene cross-fade)
   {
      transStartUs     = sys_time_get_system_time();
      transDurUs       = 275000;
      transitionActive = 1;
      return 1;
   }
   uint64_t durUs; int fade;
   if (!transitionFor(name, &durUs, &fade)) return 0;
   transStartUs     = sys_time_get_system_time();
   transDurUs       = durUs;
   transFade        = fade;
   transOldBlack    = !presentedOnce;   // nothing presented yet -> dissolve/fade from black
   transitionActive = 1;
   return 1;
}

int hasSceneTransitionElapsed(void)
{
   return sys_time_get_system_time() - transStartUs >= transDurUs;
}

void endSceneTransition(void)
{
   presentedOnce = 1;   // the new scene is now a valid "old" for the next one
   rtOld = rtCur;
   rtCur ^= 1;
   transitionActive = 0;
   shakeKind = 0;
}

void beginScenePause(double seconds)
{
   pauseStartUs = sys_time_get_system_time();
   pauseDurUs   = seconds > 0.0 ? (uint64_t)(seconds * 1000000.0) : 0;
}

int hasScenePauseElapsed(void)
{
   return pauseDurUs > 0 && sys_time_get_system_time() - pauseStartUs >= pauseDurUs;
}

int captureSceneThumb(unsigned char *out, int outW, int outH)
{
   if (!out || outW <= 0 || outH <= 0) return -1;

   // Read the front display buffer: the last rendered + flipped frame, i.e. the game frame the menu
   // was opened over (it already holds letterbox + scene + dialogue). No re-render needed -- the menu
   // frame hasn't flipped yet, so the front buffer is still the game frame.
   int sw = 0, sh = 0, pitch = 0;
   const unsigned char *src = (const unsigned char *)getGfxDisplayBuffer(&sw, &sh, &pitch);
   if (!src || pitch <= 0 || sw <= 0 || sh <= 0) return -1;

   // Crop to the game content area (drop the black letterbox bars), box-average down to outW x outH.
   int cx, cy, cw, ch; getSceneContentRect(&cx, &cy, &cw, &ch);
   if (cx < 0) cx = 0;
   if (cy < 0) cy = 0;
   if (cx + cw > sw) cw = sw - cx;
   if (cy + ch > sh) ch = sh - cy;
   if (cw <= 0 || ch <= 0) return -1;

   for (int ty = 0; ty < outH; ++ty)
   {
      int sy0 = cy + ty * ch / outH;
      int sy1 = cy + (ty + 1) * ch / outH;
      if (sy1 <= sy0) sy1 = sy0 + 1;
      if (sy1 > cy + ch) sy1 = cy + ch;
      for (int tx = 0; tx < outW; ++tx)
      {
         int sx0 = cx + tx * cw / outW;
         int sx1 = cx + (tx + 1) * cw / outW;
         if (sx1 <= sx0) sx1 = sx0 + 1;
         if (sx1 > cx + cw) sx1 = cx + cw;
         unsigned int s0 = 0, s1 = 0, s2 = 0, s3 = 0, cnt = 0;
         for (int yy = sy0; yy < sy1; ++yy)
         {
            const unsigned char *row = src + (size_t)yy * pitch + (size_t)sx0 * 4;
            for (int xx = sx0; xx < sx1; ++xx)
            {
               s0 += row[0]; s1 += row[1]; s2 += row[2]; s3 += row[3];
               row += 4; cnt++;
            }
         }
         unsigned char *o = out + ((size_t)ty * outW + tx) * 4;
         if (cnt) { o[0] = (unsigned char)(s0 / cnt); o[1] = (unsigned char)(s1 / cnt); o[2] = (unsigned char)(s2 / cnt); o[3] = (unsigned char)(s3 / cnt); }
         else     { o[0] = o[1] = o[2] = 0; o[3] = 255; }
      }
   }
   return 0;
}
