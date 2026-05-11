// sidepanel - slides in from the right
#include "overlays/sidepanel.h"
#include "gfx.h"
#include "colors.h"
#include "font.h"
#include "anim.h"
#include <string.h>

#define SIDEPANEL_WIDTH 400

static float x;
static Anims anims;
static Font font;
static TextTexture titleText;
static TextTexture bodyText;

static void init(void)
{
    font = fontOpenSystem(FONT_POP);
    memset(&titleText, 0, sizeof(titleText));
    memset(&bodyText, 0, sizeof(bodyText));
    fontRender(&titleText, &font, 20, "Side Panel", COLOR_WHITE, SIDEPANEL_WIDTH - 40, TEXT_NOWRAP);
    fontRender(&bodyText, &font, 14, "This panel slides in from the right. Press SELECT to close.", COLOR_SLATE_300, SIDEPANEL_WIDTH - 40, TEXT_WRAP);
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

    gfxDrawTexture(px + 20, 30, titleText.tex.w, titleText.tex.h, titleText.tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
    gfxDrawTexture(px + 20, 70, bodyText.tex.w, bodyText.tex.h, bodyText.tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_NEAREST);
}

static void term(void)
{
    animCancelAll(&anims);
    fontClose(&font);
    sidepanel.status = OVERLAY_TERMINATED;
}

Overlay sidepanel = { show, hide, update, draw, term, OVERLAY_TERMINATED };
