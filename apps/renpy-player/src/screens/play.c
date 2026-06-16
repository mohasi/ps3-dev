#include "screens/play.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>           // sqrtf (cursor acceleration magnitude)
#include <sys/sys_time.h>   // sys_time_get_system_time (cursor idle-hide timing)

#include "gfx.h"
#include "colors.h"
#include "font.h"
#include "pad.h"
#include "file.h"
#include "printf.h"
#include "dbg.h"
#include "screen-manager.h"
#include "config.h"
#include "rpk.h"
#include "rbc.h"
#include "vars.h"
#include "gui.h"
#include "sprite-regions.h"      // packed sprite sheet regions (hand cursor); auto-generated
#include "assets.h"
#include "sound.h"
#include "history.h"              // rollback timeline (Frame store + cursor)
#include "saytext.h"              // dialogue / NVL / menu text layer (build + draw)
#include "scene-draw.h"          // letterbox + background + sprites + transitions + pause
#include "mainmenu.h"             // classic-theme main menu (M_MAINMENU)
#include "gamemenu.h"             // classic-theme in-game menu (M_GAMEMENU)
#include "focus.h"                // geometric focus nav (imagemap hotspots)
#include "vm.h"                   // bytecode interpreter; play.c implements its host hooks
#include "gamepath.h"             // getGameRpkPath() (game + saves location)
#include "savestate.h"            // generic VM-state save/load (game-menu Save/Load)

#define HINT_GRAY 0xFFA0A0A0   // engine UI only (navigation hints), not a game/Ren'Py value

// Screen mode. M_RUN is the initial/executing sentinel (the VM runs synchronously, then a host
// hook sets one of the interactive modes before runVm returns).
typedef enum { M_RUN, M_SAY, M_MENU, M_DONE, M_ERROR, M_TRANS, M_PAUSE, M_MAINMENU, M_GAMEMENU, M_IMAGEMAP } Mode;

static RbcProgram prog;
static int        loaded;
static Mode       mode;

// Boot phases for classic-theme games: run the game's `splashscreen` label, show its main
// menu, then `start`. Games with no menu in the manifest skip straight to `start` (as before).
typedef enum { BOOT_GAME, BOOT_SPLASH, BOOT_MENU } BootPhase;
static BootPhase bootPhase;

// The main menu itself lives in mainmenu.c (M_MAINMENU just drives it). enterMainMenu builds it
// and enters the mode; it's defined lower down but the endProgram hook above it needs it.
static void enterMainMenu(void);

// 1 while the in-game menu was opened FROM THE TITLE (main-menu "Load Game"): GM_RETURN / Circle
// then goes back to the main menu rather than resuming a (non-existent) in-game line.
static int gmFromTitle;

// Pixel-exact save screenshot. On opening the menu we flag a capture; the next drawPlay (inside a
// gfx frame) re-renders the current scene+dialogue into a main-memory target, crops the game area
// and downscales to THUMB_W x THUMB_H (config.thumbnail_width/height). GM_DO_SAVE writes it as the
// slot's slot-N.png. Capture happens in DRAW so it runs inside beginGfxFrame/endGfxFrame.
#define THUMB_W 320
#define THUMB_H 240
static unsigned char pendingThumb[THUMB_W * THUMB_H * 4];
static int           pendingThumbValid;   // pendingThumb holds a fresh capture (for the next save)
static int           capturePending;      // a capture was requested (do it on the next draw)

static const char *curWho;     // NULL = narration
static const char *curWhat;
static const char *curScene;
static const char *curShow;    // last shown image name (for the debug overlay)

static int   menuCount;
static const char *menuCap[MAX_CHOICES];
static int   menuTarget[MAX_CHOICES];
static int   menuSel;
static int   menuHasCaption;   // the menu had a caption (narrator line shown in the dialogue box under the choices)

static char  errText[256];

static Font        font;        // opened here, shared with the text layer via initSay
static int         fontReady;
static Font        igFont;      // in-game chat font (gui.igFont), shared via initSayIngame
static int         igFontReady;
static TextTexture infoTex;     // debug overlay (scene/show names)

// Current line's NVL state, for history + `nvl clear`. The page CONTENT + textures live in
// saytext; play.c only tracks whether the current line is NVL and which page it belongs to.
static int curNvl;
static int curNvlPage;   // id of the NVL page being accumulated (for the rollback page rebuild)
static int curIngame;    // is the current line an in-game chat-box line? (Say kind=2)

// ---- rendering ----

static void renderInfo(void) { }   // dev scene/show HUD removed (not a Ren'Py concept)

// Overlay-set rollback: snapshot/restore which HUD overlays were active at a line (defined below, after
// the activeOverlays store). The var store rolls back already; the active overlay SET is separate state
// (showOverlay/hideOverlay = config.overlay_functions add/remove), so it must be snapshotted too.
static void snapshotOverlaysToFrame(Frame *f);
static void restoreOverlaysFromFrame(const Frame *f);

// Restore and show the frame the history cursor is on (used when stepping back/forward).
static void showFrame(void)
{
    const Frame *frame = getHistoryCurrent();
    if (!frame) return;
    curWho   = frame->who;
    curWhat  = frame->what;
    curScene = frame->scene;
    curNvl   = frame->nvl;
    curIngame = frame->ingame;
    restoreVarsSnap(frame->varSnap);   // roll the variable store back to this line (overlays/HUD/[var] follow)
    restoreOverlaysFromFrame(frame);   // and the active HUD overlay SET (not covered by the var snapshot)
    resolveScene(&prog, curScene);   // restore this line's background
    clearSprites();                // rebuild the sprites that were on screen for this line
    for (int s = 0; s < frame->showCount; s++) showSprite(&prog, frame->shows[s], frame->showAt[s], frame->showAtl[s]);
    curShow = frame->showCount > 0 ? frame->shows[frame->showCount - 1] : NULL;
    restoreSoundMusic(frame->musicCmd);   // music follows the line we stepped to
    mode = M_SAY;
    restartGuiCtc();               // the line (re)appears: restart the Blink clock
    renderInfo();

    int cx, cy, cw, ch; getSceneContentRect(&cx, &cy, &cw, &ch);
    if (frame->isPause) { showSayBlank(); return; }   // paused scene held -> no textbox, just the visuals
    if (!curNvl) { showSayLine(cx, cy, cw, ch, curWho, curWhat, 0, curIngame, 0, 1); return; }

    // Restore the whole stacked NVL page (not just this line); typing = 1 = the current line types.
    const char *who[NVL_MAX], *what[NVL_MAX];
    int count = getHistoryNvlPage(who, what, NVL_MAX);
    showSayNvlPage(cw, who, what, count, 1);
}

// ---- VM host hooks (the screen's reaction to each executed instruction; see vm.h) ----

