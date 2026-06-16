#include "typewriter.h"

#include <sys/sys_time.h>

#include "gfx.h"
#include "colors.h"

void startTypewriter(Typewriter *tw, int cps, int instant)
{
    tw->cps     = cps;
    tw->total   = tw->reveal.count;
    tw->startUs = sys_time_get_system_time();
    tw->done    = instant || cps <= 0 || tw->total <= 0;
    tw->shown   = tw->done ? tw->total : 0;
}

void tickTypewriter(Typewriter *tw)
{
    if (tw->done) return;
    uint64_t elapsed = sys_time_get_system_time() - tw->startUs;
    int revealed = (int)((double)elapsed / 1000000.0 * (double)tw->cps);
    if (revealed >= tw->total) { revealed = tw->total; tw->done = 1; }
    tw->shown = revealed;
}

void completeTypewriter(Typewriter *tw) { tw->done = 1; tw->shown = tw->total; }

int isTypewriterDone(const Typewriter *tw) { return tw->done; }

void drawTypewriter(const Typewriter *tw, TextTexture *tex, int x, int y)
{
    if (!tex->valid || tex->tex.w <= 0 || tex->tex.h <= 0) return;
    const TextReveal *reveal = &tw->reveal;
    int shown = tw->done ? tw->total : tw->shown;

    if (shown >= reveal->count)   // whole line
    {
        drawGfxTexture(x, y, tex->tex.w, tex->tex.h, tex->tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
        return;
    }
    if (shown <= 0) return;

    int texW = tex->tex.w, texH = tex->tex.h;
    int last = shown - 1;
    int lineTop = reveal->line[last] * reveal->lineHeight; if (lineTop > texH) lineTop = texH;
    int revealX = reveal->endX[last];    if (revealX > texW) revealX = texW;

    // complete lines above the current one: full width
    if (lineTop > 0)
        drawGfxTexture(x, y, texW, lineTop, tex->tex,
                       0.0f, 0.0f, 1.0f, (float)lineTop / (float)texH, COLOR_WHITE, GFX_FILTER_LINEAR);

    // the current line, up to the revealed x
    int lineH = reveal->lineHeight; if (lineTop + lineH > texH) lineH = texH - lineTop;
    if (revealX > 0 && lineH > 0)
        drawGfxTexture(x, y + lineTop, revealX, lineH, tex->tex,
                       0.0f, (float)lineTop / (float)texH,
                       (float)revealX / (float)texW, (float)(lineTop + lineH) / (float)texH,
                       COLOR_WHITE, GFX_FILTER_LINEAR);
}
