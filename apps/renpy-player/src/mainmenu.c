#include "mainmenu.h"

#include <string.h>

#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "gui.h"
#include "assets.h"
#include "config.h"        // RR12G_PATH / RR6G_PATH
#include "ui/slice.h"
#include "focus.h"         // geometric focus navigation (the engine's, shared by all menus)
#include "dbg.h"

typedef struct { GfxTexture idle, hover; int hasHover; MmAction action; } MmButton;

static GfxTexture mmBgTex;    static int mmBgLoaded;
static GfxTexture mmFrameTex; static int mmFrameLoaded, mmFrameCap;
static MmButton   mmBtn[GUI_MMBTN_MAX]; static int mmBtnN;

void freeMainMenu(void)
{
    if (mmBgLoaded) { freeGfxTexture(&mmBgTex); mmBgLoaded = 0; }
    if (mmFrameLoaded) { freeGfxTexture(&mmFrameTex); mmFrameLoaded = 0; }
    for (int i = 0; i < mmBtnN; i++)
    {
        freeGfxTexture(&mmBtn[i].idle);
        if (mmBtn[i].hasHover) freeGfxTexture(&mmBtn[i].hover);
    }
    mmBtnN = 0;
}

// Builds the menu from the game's actual config.main_menu (manifest mm_order) -- not a hardcoded
// list -- mapping each label to its engine-constant action and loading the button art. Labels with
// no image art (e.g. Help) are skipped (this player's main menu is image-button only). Falls back to
// the engine-default order when the manifest doesn't carry mm_order.
void buildMainMenu(void)
{
    static const struct { const char *label; MmAction action; } act[] = {   // config.main_menu actions (00menus.rpy)
        { "Start Game",  MM_ACTION_START },
        { "Load Game",   MM_ACTION_LOAD  },   // _intra_jumps("load_screen") -> the Load file picker
        { "Preferences", MM_ACTION_NONE  },   // _intra_jumps("preferences_screen") -> GM5 (not built yet)
        { "Quit",        MM_ACTION_QUIT  },
    };
    static const char *fallback[] = { "Start Game", "Load Game", "Preferences", "Quit" };
    freeMainMenu();
    int useOrder = gui.mmOrderCount > 0;
    int count = useOrder ? gui.mmOrderCount : (int)(sizeof fallback / sizeof fallback[0]);
    for (int idx = 0; idx < count && mmBtnN < GUI_MMBTN_MAX; idx++)
    {
        const char *label = useOrder ? gui.mmOrder[idx] : fallback[idx];
        int manifestIdx = -1;
        for (int i = 0; i < gui.mmBtnCount; i++)
            if (strcmp(gui.mmBtnLabel[i], label) == 0) { manifestIdx = i; break; }
        if (manifestIdx < 0) continue;                         // no art for this label -> skip (image menu)
        MmButton *button = &mmBtn[mmBtnN];
        memset(button, 0, sizeof *button);
        if (!loadAssetTexture(gui.mmBtnIdle[manifestIdx], &button->idle)) continue;   // art missing -> skip
        if (gui.mmBtnHover[manifestIdx][0]) button->hasHover = loadAssetTexture(gui.mmBtnHover[manifestIdx], &button->hover);
        button->action = MM_ACTION_NONE;                       // custom labels (Gallery) -> inert
        for (int a = 0; a < (int)(sizeof act / sizeof act[0]); a++)
            if (strcmp(act[a].label, label) == 0) { button->action = act[a].action; break; }
        mmBtnN++;
    }
    mmBgLoaded = gui.mmBg[0] ? loadAssetTexture(gui.mmBg, &mmBgTex) : 0;

    // RoundRect template for the menu frame: rr12g (native > 640) else rr6g -- the engine PNG
    // shipped in the player's res\, tinted by the frame colour at draw time (= OneOrTwoColor).
    // Skipped when the game re-parented mm_menu_frame to style.default (no frame box / padding).
    mmFrameLoaded = 0;
    if (!gui.mmFrameNone)
    {
        if (gui.nativeW > 0 && gui.nativeW <= 640) { mmFrameTex = loadGfxTexture(RR6G_PATH);  mmFrameCap = 6;  }
        else                                       { mmFrameTex = loadGfxTexture(RR12G_PATH); mmFrameCap = 12; }
        mmFrameLoaded = (mmFrameTex.w > 0 && mmFrameTex.h > 0);
    }
    clearFocus();
    logInfo("[rpp] main menu: %d buttons, bg=%d frame=%d\n", mmBtnN, mmBgLoaded, mmFrameLoaded);
}