// Record a rollback frame: the current scene/sprites/music + (for dialogue) the speaker line. Used by
// Say lines and by renpy.pause() checkpoints (isPause=1, who/what NULL) so a pause's visuals (e.g. the
// "Disconnected" message shown then paused) can be rolled back to faithfully.
static void captureFrame(const char *who, const char *what, int nvl, int ingame, int isPause)
{
    Frame *frame = appendHistory();
    if (!frame) return;
    frame->who = who; frame->what = what; frame->scene = curScene;
    frame->nvl = nvl; frame->ingame = ingame; frame->isPause = isPause; frame->nvlPage = curNvlPage;
    frame->musicCmd  = getSoundMusicCmd();
    frame->showCount = getSpriteCount();
    for (int i = 0; i < getSpriteCount(); i++) { frame->shows[i] = getSpriteAt(i)->src; frame->showAt[i] = getSpriteAt(i)->atSrc; frame->showAtl[i] = getSpriteAt(i)->atlId; }
    snapshotOverlaysToFrame(frame);   // record which HUD overlays were active, for rollback
}

// A pause checkpoint with the current scene held (no dialogue). Lets rollback land on it.
void recordPauseFrame(void) { captureFrame(NULL, NULL, 0, 0, 1); }

// Route a jump to a Ren'Py engine-generated screen label (created by layout.imagemap_load_save /
// _preferences, which we don't execute) to our own menu. Returns 1 if handled (the VM yields).
int jumpToEngineScreen(const char *name)
{
    if (!name) return 0;
    if (strcmp(name, "load_screen") == 0 || strcmp(name, "save_screen") == 0 || strcmp(name, "_load_screen") == 0)
    {
        gmFromTitle = 1;
        enterGameMenuFromTitle();   // our file picker (Load from the title)
        mode = M_GAMEMENU;
        return 1;
    }
    return 0;   // unknown engine screen -> inert (preferences/extra: follow-up)
}

// Show a dialogue / NVL line: update the current speaker, record a history frame for rollback,
// then hand the line to the text layer and enter the M_SAY interaction.
void beginSay(const char *who, const char *what, int nvl, int ingame)
{
    int freshPage = nvl && !curNvl;   // first NVL line after ADV -> a new page
    curWho = who; curWhat = what; curNvl = nvl; curIngame = ingame;
    if (freshPage) curNvlPage++;
    captureFrame(who, what, nvl, ingame, 0);   // record this line for rollback
    mode = M_SAY;
    restartGuiCtc();   // new line: the ctc's anim.Blink starts now
    renderInfo();
    int cx, cy, cw, ch; getSceneContentRect(&cx, &cy, &cw, &ch);
    showSayLine(cx, cy, cw, ch, who, what, nvl, ingame, freshPage, 1);
}

void applyScene(const char *name)
{
    curScene = name;
    curShow  = NULL;
    clearSprites();
    resolveScene(&prog, curScene);
    curNvl = 0;
    clearSayNvl();
}

void showImage(const char *name, const char *at, int atlId) { curShow = name; showSprite(&prog, name, at, atlId); }
void hideImage(const char *name)                 { hideSprite(name); curShow = NULL; }

// Start a `with` dissolve/fade (scene-draw plays it); enter the wait state. 1 = a transition is
// playing, 0 = it was instant (keep executing).
int beginTransition(const char *name)
{
    if (!beginSceneTransition(name)) return 0;
    mode = M_TRANS;
    return 1;
}

void beginMenu(const char *caption, const char *const *captions, const int *targets, int count)
{
    menuCount = count;
    for (int i = 0; i < count; i++) { menuCap[i] = captions[i]; menuTarget[i] = targets[i]; }
    menuSel = 0;
    mode = M_MENU;
    renderInfo();
    int cx, cy, cw, ch; getSceneContentRect(&cx, &cy, &cw, &ch);
    // A menu CAPTION (a menu item with no block) is NOT a choice: Ren'Py shows it via the narrator in the
    // dialogue box while the choices display. Render it as a narrator say line; M_MENU draws that box under
    // the choices. (Shown fully, no typewriter, so the choices are readable immediately.)
    menuHasCaption = (caption != NULL && caption[0] != '\0');
    if (menuHasCaption)
    {
        curWho = NULL; curWhat = caption; curNvl = 0; curIngame = 0;
        showSayLine(cx, cy, cw, ch, NULL, caption, 0, 0, 0, 0);
    }
    showSayMenu(cw, captions, count, menuSel);
}

// ---- imagemap menu (M_IMAGEMAP): the generic imagemap mechanism (_layout/imagemap_common.rpym) ----
// One renderer for BOTH faithful forms: the simple renpy.imagemap (kind=="", ground+hover, returns a
// value) and the themed layout imagemap screens (kind!="", ground + 4 state images, named hotspots).
// Drawing is uniform: draw ground full-screen, then for each hotspot draw its current state image
// (idle / hover / selected_idle / selected_hover) CROPPED to the hotspot rect, exactly as _ImageMapper
// does with LiveCrop. The interactive logic per screen (file-picker slots/paging, preferences toggles,
// nav actions) is layered on top by dispatching the hotspot NAME.
static const RbcImageMap *imMap;
static const char        *imResultVar;   // simple form: var the chosen hotspot's value is written to

// State images. IM_GROUND/IDLE/HOVER/SEL_IDLE/SEL_HOVER mirror _ImageMapper's fields.
enum { IM_GROUND = 0, IM_IDLE, IM_HOVER, IM_SEL_IDLE, IM_SEL_HOVER, IM_NTEX };
static GfxTexture imTex[IM_NTEX];
static int        imTexOk[IM_NTEX];

static const char *imBaseName(const char *path)
{
    if (!path) return "";
    const char *s = strrchr(path, '/');
    if (s) return s + 1;
    s = strrchr(path, '\\');
    return s ? s + 1 : path;
}

static void freeImageMap(void)
{
    for (int i = 0; i < IM_NTEX; i++)
        if (imTexOk[i]) { freeGfxTexture(&imTex[i]); imTexOk[i] = 0; }
    imMap = NULL;
}

// Load a state image; an empty path means "fall back" (engine: idle->ground, hover->idle,
// selected_*->idle/hover) so the simple form (only ground+hover baked) and partial themed forms work.
static void imLoad(int slot, const char *path)
{
    if (path && path[0])
        imTexOk[slot] = loadAssetTexture(imBaseName(path), &imTex[slot]);
    else
        imTexOk[slot] = 0;
}

void beginImageMap(const RbcImageMap *im, const char *resultVar)
{
    freeImageMap();
    imMap = im;
    imResultVar = resultVar ? resultVar : "";
    if (im)
    {
        imLoad(IM_GROUND,   im->ground);
        imLoad(IM_IDLE,     im->idle);
        imLoad(IM_HOVER,    im->hover);
        imLoad(IM_SEL_IDLE, im->selectedIdle);
        imLoad(IM_SEL_HOVER,im->selectedHover);
    }
    clearFocus();
    mode = M_IMAGEMAP;
    logInfo("[rpp] imagemap[%s]: %s (%d hotspots) -> %s\n",
            im && im->kind ? im->kind : "", im ? im->ground : "?",
            im ? im->hotspotCount : 0, imResultVar);
}

