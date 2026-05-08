// demo screen - showcases engine features
#include "screens/demo.h"
#include "gfx.h"
#include "colors.h"
#include "pad-input.h"
#include "bars.h"
#include "bouncing-box.h"
#include "gradient-tri.h"
#include "animated-sprite.h"
#include "audio.h"
#include "font.h"
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
static GfxTexture titleText;
static GfxTexture wrapText;
static GfxTexture ellipsisText;

static void demoInit(void)
{
    initAnimatedSprite();

    makoto = gfxLoadTexture("/dev_hdd0/game/APPSMP001/USRDIR/makoto.png");

    sfxMakoto = sfxLoad("/dev_hdd0/game/APPSMP001/USRDIR/makoto.wav", SFX_MEMORY);
    bgm = sfxLoad("/dev_hdd0/game/APPSMP001/USRDIR/price.ogg", SFX_STREAM);

    pop = fontOpenSystem(FONT_POP);

    titleText = fontToTexture(AUTO, AUTO, "app-sample", &pop, 20, COLOR_WHITE, TEXT_NOWRAP);
    wrapText = fontToTexture(350, AUTO, "The quick brown fox jumps over the lazy dog. This text should word wrap within the bounding box.", &pop, 14, COLOR_EMERALD_300, TEXT_WRAP);
    ellipsisText = fontToTexture(200, AUTO, "This long text gets cut off with an ellipsis at the end", &pop, 14, COLOR_AMBER_300, TEXT_NOWRAP_ELLIPSIS);

    circleX = 800.0f;
    colorT = 0.0f;
    animSet(&anims, &circleX, 800.0f, 1800.0f, 2000, EASE_IN_OUT_QUAD, ANIM_PINGPONG, NULL);
    animSet(&anims, &colorT, 0.0f, 1.0f, 800, EASE_IN_OUT_QUAD, ANIM_PINGPONG, NULL);
}

static void demoResume(void)
{
    sfxResume(&sfxMakoto);
    sfxResume(&bgm);
    animResume(&anims);
}

static void demoUpdate(void)
{
    animUpdate(&anims);

    if (pad.btn.cross == BTN_PRESSED)
        sfxPlay(&sfxMakoto, SFX_DEFAULT_VOLUME, SFX_DEFAULT_SPEED, SFX_LOOP);

    if (pad.btn.start == BTN_PRESSED) {
        if (bgm.state == SFX_STATE_PLAYING)
            sfxStop(&bgm);
        else
            sfxPlay(&bgm, SFX_DEFAULT_VOLUME, SFX_DEFAULT_SPEED, 1);
    }

    if (pad.btn.up == BTN_PRESSED)
        sfxMasterVolumeUp(0.1f);

    if (pad.btn.down == BTN_PRESSED)
        sfxMasterVolumeDown(0.1f);

    if (pad.btn.right == BTN_PRESSED) {
        pushScreen(&paletteScreen);
        return;
    }

    if (pad.btn.select == BTN_PRESSED) {
        if (sidepanel.status == OVERLAY_VISIBLE)
            overlayHide(&sidepanel);
        else
            overlayShow(&sidepanel);
    }

    moveBouncingBox();
    updateAnimatedSprite();
    overlayUpdate(&sidepanel);
}

static void demoDraw(void)
{
    drawBars();
    drawBouncingBox();
    drawGradientTriangle();
    drawAnimatedSprite(500, 100);
    gfxDrawTexture(100, 400, makoto.w, makoto.h, makoto, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
    gfxDrawTexture(40, 170, titleText.w, titleText.h, titleText, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
    padDraw(40, 240, &pop, 14, COLOR_SKY_300);

    gfxDrawTexture(500, 300, wrapText.w, wrapText.h, wrapText, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
    gfxDrawTexture(500, 380, ellipsisText.w, ellipsisText.h, ellipsisText, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);

    uint32_t circleColor = interpolateColor(COLOR_WHITE, COLOR_RED, colorT);
    gfxFillCircle((int)circleX, 130, 25, circleColor);

    overlayDraw(&sidepanel);
}

static void demoSuspend(void)
{
    sfxPause(&sfxMakoto);
    sfxPause(&bgm);
    animPause(&anims);
}

static void demoTerm(void)
{
    overlayTerm(&sidepanel);
    animCancelAll(&anims);
    fontClose(&pop);
    sfxFree(&sfxMakoto);
    sfxFree(&bgm);
}

Screen demoScreen = { demoInit, demoResume, demoUpdate, demoDraw, demoSuspend, demoTerm, SCREEN_TERMINATED };
