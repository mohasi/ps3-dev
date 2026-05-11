// sidepanel - slides in from the right
#include "overlays/sidepanel.h"
#include "gfx.h"
#include "colors.h"
#include "font.h"
#include "ui/label.h"
#include "ui/rectangle.h"
#include "anim.h"

#define SIDEPANEL_WIDTH 400

static float x;
static Anims anims;
static Font font;
static Rectangle bg;
static Label titleText;
static Label bodyText;

static void init(void)
{
    font = openSystemFont(FONT_POP);
    initRectangle(&bg, 0, 0, 0, getGfxScreenHeight(), COLOR_SLATE_800);
    initLabel(&titleText, &font, 0, 30, SIDEPANEL_WIDTH - 40, AUTO, 20, COLOR_WHITE, TEXT_NOWRAP, "Side Panel");
    initLabel(&bodyText, &font, 0, 70, SIDEPANEL_WIDTH - 40, AUTO, 14, COLOR_SLATE_300, TEXT_WRAP, "This panel slides in from the right. Press SELECT to close.");
}

static void show(void)
{
    if (sidepanel.status == OVERLAY_TERMINATED) init();
    int sw = getGfxScreenWidth();
    x = (float)sw;
    setAnim(&anims, &x, (float)sw, (float)(sw - SIDEPANEL_WIDTH), 300, EASE_OUT_CUBIC, ANIM_ONCE, NULL);
    sidepanel.status = OVERLAY_VISIBLE;
}

static void onHidden(AnimHandle self)
{
    (void)self;
    sidepanel.status = OVERLAY_HIDDEN;
}

static void hide(void)
{
    int sw = getGfxScreenWidth();
    setAnim(&anims, &x, x, (float)sw, 300, EASE_IN_CUBIC, ANIM_ONCE, onHidden);
}

static void update(void)
{
    updateAnim(&anims);
}

static void draw(void)
{
    int px = (int)x;
    int sw = getGfxScreenWidth();

    bg.x = px;
    bg.w = sw - px;
    drawRectangle(&bg);

    moveLabel(&titleText, px + 20, titleText.y);
    moveLabel(&bodyText, px + 20, bodyText.y);
    drawLabel(&titleText);
    drawLabel(&bodyText);
}

static void term(void)
{
    cancelAllAnims(&anims);
    closeFont(&font);
    sidepanel.status = OVERLAY_TERMINATED;
}

Overlay sidepanel = { show, hide, update, draw, term, OVERLAY_TERMINATED };