// Resolve a state slot through the engine's fallback chain to an actually-loaded texture (or -1).
static int imResolveTex(int slot)
{
    for (;;)
    {
        if (slot < 0) return -1;
        if (imTexOk[slot]) return slot;
        switch (slot)
        {
            case IM_IDLE:     slot = IM_GROUND; break;   // idle -> ground
            case IM_HOVER:    slot = IM_IDLE;   break;   // hover -> idle
            case IM_SEL_IDLE: slot = IM_IDLE;   break;   // selected_idle -> idle
            case IM_SEL_HOVER:slot = IM_HOVER;  break;   // selected_hover -> hover
            default:          return -1;                 // ground has no fallback
        }
    }
}

// The focused hotspot's value -> result var, then resume the VM (the label's if/elif reads it).
// (Simple renpy.imagemap form; themed screens dispatch by name instead -- added with their logic layer.)
static void imageMapSelect(void)
{
    int id = getFocusId();
    if (id >= 0 && imMap && id < imMap->hotspotCount)
    {
        Value v; v.t = VT_STR; v.i = 0; v.f = 0; v.s = imMap->hotspots[id].name;
        setVar(imResultVar, &v, 0);   // overwrite
    }
    freeImageMap();
    runVm();
}

// Screen rect of a hotspot (native corner coords -> the letterboxed content rect).
static void imageMapHotspotRect(const RbcHotspot *h, int cx, int cy, int cw, int ch, int *x, int *y, int *w, int *hgt)
{
    float nw = gui.nativeW > 0 ? (float)gui.nativeW : 800.0f;
    float nh = gui.nativeH > 0 ? (float)gui.nativeH : 600.0f;
    *x   = cx + (int)((float)h->x0 / nw * cw + 0.5f);
    *y   = cy + (int)((float)h->y0 / nh * ch + 0.5f);
    *w   = (int)((float)(h->x1 - h->x0) / nw * cw + 0.5f);
    *hgt = (int)((float)(h->y1 - h->y0) / nh * ch + 0.5f);
}

// Draw one hotspot's state image cropped to its rect (LiveCrop((x1,y1,w,h), image) drawn at xpos/ypos).
static void imageMapDrawHotspot(const RbcHotspot *h, int slot, int cx, int cy, int cw, int ch)
{
    int t = imResolveTex(slot);
    if (t < 0) return;
    float nw = gui.nativeW > 0 ? (float)gui.nativeW : 800.0f;
    float nh = gui.nativeH > 0 ? (float)gui.nativeH : 600.0f;
    int x, y, w, hgt; imageMapHotspotRect(h, cx, cy, cw, ch, &x, &y, &w, &hgt);
    drawGfxTexture(x, y, w, hgt, imTex[t],
                   (float)h->x0 / nw, (float)h->y0 / nh, (float)h->x1 / nw, (float)h->y1 / nh,
                   COLOR_WHITE, GFX_FILTER_LINEAR);
}

