// animated sprite - sprite sheet with frame animation
#include "animated-sprite.h"
#include "gfx.h"
#include "colors.h"

#define SPRITE_PATH "/dev_hdd0/game/APPSMP001/USRDIR/pattern.png"
#define SPRITE_FRAME_SIZE 32
#define SPRITE_FRAME_COUNT 23
#define SPRITE_ANIM_SPEED 6

static GfxTexture spriteTex;
static int spriteFrame = 0;
static int spriteTick = 0;

void initAnimatedSprite(void)
{
    spriteTex = gfxLoadTexture(SPRITE_PATH);
}

void updateAnimatedSprite(void)
{
    spriteTick++;
    if (spriteTick >= SPRITE_ANIM_SPEED) {
        spriteTick = 0;
        spriteFrame++;
        if (spriteFrame >= SPRITE_FRAME_COUNT) {
            spriteFrame = 0;
        }
    }
}

void drawAnimatedSprite(int x, int y)
{
    if (spriteTex.w > 0) {
        float frameH = (float)SPRITE_FRAME_SIZE / (float)spriteTex.h;
        float v0 = frameH * spriteFrame;
        float v1 = v0 + frameH;
        gfxDrawTexture(x, y, SPRITE_FRAME_SIZE * 4, SPRITE_FRAME_SIZE * 4, spriteTex, 0.0f, v0, 1.0f, v1, COLOR_WHITE, GFX_FILTER_NEAREST);
    } else {
        gfxFillRectangle(x, y, SPRITE_FRAME_SIZE * 4, SPRITE_FRAME_SIZE * 4, COLOR_RED);
    }
}
