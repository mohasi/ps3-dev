// demo screen - showcases engine features
#include "screens/demo.h"
#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "pad-display.h"
#include "bars.h"
#include "bouncing-box.h"
#include "gradient-tri.h"
#include "animated-sprite.h"
#include "audio.h"
#include "font.h"
#include "ui/label.h"
#include "ui/circle.h"
#include "anim.h"
#include "overlays/sidepanel.h"
#include "screens/palette.h"
#include "screen-manager.h"

static GfxTexture makoto;
static Audio sfxMakoto;
static Audio bgm;
static Font pop;
static Anims anims;
static float circleX;
static float colorT;
static Circle circle;
static Label titleText;
static Label wrapText;
static Label ellipsisText;

static void initDemo(void)
{
   initAnimatedSprite();
   initBars();
   initBouncingBox();

   makoto = loadGfxTexture("/dev_hdd0/game/APPSMP001/USRDIR/makoto.png");

   sfxMakoto = loadAudio("/dev_hdd0/game/APPSMP001/USRDIR/makoto.wav", AUDIO_MEMORY);
   bgm = loadAudio("/dev_hdd0/game/APPSMP001/USRDIR/price.ogg", AUDIO_STREAM);

   pop = openSystemFont(FONT_POP);

   initLabel(&titleText, &pop, 40, 170, AUTO, AUTO, 20, COLOR_WHITE, TEXT_NOWRAP, "app-sample");
   initLabel(&wrapText, &pop, 500, 300, 350, AUTO, 14, COLOR_EMERALD_300, TEXT_WRAP, "The quick brown fox jumps over the lazy dog. This text should word wrap within the bounding box.");
   initLabel(&ellipsisText, &pop, 500, 380, 200, AUTO, 14, COLOR_AMBER_300, TEXT_NOWRAP_ELLIPSIS, "This long text gets cut off with an ellipsis at the end");

   initCircle(&circle, 800, 130, 25, COLOR_WHITE);

   initPadDisplay(&pop, 40, 240, 14, COLOR_SKY_300);

   circleX = 800.0f;
   colorT = 0.0f;
   setAnim(&anims, &circleX, 800.0f, 1800.0f, 2000, EASE_IN_OUT_QUAD, ANIM_PINGPONG, NULL);
   setAnim(&anims, &colorT, 0.0f, 1.0f, 800, EASE_IN_OUT_QUAD, ANIM_PINGPONG, NULL);
}

static void resumeDemo(void)
{
   resumeAudio(&sfxMakoto);
   resumeAudio(&bgm);
   resumeAnim(&anims);
}

static void updateDemo(void)
{
   updateAnim(&anims);

   if (isPadButtonPressed(PAD_BTN_CROSS))
      playAudio(&sfxMakoto, AUDIO_DEFAULT_VOLUME, AUDIO_DEFAULT_SPEED, AUDIO_NO_LOOP);

   if (isPadButtonPressed(PAD_BTN_START)) {
      if (bgm.state == AUDIO_STATE_PLAYING)
         stopAudio(&bgm);
      else
         playAudio(&bgm, AUDIO_DEFAULT_VOLUME, AUDIO_DEFAULT_SPEED, 1);
   }

   if (isPadButtonPressed(PAD_BTN_UP))
      raiseAudioMasterVolume(0.1f);

   if (isPadButtonPressed(PAD_BTN_DOWN))
      lowerAudioMasterVolume(0.1f);

   if (isPadButtonPressed(PAD_BTN_RIGHT)) {
      pushScreen(&paletteScreen);
      return;
   }

   if (isPadButtonPressed(PAD_BTN_SELECT)) {
      if (isOverlayVisible(&sidepanel))
         hideOverlay(&sidepanel);
      else
         showOverlay(&sidepanel);
   }

   updateBouncingBox();
   updateAnimatedSprite();
   updateOverlay(&sidepanel);
}

static void drawDemo(void)
{
   drawBars();
   drawBouncingBox();
   drawGradientTriangle();
   drawAnimatedSprite(500, 100);
   drawGfxTexture(100, 400, makoto.w, makoto.h, makoto, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
   drawLabel(&titleText);
   drawPadDisplay();

   drawLabel(&wrapText);
   drawLabel(&ellipsisText);

   moveCircle(&circle, (int)circleX, circle.cy);
   circle.fill = interpolateColor(COLOR_WHITE, COLOR_RED, colorT);
   drawCircle(&circle);

   drawOverlay(&sidepanel);
}

static void suspendDemo(void)
{
   pauseAudio(&sfxMakoto);
   pauseAudio(&bgm);
   pauseAnim(&anims);
}

static void termDemo(void)
{
   termOverlay(&sidepanel);
   cancelAllAnims(&anims);

   termAnimatedSprite();
   termPadDisplay();
   freeLabel(&titleText);
   freeLabel(&wrapText);
   freeLabel(&ellipsisText);

   finishGfx();
   freeGfxTexture(&makoto);

   closeFont(&pop);
   freeAudio(&sfxMakoto);
   freeAudio(&bgm);
}

Screen demoScreen = { initDemo, resumeDemo, updateDemo, drawDemo, suspendDemo, termDemo, SCREEN_TERMINATED };