static void drawImageMap(int cx, int cy, int cw, int ch)
{
    clearGfx(0xFF000000);
    if (imTexOk[IM_GROUND])
        drawGfxTexture(cx, cy, cw, ch, imTex[IM_GROUND], 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
    if (!imMap) return;

    beginFocusFrame();
    for (int i = 0; i < imMap->hotspotCount; i++)
    {
        const RbcHotspot *h = &imMap->hotspots[i];
        int x, y, w, hgt; imageMapHotspotRect(h, cx, cy, cw, ch, &x, &y, &w, &hgt);
        addFocus(i, x, y, w, hgt);
        // State per _ImageMapper.button: selected uses selected_*; focus picks hover vs idle.
        // (Selection state for themed screens -- e.g. the newest save slot, current prefs value --
        // is supplied by the per-screen logic layer; for now nothing is "selected".)
        int focused = isFocused(i);
        int slot = focused ? IM_HOVER : IM_IDLE;
        imageMapDrawHotspot(h, slot, cx, cy, cw, ch);
    }
}

// ---- HUD overlays (config.overlay_functions): guarded widgets drawn over the in-game scene ----
#define OV_ACTIVE_MAX 8
static const RbcOverlay *activeOverlays[OV_ACTIVE_MAX];
static int activeOverlayCount;

// Lazy texture cache for overlay widget images (loading per frame would be ruinous).
#define OVTEX_MAX 64
static struct { char file[64]; GfxTexture tex; int ok; } ovTexCache[OVTEX_MAX];
static int ovTexCacheN;

void showOverlay(const char *name)
{
    const RbcOverlay *ov = getRbcOverlayByName(&prog, name);
    if (!ov) return;
    for (int i = 0; i < activeOverlayCount; i++) if (activeOverlays[i] == ov) return;   // already active
    if (activeOverlayCount < OV_ACTIVE_MAX) activeOverlays[activeOverlayCount++] = ov;
}
void hideOverlay(const char *name)
{
    const RbcOverlay *ov = getRbcOverlayByName(&prog, name);
    if (!ov) return;
    for (int i = 0; i < activeOverlayCount; i++)
        if (activeOverlays[i] == ov) { activeOverlays[i] = activeOverlays[--activeOverlayCount]; return; }
}

// Snapshot / restore the active HUD overlay SET for rollback (forward-declared up by showFrame). The
// set is which config.overlay_functions are active (showOverlay/hideOverlay), separate from the var
// store -- so stepping back to a line restores the overlays that were up then, not the latest set.
static void snapshotOverlaysToFrame(Frame *f)
{
    f->overlayCount = activeOverlayCount;
    for (int i = 0; i < activeOverlayCount && i < HISTORY_OVERLAY_MAX; i++) f->overlays[i] = activeOverlays[i];
}
static void restoreOverlaysFromFrame(const Frame *f)
{
    activeOverlayCount = f->overlayCount < OV_ACTIVE_MAX ? f->overlayCount : OV_ACTIVE_MAX;
    for (int i = 0; i < activeOverlayCount; i++) activeOverlays[i] = (const RbcOverlay *)f->overlays[i];
}

static int ovGetTex(const char *file, GfxTexture *out)
{
    const char *base = imBaseName(file);
    for (int i = 0; i < ovTexCacheN; i++)
        if (strcmp(ovTexCache[i].file, base) == 0) { if (!ovTexCache[i].ok) return 0; *out = ovTexCache[i].tex; return 1; }
    if (ovTexCacheN >= OVTEX_MAX) return 0;
    int slot = ovTexCacheN++;
    snprintf(ovTexCache[slot].file, sizeof ovTexCache[slot].file, "%s", base);
    ovTexCache[slot].ok = loadAssetTexture(base, &ovTexCache[slot].tex);
    if (!ovTexCache[slot].ok) return 0;
    *out = ovTexCache[slot].tex;
    return 1;
}
static void freeOverlays(void)
{
    for (int i = 0; i < ovTexCacheN; i++) if (ovTexCache[i].ok) freeGfxTexture(&ovTexCache[i].tex);
    ovTexCacheN = 0;
    activeOverlayCount = 0;
}

// ---- virtual mouse cursor ----
// PC Ren'Py UI is mouse-driven (hover reveals buttons, click activates; some games require clicking a
// specific spot). The console has no mouse, so we drive a cursor with the left stick: hovering a HUD
// imagebutton shows its hover art (the idle art is often invisible), and a click activates it.
static float    cursorX, cursorY;
static int      cursorReady;
static uint64_t cursorLastMoveUs;     // when the stick last moved the cursor (drives auto-hide)
static uint64_t cursorMoveStartUs;    // when the CURRENT continuous stick-push began (drives the accel ramp)
static uint64_t cursorClickUntilUs;   // keep showing the click pose until this time (X = a brief press flash)
static int      cursorMovedFrame;     // 1 if the stick moved the cursor THIS frame (gates hover so it never fights the d-pad)
static GfxTexture spriteSheet; static int spriteSheetOk;   // packed sprite sheet (hand cursor art); see sprite-regions.h
#define CURSOR_IDLE_HIDE_US  4000000ull   // hide the cursor after ~4s with no stick movement
#define CURSOR_CLICK_HOLD_US 500000ull    // keep the click pose up ~500ms after an X press (was: only while held)
// The hand art is near-white, so a multiply tint recolours it cleanly; a dark outline drawn around it
// guarantees the cursor never blends into a same-colour background (a single hue alone could). To use
// red instead of green, change CURSOR_TINT to 0xFFFF2A2Au -- the black outline does the contrast work.
#define CURSOR_TINT     0xFF22FF22u   // bright green fill (white hand * this)
#define CURSOR_OUTLINE  0xFF000000u   // opaque-black silhouette ringed around the hand
#define CURSOR_OUTLINE_PX 2           // outline thickness (px the silhouette is offset)

static void loadCursor(void)
{
    spriteSheet = loadGfxTexture(SPRITES_SHEET_PATH);
    spriteSheetOk = (spriteSheet.w > 0 && spriteSheet.h > 0);
}

// Draw one sheet region at (x,y) at its native size (UVs are normalized to the sheet's full size),
// modulated by `tint` (COLOR_WHITE = the art's own colours; any colour multiplies the near-white hand).
static void drawSprite(int x, int y, SpriteRegion r, uint32_t tint)
{
    if (!spriteSheetOk) return;
    float u0 = (float)r.x / spriteSheet.w, v0 = (float)r.y / spriteSheet.h;
    float u1 = (float)(r.x + r.w) / spriteSheet.w, v1 = (float)(r.y + r.h) / spriteSheet.h;
    drawGfxTexture(x, y, r.w, r.h, spriteSheet, u0, v0, u1, v1, tint, GFX_FILTER_LINEAR);
}

// The console has no mouse, so the left stick drives the cursor. Two effects make it feel like a real
// pointer:
//   (1) TIME ACCELERATION -- the cursor starts slow (CURSOR_MIN_SPEED, for a precise tap) and ramps up
//       to CURSOR_MAX_SPEED the longer you hold a direction, resetting to slow the instant you let go.
//       This is the key to fine control on a DIGITAL input (e.g. WASD/keyboard mapped to the stick, or
//       d-pad-like pads), where the analog magnitude is always full -- a quick tap nudges a few px, a
//       sustained push sweeps across the screen.
//   (2) MAGNITUDE refinement -- on a TRUE analog stick a gentle tilt also moves slower than a full push.
//       Only the magnitude is shaped; direction comes from the unit stick vector, so diagonals stay true.
#define CURSOR_DEADZONE   0.18f      // ignore ~18% of travel around centre (stick jitter / rest drift)
#define CURSOR_MIN_SPEED  1.5f       // px/frame at the START of a move (fine targeting)
#define CURSOR_MAX_SPEED  20.0f      // px/frame once the ramp is full (fast traversal; tuned on DS3)
#define CURSOR_RAMP_US    550000.0f  // reach top speed ~0.55s into one continuous push
static void updateCursor(void)
{
    int sw = getGfxScreenWidth(), sh = getGfxScreenHeight();
    if (!cursorReady) { cursorX = sw * 0.5f; cursorY = sh * 0.5f; cursorReady = 1; }
    uint64_t now = sys_time_get_system_time();
    if (isPadButtonPressed(PAD_BTN_CROSS)) cursorClickUntilUs = now + CURSOR_CLICK_HOLD_US;  // click flash
    cursorMovedFrame = 0;
    Stick s = getPadLeftStick();
    float nx = s.x / 128.0f, ny = s.y / 128.0f;     // stick deflection, -1..1 per axis
    float mag = sqrtf(nx * nx + ny * ny);            // 0..~1.41 (diagonal)
    if (mag > CURSOR_DEADZONE)
    {
        cursorMovedFrame = 1;
        if (cursorMoveStartUs == 0) cursorMoveStartUs = now;       // a fresh push begins slow
        float ramp = (float)(now - cursorMoveStartUs) / CURSOR_RAMP_US;   // 0..1 over CURSOR_RAMP_US
        if (ramp > 1.0f) ramp = 1.0f;
        float t = (mag - CURSOR_DEADZONE) / (1.0f - CURSOR_DEADZONE);     // 0..1 deflection past deadzone
        if (t > 1.0f) t = 1.0f;
        float speed = CURSOR_MIN_SPEED + ramp * (CURSOR_MAX_SPEED - CURSOR_MIN_SPEED);  // (1) time accel
        speed *= 0.4f + 0.6f * t;                                  // (2) analog refinement (digital: t=1)
        cursorX += (nx / mag) * speed;                             // nx/mag = unit direction (mag>0 here)
        cursorY += (ny / mag) * speed;
        if (cursorX < 0) cursorX = 0; else if (cursorX > sw - 1) cursorX = (float)(sw - 1);
        if (cursorY < 0) cursorY = 0; else if (cursorY > sh - 1) cursorY = (float)(sh - 1);
        cursorLastMoveUs = now;
    }
    else cursorMoveStartUs = 0;   // stick centred/released -> the next move starts slow again
}

// Visible only briefly after the stick last moved (Ren'Py hides an idle mouse the same way).
static int cursorVisible(void)
{
    return cursorReady && (sys_time_get_system_time() - cursorLastMoveUs) < CURSOR_IDLE_HIDE_US;
}

// True only on frames where the stick actually moved the cursor. Menus use this (not cursorVisible)
// to drive hover-focus, so a cursor that's merely still-visible-but-idle doesn't re-grab focus every
// frame and override d-pad navigation. When the cursor is idle, the d-pad owns the focus.
static int cursorMoved(void) { return cursorMovedFrame; }

static void drawCursor(void)
{
    if (!cursorVisible()) return;
    int x = (int)(cursorX + 0.5f), y = (int)(cursorY + 0.5f);
    if (spriteSheetOk)   // hand art: pointing fingertip is the top-left of the sprite (the click point)
    {
        // resting/moving pose is the pointing hand; the click pose latches for ~500ms after an X press
        // (or stays up while X is held) so a quick click is clearly visible.
        int clicking = (sys_time_get_system_time() < cursorClickUntilUs) || isPadButtonHeld(PAD_BTN_CROSS);
        SpriteRegion r = spriteRegions[clicking ? SPRITE_HAND_CLICK : SPRITE_HAND];
        // Dark outline: the same silhouette drawn offset in 8 directions, so the bright hand always has a
        // contrasting border and never disappears into a matching background. Bright fill drawn on top.
        for (int dy = -CURSOR_OUTLINE_PX; dy <= CURSOR_OUTLINE_PX; dy += CURSOR_OUTLINE_PX)
            for (int dx = -CURSOR_OUTLINE_PX; dx <= CURSOR_OUTLINE_PX; dx += CURSOR_OUTLINE_PX)
                if (dx || dy) drawSprite(x + dx, y + dy, r, CURSOR_OUTLINE);
        drawSprite(x, y, r, CURSOR_TINT);
        return;
    }
    for (int k = 0; k <= 18; k++) fillGfxRectangle(x, y + k, (k / 2) + 2, 1, 0xFF000000u);          // fallback arrow
    for (int k = 2; k <= 15; k++) fillGfxRectangle(x + 1, y + k, (k / 2) > 0 ? (k / 2) : 1, 1, CURSOR_TINT);
}

// The HUD imagebutton under (px,py), or NULL. Topmost (last-drawn) wins; respects guards/suppress.
static const RbcOvWidget *findOverlayButtonAt(int cx, int cy, int cw, int ch, int px, int py)
{
    (void)ch;
    if (activeOverlayCount == 0) return NULL;
    Value sv;
    if (getVar("suppress_overlay", &sv) && (sv.t == VT_BOOL || sv.t == VT_INT) && sv.i) return NULL;
    float as = getGuiAssetScale(cw);
    for (int o = activeOverlayCount - 1; o >= 0; o--)
    {
        const RbcOverlay *ov = activeOverlays[o];
        for (int i = ov->widgetCount - 1; i >= 0; i--)
        {
            const RbcOvWidget *wd = &ov->widgets[i];
            if (wd->kind != OV_IMAGEBUTTON) continue;
            if (!isCondTrue(&prog, wd->guardExpr)) continue;
            GfxTexture t;
            if (!ovGetTex(wd->a, &t)) continue;
            int sx = cx + (int)(wd->x * as + 0.5f), sy = cy + (int)(wd->y * as + 0.5f);
            int w = (int)(t.w * as + 0.5f), h = (int)(t.h * as + 0.5f);
            if (px >= sx && px < sx + w && py >= sy && py < sy + h) return wd;
        }
    }
    return NULL;
}

// Activate a clicked HUD imagebutton. Save/Load/Preferences/Title route to our in-game menu (the same
// screen the Triangle button opens); other actions (custom game screens like the stats menu, skip/auto
// toggles) are inert for now -- a follow-up once those screens/prefs are emulated.
static void dispatchOverlayClick(const RbcOvWidget *wd)
{
    const char *a = wd && wd->action ? wd->action : "";
    if (strncmp(a, "menu:", 5) == 0 ||
        (strncmp(a, "call:", 5) == 0 && (strstr(a, "_game_menu") || strstr(a, "_main_menu"))))
    {
        capturePending = 1;
        enterGameMenuOn(a);   // route to the sub-screen the button targets (save/load/preferences), not always Save
        mode = M_GAMEMENU;
    }
}

// Draw active HUD overlays over the in-game scene: Image + ImageButton widgets whose guard holds.
// The imagebutton under the cursor shows its hover art (idle is often invisible). Native positions/
// sizes scale to the letterboxed content rect (same assetScale as sprites). Text readouts deferred.
static void drawActiveOverlays(int cx, int cy, int cw, int ch)
{
    if (activeOverlayCount == 0) return;
    // Ren'Py's `suppress_overlay` (set True by the splashscreen, False at start) hides the HUD.
    Value sv;
    if (getVar("suppress_overlay", &sv) && (sv.t == VT_BOOL || sv.t == VT_INT) && sv.i) return;
    const RbcOvWidget *hover = cursorVisible() ? findOverlayButtonAt(cx, cy, cw, ch, (int)(cursorX + 0.5f), (int)(cursorY + 0.5f)) : NULL;
    float as = getGuiAssetScale(cw);
    for (int o = 0; o < activeOverlayCount; o++)
    {
        const RbcOverlay *ov = activeOverlays[o];
        for (int i = 0; i < ov->widgetCount; i++)
        {
            const RbcOvWidget *wd = &ov->widgets[i];
            if (wd->kind == OV_TEXT) continue;                 // deferred
            if (!isCondTrue(&prog, wd->guardExpr)) continue;   // guard false -> skip
            const char *img = (wd == hover && wd->kind == OV_IMAGEBUTTON && wd->b && wd->b[0]) ? wd->b : wd->a;
            GfxTexture t;
            if (!ovGetTex(img, &t)) continue;
            int sx = cx + (int)(wd->x * as + 0.5f);
            int sy = cy + (int)(wd->y * as + 0.5f);
            int sw = (int)(t.w * as + 0.5f);
            int sh = (int)(t.h * as + 0.5f);
            drawGfxTexture(sx, sy, sw, sh, t, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
        }
    }
}

// Raw python is a no-op (assignments are lowered to ASSIGN) except `renpy.pause(N)` -- a timed
// hold of the current scene, common in intros. 1 = a pause started (the VM yields).
int runPyExec(const char *code)
{
    const char *pauseCall = code ? strstr(code, "renpy.pause") : NULL;
    if (!pauseCall) return 0;
    const char *paren = strchr(pauseCall, '(');
    double seconds = 0.0;   // 0 = wait for input only
    if (paren) { const char *arg = paren + 1; while (*arg == ' ') arg++; if (*arg && *arg != ')') seconds = atof(arg); }
    recordPauseFrame();   // checkpoint so rollback can land on the paused scene (e.g. the "Disconnected" message)
    beginScenePause(seconds);
    mode = M_PAUSE;
    return 1;
}

void runUserLine(const char *line)
{
    if (strstr(line, "nvl clear") || strstr(line, "nvl hide")) { curNvl = 0; clearSayNvl(); }
    else if (strncmp(line, "play ", 5) == 0 || strncmp(line, "stop ", 5) == 0) execSound(line);
}

// Reached whenever the program would end (Return at depth 0, End, off-the-end). During the
// splashscreen boot phase this hands off to the main menu instead of ending.
void endProgram(void)
{
    if (bootPhase == BOOT_SPLASH) { enterMainMenu(); return; }
    mode = M_DONE;
    int cx, cy, cw, ch; getSceneContentRect(&cx, &cy, &cw, &ch);
    showSayEnd(cw, "The End.");
}

void failVm(const char *message)
{
    snprintf(errText, sizeof errText, "%s", message);
    logError("[rpp] vm error: %s\n", message);
    mode = M_ERROR;
    int cx, cy, cw, ch; getSceneContentRect(&cx, &cy, &cw, &ch);
    showSayEnd(cw, errText);
}

// ---- main menu (M_MAINMENU drives mainmenu.c) ----

static void enterMainMenu(void)
{
    // config.main_menu_music: play it while the menu is up (the game's own `play music` on start
    // replaces it). Faithful to Ren'Py, which plays main_menu_music on entering the main menu.
    if (gui.mmMusic[0])
    {
        char cmd[160];
        snprintf(cmd, sizeof cmd, "play music \"%s\"", gui.mmMusic);
        execSound(cmd);
    }

    // If the game defines its OWN main_menu label (e.g. an imagemap menu, as RE:Alistair does), run
    // that in the VM rather than our built-in manifest menu -- the label drives the menu and jumps to
    // start / load / etc. itself. Otherwise show the built-in classic menu (mainmenu.c).
    int mmAddr = getRbcLabelAddr(&prog, "main_menu");
    if (mmAddr >= 0)
    {
        bootPhase = BOOT_GAME;   // the game's label owns the flow from here
        startVm(mmAddr);
        runVm();
        return;
    }
    buildMainMenu();   // build the menu + load its art (mainmenu.c)
    bootPhase = BOOT_MENU;
    mode = M_MAINMENU;
}

// Acts on the menu's chosen action: Start runs the `start` label; Quit leaves to the selector.
static void applyMainMenuAction(MmAction action)
{
    if (action == MM_ACTION_START)
    {
        freeMainMenu();
        bootPhase = BOOT_GAME;
        int startAddr = getRbcLabelAddr(&prog, "start");
        startVm(startAddr >= 0 ? startAddr : (int)prog.entryAddr);
        runVm();
    }
    else if (action == MM_ACTION_LOAD)
    {
        // "Load Game" (the "Continue" art button): open the Load file picker from the title.
        freeMainMenu();
        gmFromTitle = 1;
        enterGameMenuFromTitle();
        mode = M_GAMEMENU;
    }
    else if (action == MM_ACTION_QUIT) popScreen();
}

// ---- save / load bridge (the screen owns the visual state; savestate owns vm + vars) ----

static SaveScene loadBuf;   // stable backing for a loaded line's strings (curWho/curScene point in here)

// Snapshot the live dialogue + scene into a SaveScene and write it to the slot.
static int playSaveToSlot(const char *slot)
{
    SaveScene sc; memset(&sc, 0, sizeof sc);
    snprintf(sc.who,   sizeof sc.who,   "%s", curWho   ? curWho   : "");
    snprintf(sc.what,  sizeof sc.what,  "%s", curWhat  ? curWhat  : "");
    snprintf(sc.scene, sizeof sc.scene, "%s", curScene ? curScene : "");
    const char *mc = getSoundMusicCmd();
    snprintf(sc.music, sizeof sc.music, "%s", mc ? mc : "");
    sc.nvl = curNvl;
    sc.ingame = curIngame;
    sc.showCount = getSpriteCount() < SPR_MAX ? getSpriteCount() : SPR_MAX;
    for (int i = 0; i < sc.showCount; i++)
    {
        const Sprite *s = getSpriteAt(i);
        snprintf(sc.shows[i],  sizeof sc.shows[0],  "%s", s->src   ? s->src   : "");
        snprintf(sc.showAt[i], sizeof sc.showAt[0], "%s", s->atSrc ? s->atSrc : "");
        sc.showAtl[i] = s->atlId;   // save the ATL id so load restores placement/anim (not just the bare sprite)
    }
    // The slot label is the engine's store.save_name if the game set one, else the game's name.
    char name[64]; Value v;
    if (getVar("save_name", &v) && v.t == VT_STR && v.s && v.s[0]) snprintf(name, sizeof name, "%s", v.s);
    else snprintf(name, sizeof name, "%s", getGameName());
    return saveStateCapture(slot, &sc, name);
}

// Rebuild the scene + show the line from loadBuf (vars + vm position were already restored).
static void playApplyLoad(void)
{
    bootPhase = BOOT_GAME;   // loading a slot puts us in-game (matters when loaded from the title)
    resetHistory();
    curWho   = loadBuf.who[0]   ? loadBuf.who   : NULL;
    curWhat  = loadBuf.what;
    curScene = loadBuf.scene[0] ? loadBuf.scene : NULL;
    curNvl   = loadBuf.nvl;
    curIngame = loadBuf.ingame;
    resolveScene(&prog, curScene);
    clearSprites();
    // Pass the saved ATL id (not -1) so each sprite is re-shown WITH its transform (position/anim),
    // matching rollback. -1 only when the save predates the field (showAtl defaults 0 -> guard below).
    for (int i = 0; i < loadBuf.showCount; i++) showSprite(&prog, loadBuf.shows[i], loadBuf.showAt[i], loadBuf.showAtl[i]);
    curShow = loadBuf.showCount > 0 ? loadBuf.shows[loadBuf.showCount - 1] : NULL;
    restoreSoundMusic(loadBuf.music[0] ? loadBuf.music : NULL);

    Frame *f = appendHistory();   // seed the rollback timeline with the restored line
    if (f)
    {
        f->who = curWho; f->what = curWhat; f->scene = curScene; f->nvl = curNvl; f->ingame = curIngame; f->nvlPage = curNvlPage;
        f->musicCmd = getSoundMusicCmd(); f->showCount = getSpriteCount();
        for (int i = 0; i < getSpriteCount(); i++) { f->shows[i] = getSpriteAt(i)->src; f->showAt[i] = getSpriteAt(i)->atSrc; f->showAtl[i] = getSpriteAt(i)->atlId; }
    }
    mode = M_SAY;
    restartGuiCtc();
    renderInfo();
    int cx, cy, cw, ch; getSceneContentRect(&cx, &cy, &cw, &ch);
    if (!curNvl) { showSayLine(cx, cy, cw, ch, curWho, curWhat, 0, curIngame, 0, 1); return; }
    const char *who[1] = { curWho }, *what[1] = { curWhat };
    showSayNvlPage(cw, who, what, 1, 1);
}

// Acts on the in-game menu's chosen action. Return resumes the line we left; Main Menu / Quit have
// already passed the engine's yes/no prompt inside the menu. Save writes the slot and stays open;
// Load restores the slot and resumes play. (Preferences lands in GM5.)
static void applyGameMenuAction(GmAction action)
{
    switch (action)
    {
        // Return / Circle: in-game it resumes the line; opened from the title it goes back to the menu.
        case GM_RETURN:   freeGameMenu(); if (gmFromTitle) { gmFromTitle = 0; enterMainMenu(); } else mode = M_SAY; break;
        case GM_MAINMENU: freeGameMenu(); gmFromTitle = 0; enterMainMenu(); break;
        case GM_QUIT:     freeGameMenu(); popScreen();     break;
        case GM_DO_SAVE:
            playSaveToSlot(getGameMenuSelectedSlot());
            if (pendingThumbValid) saveThumbWrite(getGameMenuSelectedSlot(), pendingThumb, THUMB_W, THUMB_H);
            refreshGameMenuSlots();
            break;
        case GM_DO_LOAD:
        {
            // Restores vm + vars + scene and enters play -- works the same whether opened in-game or
            // from the title (the title "Load Game" path), since the save carries the full VM state.
            int lr = saveStateLoad(getGameMenuSelectedSlot(), &loadBuf);
            if (lr == 0) { freeGameMenu(); gmFromTitle = 0; playApplyLoad(); }
            else logWarn("[rpp] load: slot '%s' failed (rc=%d) -- incompatible/old save format? (stays in menu)\n",
                         getGameMenuSelectedSlot(), lr);   // was a SILENT no-op; now diagnosable
            break;
        }
        default: break;   // GM_PREFERENCES: its screen lands in GM5
    }
}

// ---- screen lifecycle ----

static void initPlay(void)
{
    logInfo("[rpp] play: init\n");
    loaded = 0;
    resetHistory();
    curNvl = 0;
    curIngame = 0;
    curWho = curWhat = curScene = curShow = NULL;

    initScene();   // allocate the offscreen targets `with` transitions cross-fade between
    loadCursor();  // optional hand/pointer art for the virtual cursor (res/cursor.png), else a drawn arrow

    // Safe default font first, so error rendering (failVm()) always has a font. Share it with
    // the text layer (loadGameFont may upgrade it in place below; saytext holds the address).
    font = openSystemFont(FONT_SANS);
    fontReady = 1;
    initSay(&font);
    initGameMenu(&font);

    // 1) Load bytecode.
    RpkFile r;
    int rc = openRpk(&r, getGameRpkPath());
    if (rc != 0) { failVm("could not open bundle"); return; }

    unsigned char *buf = NULL;
    long len = 0;
    int er = readRpkEntry(&r, "game.rbc", &buf, &len);
    closeRpk(&r);
    if (er != 0 || !buf) { failVm("could not read game.rbc"); return; }

    int pr = parseRbc(buf, len, &prog);
    free(buf);
    if (pr != 0) { failVm("could not parse bytecode"); return; }
    loaded = 1;
    initVm(&prog);   // hand the interpreter the loaded program
    initAssets(&prog);

    // 2) Load the GUI manifest FIRST. loadGui calls guiDefaults() which zeroes `gui`, then fills it
    //    from THIS game's game.gui (incl. text_font -> gui.dlgFont). This must precede loadGameFont:
    //    `gui` is a process-global that is NOT cleared on game exit, and loadGameFont prefers
    //    gui.dlgFont -- so if a previous game (e.g. Alistair, text_font=toony_loons.otf) ran first,
    //    a stale dlgFont would make this game try to load the wrong font and fall back. (Regression:
    //    play Alistair, exit, play broken -> broken's text rendered with the fallback font.)
    loadGui(getGameRpkPath());

    // 3) Upgrade to the dialogue font the manifest names (gui.dlgFont), else the script, else any
    //    bundled .ttf that opens.
    if (!loadGameFont(&prog, &font, &fontReady))
        logWarn("[rpp] keeping system font (no bundled font opened)\n");

    // 4) Establish default/define values (and init-region assignments) before play starts.
    initVars(&prog);

    // 4b') Load the in-game chat font (gui.igFont) if the game has an in-game textbox. Falls back to
    //      the dialogue font inside saytext if it isn't bundled / won't open.
    if (gui.igFont[0] && loadNamedFont(gui.igFont, &igFont)) { igFontReady = 1; initSayIngame(&igFont); logInfo("[rpp] ig font: loaded '%s'\n", gui.igFont); }
    else { initSayIngame(NULL); logWarn("[rpp] ig font: NOT loaded ('%s') -> chat falls back to dialogue font\n", gui.igFont); }

    // 3c) Start the audio mixer (so `play music/sound` in the opening works).
    initSound();

    // 3d) Run Ren'Py's init phase: the `__init__` prologue holds the top-level/init-block ops (var
    //     assigns, config, overlay registration via OverlayShow). Runs once, before the game boots.
    int initAddr = getRbcLabelAddr(&prog, "__init__");
    if (initAddr >= 0) { startVm(initAddr); runVmInit(); }

    // 4) Boot. Classic-theme games (the manifest defines a main menu) run the splashscreen
    //    label, then show the main menu, then start; games with no menu run from `start`
    //    directly, exactly as before.
    bootPhase = BOOT_GAME;
    if (gui.mmBg[0] || gui.mmBtnCount > 0)
    {
        bootPhase = BOOT_SPLASH;
        int splash = getRbcLabelAddr(&prog, "splashscreen");
        if (splash >= 0) { startVm(splash); runVm(); }   // endProgram() -> enterMainMenu
        else enterMainMenu();
    }
    else
    {
        startVm((int)prog.entryAddr);
        runVm();   // run until first Say / Menu / End
    }
}

static void resumePlay(void) {}
static void suspendPlay(void) {}

static void updatePlay(void)
{
    updateCursor();   // the virtual cursor tracks the left stick in every play mode
    switch (mode)
    {
        case M_SAY:
            tickSay();   // advance the typewriter reveal
            // X = forward; Circle = back through history; Triangle = open the game menu.
            if (isPadButtonPressed(PAD_BTN_TRIANGLE)) { capturePending = 1; enterGameMenu(); mode = M_GAMEMENU; break; }
            if (isPadButtonPressed(PAD_BTN_CROSS))
            {
                if (!isSayTypingDone()) { completeSayTyping(); break; }   // first press completes the reveal
                // With the cursor visible, a click on a HUD button activates it; otherwise advance.
                if (cursorVisible())
                {
                    int ccx, ccy, ccw, cch; getSceneContentRect(&ccx, &ccy, &ccw, &cch);
                    const RbcOvWidget *btn = findOverlayButtonAt(ccx, ccy, ccw, cch, (int)(cursorX + 0.5f), (int)(cursorY + 0.5f));
                    if (btn) { dispatchOverlayClick(btn); break; }
                }
                if (stepHistoryForward()) showFrame();
                else
                {
                    // At the latest line the on-screen scene/sprites are already correct (live, or
                    // rebuilt by the last showFrame); just restore the scalars and run the VM on.
                    const Frame *latest = getHistoryLatest();
                    if (latest)
                    {
                        curScene = latest->scene;
                        curShow  = latest->showCount > 0 ? latest->shows[latest->showCount - 1] : NULL;
                    }
                    runVm();   // pc already points at the next instruction
                }
            }
            else if (isPadButtonPressed(PAD_BTN_CIRCLE))
            {
                if (stepHistoryBack()) showFrame();
            }
            break;
        case M_MENU:
        {
            int moved = 0;
            if (isPadButtonPressed(PAD_BTN_DOWN)) { menuSel = (menuSel + 1) % menuCount; moved = 1; }
            if (isPadButtonPressed(PAD_BTN_UP))   { menuSel = (menuSel - 1 + menuCount) % menuCount; moved = 1; }
            if (moved) { int cx, cy, cw, ch; getSceneContentRect(&cx, &cy, &cw, &ch); showSayMenu(cw, menuCap, menuCount, menuSel); }
            if (isPadButtonPressed(PAD_BTN_CROSS)) { gotoVm(menuTarget[menuSel]); runVm(); }
            break;
        }
        case M_TRANS:
            // The transition plays itself out; X / DPAD-right skips to the end. Then the new scene
            // becomes the next "old" and the VM resumes (pc already points past the With).
            if (hasSceneTransitionElapsed() || isPadButtonPressed(PAD_BTN_CROSS) || isPadButtonPressed(PAD_BTN_RIGHT))
            {
                endSceneTransition();
                runVm();
            }
            break;
        case M_PAUSE:
            // Hold the current scene (drawn normally) for the pause's duration; X/-> skips.
            if (hasScenePauseElapsed() || isPadButtonPressed(PAD_BTN_CROSS) || isPadButtonPressed(PAD_BTN_RIGHT))
                runVm();   // resume past the pause
            break;
        case M_IMAGEMAP:
        {
            // Cursor hover: focus the hotspot under the pointer (Ren'Py mouse focus = point-in-rect), so
            // its hover art shows; a click activates it, a click over no hotspot does nothing. Only drives
            // focus while the cursor is actively moving (visible && moved), so a still cursor never fights
            // the d-pad. The hotspot rects come from drawImageMap's addFocus (one frame old).
            int curActive = cursorVisible() && cursorMoved();
            int hoverId = curActive ? focusAt((int)(cursorX + 0.5f), (int)(cursorY + 0.5f)) : -1;
            if (hoverId >= 0) setFocus(hoverId);
            if (isPadButtonPressed(PAD_BTN_UP))    moveFocus(0, -1);
            if (isPadButtonPressed(PAD_BTN_DOWN))  moveFocus(0,  1);
            if (isPadButtonPressed(PAD_BTN_LEFT))  moveFocus(-1, 0);
            if (isPadButtonPressed(PAD_BTN_RIGHT)) moveFocus(1,  0);
            if (isPadButtonPressed(PAD_BTN_CROSS) && !(curActive && hoverId < 0)) imageMapSelect();
            break;
        }
        case M_MAINMENU:
            // Pass cursor-DRIVES-focus = visible AND moved this frame, so a still cursor never fights the d-pad.
            applyMainMenuAction(updateMainMenu(cursorVisible() && cursorMoved(),
                                               (int)(cursorX + 0.5f), (int)(cursorY + 0.5f)));
            break;
        case M_GAMEMENU:
            applyGameMenuAction(updateGameMenu(cursorVisible() && cursorMoved(),
                                               (int)(cursorX + 0.5f), (int)(cursorY + 0.5f)));
            break;
        case M_DONE:
        case M_ERROR:
            if (isPadButtonPressed(PAD_BTN_CIRCLE)) popScreen();
            break;
        default: break;
    }
}

// ---- side image (show_side_image=ConditionSwitch): the speaking character's portrait beside the box ----
static GfxTexture sideTex; static int sideTexOk; static char sideTexName[48];
// While `who` speaks, Ren'Py shows their side image (character.py -> ui.image(side_image)). The image is
// a ConditionSwitch: evaluate each branch's condition (against the live vars, via the rbc expr VM) in
// order and draw the FIRST true branch's image, placed in the screen by the ConditionSwitch xalign/yalign
// (e.g. 0/1.0 = bottom-left). Lazy-loads + caches the chosen portrait (re-decodes only when it changes).
static void drawSideImage(int cx, int cy, int cw, int ch)
{
    const GuiSideImage *si = getGuiSideImage(curWho);
    if (!si) return;
    const char *img = NULL;
    for (int i = 0; i < si->count; i++)
        if (isCondTrue(&prog, si->exprId[i])) { img = si->img[i]; break; }   // first true branch wins
    if (!img) return;
    if (strcmp(sideTexName, img) != 0)   // changed -> (re)load
    {
        if (sideTexOk) { freeGfxTexture(&sideTex); sideTexOk = 0; }
        sideTexOk = loadAssetTexture(img, &sideTex);
        snprintf(sideTexName, sizeof sideTexName, "%s", img);
    }
    if (!sideTexOk) return;
    float as = getGuiAssetScale(cw);
    int w = (int)(sideTex.w * as + 0.5f), h = (int)(sideTex.h * as + 0.5f);
    int x = cx + (int)(si->alignX * (cw - w) + 0.5f);   // placed in the content rect by xalign/yalign
    int y = cy + (int)(si->alignY * (ch - h) + 0.5f);
    drawGfxTexture(x, y, w, h, sideTex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
}

// Draws the whole scene (letterbox, background, sprites, and -- in M_SAY/M_MENU/M_DONE --
// The UI that sits on top of the scene: the debug overlay plus the mode's dialogue/menu/end text.
// Passed to scene-draw, which calls it after the background + sprites (and skips it for the plain
// scene snapshot a transition/pause shows -- those modes match nothing here but the debug line).
static void drawOverlay(int cx, int cy, int cw, int ch)
{
    // (No dev scene/show HUD -- not a Ren'Py concept.)
    if      (mode == M_SAY)                       drawSayLine(cx, cy, cw, ch);
    else if (mode == M_MENU)
    {
        if (menuHasCaption) drawSayLine(cx, cy, cw, ch);   // the narrator caption in the dialogue box...
        drawSayMenu(cx, cy, cw, ch, menuSel, menuCount);   // ...with the choices above it
    }
    else if (mode == M_DONE || mode == M_ERROR)   drawSayEnd(cx, cy, cw, ch);
    // The side image is stacked AFTER the say window in the engine (ui.image(side_image) follows the
    // window in show_display_say), so it draws IN FRONT of the textbox, not behind it.
    if (mode == M_SAY) drawSideImage(cx, cy, cw, ch);
}

static void drawPlay(void)
{
    // Screenshot the game frame the menu was opened over: read it from the front display buffer here,
    // inside the gfx frame and before this (menu) frame flips, so the front buffer still holds the
    // game frame. GM_DO_SAVE later writes pendingThumb as the slot's PNG.
    if (capturePending)
    {
        capturePending = 0;
        pendingThumbValid = (captureSceneThumb(pendingThumb, THUMB_W, THUMB_H) == 0);
    }
    // The main menu and the in-game menu own their full-screen background (mm_root / gm_root), so
    // they draw straight to the framebuffer rather than over the live scene.
    if (mode == M_MAINMENU || mode == M_GAMEMENU || mode == M_IMAGEMAP)
    {
        int cx, cy, cw, ch; getSceneContentRect(&cx, &cy, &cw, &ch);
        if      (mode == M_MAINMENU) drawMainMenu(cx, cy, cw, ch);
        else if (mode == M_GAMEMENU) drawGameMenu(cx, cy, cw, ch);
        else                         drawImageMap(cx, cy, cw, ch);
        drawCursor();   // the cursor lives in every play mode (menus included)
        return;
    }
    renderSceneFrame(drawOverlay);
    { int cx, cy, cw, ch; getSceneContentRect(&cx, &cy, &cw, &ch); drawActiveOverlays(cx, cy, cw, ch); }
    drawCursor();   // virtual cursor on top of the live scene (all in-scene modes)
}

static void termPlay(void)
{
    freeTextTexture(&infoTex);   // debug overlay (the text layer owns the rest)
    freeSay();
    freeOverlays();
    freeImageMap();
    freeMainMenu();
    freeGameMenu();
    freeScene();
    termSound();
    freeAssets();
    freeGui();
    if (fontReady) { closeFont(&font); fontReady = 0; }
    if (igFontReady) { closeFont(&igFont); igFontReady = 0; }
    if (spriteSheetOk) { freeGfxTexture(&spriteSheet); spriteSheetOk = 0; }
    if (sideTexOk) { freeGfxTexture(&sideTex); sideTexOk = 0; sideTexName[0] = '\0'; }
    if (loaded) { freeRbc(&prog); loaded = 0; }
    resetVars();
    logInfo("[rpp] play: term\n");
}

Screen playScreen = {
    .init    = initPlay,
    .resume  = resumePlay,
    .update  = updatePlay,
    .draw    = drawPlay,
    .suspend = suspendPlay,
    .term    = termPlay,
    .status  = SCREEN_TERMINATED,
};
