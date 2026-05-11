// sidepanel - slides in from the right
#include "overlays/sidepanel.h"
#include "gfx.h"
#include "colors.h"
#include "anim.h"

#define SIDEPANEL_WIDTH 400

static float x;
static Anims anims;

static void show(void)
{
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
    int sh = getGfxScreenHeight();
    fillGfxRectangle(px, 0, sw - px, sh, COLOR_SLATE_800);
}

static void term(void)
{
    cancelAllAnims(&anims);
    sidepanel.status = OVERLAY_TERMINATED;
}

Overlay sidepanel = { show, hide, update, draw, term, OVERLAY_TERMINATED };
