// sidepanel - slides in from the right
#include "overlays/sidepanel.h"
#include "gfx.h"
#include "colors.h"
#include "font.h"
#include "anim.h"

#define SIDEPANEL_WIDTH 400

static float x;
static Anims anims;
static Font font;
static GfxTexture titleText;
static GfxTexture bodyText;

static void init(void)
{
    font = fontOpenSystem(FONT_POP);
    titleText = fontToTexture(SIDEPANEL_WIDTH - 40, AUTO, "Side Panel", &font, 20, COLOR_WHITE, TEXT_NOWRAP);
    bodyText = fontToTexture(SIDEPANEL_WIDTH - 40, AUTO, "This panel slides in from the right. Press SELECT to close.", &font, 14, COLOR_SLATE_300, TEXT_WRAP);
}

static void show(void)
{
    if (sidepanel.status == OVERLAY_TERMINATED) init();
    int sw = gfxScreenWidth();
    x = (float)sw;
    animSet(&anims, &x, (float)sw, (float)(sw - SIDEPANEL_WIDTH), 300, EASE_OUT_CUBIC, ANIM_ONCE, NULL);
    sidepanel.status = OVERLAY_VISIBLE;
}

static void onHidden(AnimHandle self)
{
    (void)self;
    sidepanel.status = OVERLAY_HIDDEN;
}

static void hide(void)
{
    int sw = gfxScreenWidth();
    animSet(&anims, &x, x, (float)sw, 300, EASE_IN_CUBIC, ANIM_ONCE, onHidden);
}

static void update(void)
{
    animUpdate(&anims);
}

static void draw(void)
{
    int px = (int)x;
    int sw = gfxScreenWidth();
    int sh = gfxScreenHeight();

    gfxFillRectangle(px, 0, sw - px, sh, COLOR_SLATE_800);

    gfxDrawTexture(px + 20, 30, titleText.w, titleText.h, titleText, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
    gfxDrawTexture(px + 20, 70, bodyText.w, bodyText.h, bodyText, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
}

static void term(void)
{
    animCancelAll(&anims);
    fontClose(&font);
    sidepanel.status = OVERLAY_TERMINATED;
}

Overlay sidepanel = { show, hide, update, draw, term, OVERLAY_TERMINATED };