void drawMainMenu(int cx, int cy, int cw, int ch)
{
    clearGfx(0xFF000000);
    if (mmBgLoaded)
        drawGfxTexture(cx, cy, cw, ch, mmBgTex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);

    if (mmBtnN == 0) return;

    // Everything below is TRANSLATED from the classic theme source, not eyeballed:
    //   _layout/classic_main_menu.rpym : mm_menu_frame at xpos 5/6 (xanchor 0.5), ypos 0.9
    //       (yanchor 1.0); the buttons are a style.vbox (spacing 0), equal width (size_group).
    //   mm_menu_frame -> menu_frame -> style.frame, whose roundrect_frames() sets background to
    //       RoundRect(frame) = Frame(rrNg.png, N, N) (N = 6 if native<=640 else 12) tinted by
    //       `frame` (default (100,150,200,255), or the manifest), with xpadding = ypadding = 6.
    //       Drawn as a scaled 9-slice of the engine template, tinted -- exactly RoundRect/
    //       OneOrTwoColor, with the template's real shading + AA corners.
    // Positions are fractions of the native screen, applied to the content rect.
    float assetScale = getGuiAssetScale(cw);
    float scale = getGuiScale(cw);
    int pad = gui.mmFrameNone ? 0 : (int)(6 * scale + 0.5f);

    int btnW[GUI_MMBTN_MAX], btnH[GUI_MMBTN_MAX], maxBtnW = 0, totalBtnH = 0;
    for (int i = 0; i < mmBtnN; i++)
    {
        btnW[i] = (int)(mmBtn[i].idle.w * assetScale + 0.5f);
        btnH[i] = (int)(mmBtn[i].idle.h * assetScale + 0.5f);
        if (btnW[i] > maxBtnW) maxBtnW = btnW[i];
        totalBtnH += btnH[i];
    }
    int frameW = maxBtnW + 2 * pad, frameH = totalBtnH + 2 * pad;
    int colCenterX  = cx + (int)(cw * (5.0f / 6.0f) + 0.5f);   // xpos 5/6, xanchor 0.5
    int frameBottom = cy + (int)(ch * 0.9f + 0.5f);            // ypos 0.9, yanchor 1.0
    int frameX = colCenterX - frameW / 2;
    int frameY = frameBottom - frameH;

    if (mmFrameLoaded)
    {
        SpriteRegion whole = { 0, 0, mmFrameTex.w, mmFrameTex.h };
        NineSlice frameSlice;
        initNineSliceScaled(&frameSlice, mmFrameTex, frameX, frameY, frameW, frameH, whole, mmFrameCap, mmFrameCap, scale);
        frameSlice.tint = gui.mmFrameColor;   // recolour the grayscale-gradient template
        drawNineSlice(&frameSlice);
    }

    beginFocusFrame();
    int drawY = frameY + pad;
    for (int i = 0; i < mmBtnN; i++)
    {
        int btnX = colCenterX - btnW[i] / 2;                // xanchor 0.5 (centred in column)
        addFocus(i, btnX, drawY, btnW[i], btnH[i]);
        int focused = isFocused(i);
        GfxTexture tex = (focused && mmBtn[i].hasHover) ? mmBtn[i].hover : mmBtn[i].idle;
        if (focused && !mmBtn[i].hasHover)                  // selection cue when there's no hover art
            fillGfxRectangle(btnX - 4, drawY - 2, btnW[i] + 8, btnH[i] + 4, 0x55FFFFFF);
        drawGfxTexture(btnX, drawY, btnW[i], btnH[i], tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
        drawY += btnH[i];
    }
}

// Drive the menu from BOTH the d-pad (geometric focus nav) and the virtual cursor (mouse focus). When
// the cursor is visible we focus the button under it (focusAt = Ren'Py point-in-rect mouse focus), so
// X activates whatever it points at; a click over empty space does nothing, like a real mouse. The
// button rects come from drawMainMenu's addFocus calls (one frame old -- invisible lag). curX/curY are
// already in screen pixels (the content-rect offset is baked into the rects we registered).
MmAction updateMainMenu(int curVisible, int curX, int curY)
{
    if (isPadButtonPressed(PAD_BTN_CIRCLE)) return MM_ACTION_QUIT;   // O backs out to the selector
    if (mmBtnN == 0) return MM_ACTION_NONE;

    int hoverId = curVisible ? focusAt(curX, curY) : -1;
    if (hoverId >= 0) setFocus(hoverId);                      // cursor hover highlights the button under it

    if (isPadButtonPressed(PAD_BTN_UP))    moveFocus(0, -1);   // the engine's geometric focus nav
    if (isPadButtonPressed(PAD_BTN_DOWN))  moveFocus(0,  1);
    if (isPadButtonPressed(PAD_BTN_LEFT))  moveFocus(-1, 0);
    if (isPadButtonPressed(PAD_BTN_RIGHT)) moveFocus(1,  0);
    if (isPadButtonPressed(PAD_BTN_CROSS))
    {
        if (curVisible && hoverId < 0) return MM_ACTION_NONE;  // click in empty space = nothing (mouse)
        int id = getFocusId();
        if (id >= 0 && id < mmBtnN) return mmBtn[id].action;   // NONE for inert buttons
    }
    return MM_ACTION_NONE;
}
