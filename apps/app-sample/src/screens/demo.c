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

    sfxMakoto = loadSfx("/dev_hdd0/game/APPSMP001/USRDIR/makoto.wav", SFX_MEMORY);
    bgm = loadSfx("/dev_hdd0/game/APPSMP001/USRDIR/price.ogg", SFX_STREAM);

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
    resumeSfx(&sfxMakoto);
    resumeSfx(&bgm);
    resumeAnim(&anims);
}

static void updateDemo(void)
{
    updateAnim(&anims);

    if (pad.btn.cross == BTN_PRESSED)
        playSfx(&sfxMakoto, SFX_DEFAULT_VOLUME, SFX_DEFAULT_SPEED, SFX_LOOP);

    if (pad.btn.start == BTN_PRESSED) {
        if (bgm.state == SFX_STATE_PLAYING)
            stopSfx(&bgm);
        else
            playSfx(&bgm, SFX_DEFAULT_VOLUME, SFX_DEFAULT_SPEED, 1);
    }

    if (pad.btn.up == BTN_PRESSED)
        raiseSfxMasterVolume(0.1f);

    if (pad.btn.down == BTN_PRESSED)
        lowerSfxMasterVolume(0.1f);

    if (pad.btn.right == BTN_PRESSED) {
        pushScreen(&paletteScreen);
        return;
    }

    if (pad.btn.select == BTN_PRESSED) {
        if (sidepanel.status == OVERLAY_VISIBLE)
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
    pauseSfx(&sfxMakoto);
    pauseSfx(&bgm);
    pauseAnim(&anims);
}

static void termDemo(void)
{
    termOverlay(&sidepanel);
    cancelAllAnims(&anims);
    closeFont(&pop);
    freeSfx(&sfxMakoto);
    freeSfx(&bgm);
}

Screen demoScreen = { initDemo, resumeDemo, updateDemo, drawDemo, suspendDemo, termDemo, SCREEN_TERMINATED };
