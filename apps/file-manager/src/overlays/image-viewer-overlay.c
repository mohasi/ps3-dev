// image-viewer-overlay - full-screen still-image viewer.
// Scans the chosen image's directory for sibling supported images, decodes them
// asynchronously (one in VRAM at a time), and lets the user pan/zoom and slide
// between them. Drawn on top of the home screen; closes on Circle.
#include "overlays/image-viewer-overlay.h"
#include "image-loader.h"
#include "gfx.h"
#include "pad.h"
#include "anim.h"              // applyEasing
#include "font.h"
#include "ui/label.h"
#include "file.h"              // joinPath, getParentPath, getBaseName, MAX_PATH_LEN
#include "string-utilities.h"  // strCopy, strEq, appendStr
#include "dbg.h"               // logError

#include <string.h>
#include <sys/sys_time.h>

#define COLOR_SCRIM        0xC8000000u  // ~78% black behind the image
#define SLIDE_DURATION_US  350000ULL    // 350ms slide
#define SLIDE_EASING       EASE_IN_OUT_CUBIC  // gentle start, accelerate, settle
#define ZOOM_STEP          0.02f        // per-frame zoom rate while L2/R2 held
#define ZOOM_MIN_FIT       0.1f         // safety floor below fit-to-screen
#define ZOOM_MAX           5.0f         // 500%
#define PAN_STEP           20           // pixels per D-pad step
#define MAX_IMAGES         512

// filename caption, top-left
#define NAME_X             40
#define NAME_Y             28
#define NAME_SIZE          24
#define NAME_MAX_WIDTH     1400
#define COLOR_NAME         0xFFFFFFFFu     // solid white
#define COLOR_CAPTION_BG   0x80000000u     // 50% black plate behind the caption
#define CAPTION_PAD_X      12              // plate padding around the text
#define CAPTION_PAD_Y      6
#define CAPTION_VISIBLE_US 3000000ULL      // caption auto-hides after 3s

typedef enum {
    SLIDE_NONE,      // settled (or initial open): no motion
    SLIDE_TO_NEXT,   // new image slides in from the right
    SLIDE_TO_PREV    // new image slides in from the left
} SlideDirection;

// Only one image is ever held in VRAM at a time. VRAM has no per-allocation
// free (gfx.c is a bump allocator), so we capture a mark on open and reset back
// to it before each upload -- reclaiming the previous image's memory and reusing
// it, exactly as screen-manager.c does across screens. That keeps usage flat at
// one image no matter how many are viewed, instead of leaking until VRAM runs
// out. The trade-off vs. the old design: no two-image cross-slide; the new image
// slides in over the dimmed background while the old one is released.
static struct {
    char dir[MAX_PATH_LEN];
    char names[MAX_IMAGES][IMAGE_NAME_MAX];  // supported images in dir, sorted
    int  imageCount;
    int  currentIndex;

    GfxTexture currentTex;
    SlideDirection slideDir;   // a slide-in is animating when != SLIDE_NONE
    uint64_t slideStartUs;

    int  loading;              // an async decode is in flight
    int  pendingIndex;         // names[] index being loaded
    SlideDirection pendingDir; // how to reveal it once ready
    uint64_t captionShownUs;   // when the caption was last (re)set; for auto-hide
    int  captionIsError;       // caption shows an error; never auto-hide

    float zoom;                // multiplier applied to currentTex
    int   panX, panY;          // screen-space top-left of the scaled image

    int screenW, screenH;
} state;

// filename caption (lazy-initialised on first open, kept across re-opens)
static Font   nameFont;
static int    nameFontReady;
static Label  nameLabel;

// VRAM high-water mark captured on open; image uploads sit above it and are
// reclaimed by resetting back to it (see note above).
static size_t imgVramMark;

// forward decls (referenced by the Overlay table below, defined further down)
static void show(void);
static void hide(void);
static void update(void);
static void draw(void);
static void term(void);

Overlay imageViewerOverlay = { show, hide, update, draw, term, OVERLAY_TERMINATED };

// the zoom an image first appears at: fit-to-screen when larger than the screen,
// otherwise 1:1.
static float initialZoom(const GfxTexture *tex)
{
    if (tex->w <= 0 || tex->h <= 0) return 1.0f;
    float fitW = (float)state.screenW / (float)tex->w;
    float fitH = (float)state.screenH / (float)tex->h;
    float fit  = (fitW < fitH) ? fitW : fitH;
    return (fit < 1.0f) ? fit : 1.0f;
}

// initial view: the image at its initialZoom(), centered on screen.
static void computeFitView(const GfxTexture *tex, float *zoom, int *panX, int *panY)
{
    float z = initialZoom(tex);
    *zoom = z;
    *panX = (state.screenW - (int)(tex->w * z)) / 2;
    *panY = (state.screenH - (int)(tex->h * z)) / 2;
}

// lower zoom bound: the initial view zoom, so zoom-out returns to exactly how the
// image first appeared (and no further), with a small floor for safety.
static float minZoomFor(const GfxTexture *tex)
{
    float z = initialZoom(tex);
    return (z < ZOOM_MIN_FIT) ? ZOOM_MIN_FIT : z;
}

// keeps the scaled image covering the screen when larger than it, or centered
// when smaller, so panning can never reveal a gap at an edge.
static void clampPan(void)
{
    int scaledW = (int)(state.currentTex.w * state.zoom);
    int scaledH = (int)(state.currentTex.h * state.zoom);

    if (scaledW > state.screenW) {
        if (state.panX > 0) state.panX = 0;
        if (state.panX < state.screenW - scaledW) state.panX = state.screenW - scaledW;
    } else {
        state.panX = (state.screenW - scaledW) / 2;
    }

    if (scaledH > state.screenH) {
        if (state.panY > 0) state.panY = 0;
        if (state.panY < state.screenH - scaledH) state.panY = state.screenH - scaledH;
    } else {
        state.panY = (state.screenH - scaledH) / 2;
    }
}

// changes the zoom while keeping the screen-centre point pinned, so zooming
// homes in on whatever is in the middle of the view rather than drifting from
// the top-left corner. clampPan() then keeps the result within bounds.
static void zoomTo(float newZoom)
{
    if (newZoom == state.zoom) return;
    float cx = state.screenW * 0.5f;
    float cy = state.screenH * 0.5f;
    float ratio = newZoom / state.zoom;
    state.panX = (int)(cx - (cx - (float)state.panX) * ratio);
    state.panY = (int)(cy - (cy - (float)state.panY) * ratio);
    state.zoom = newZoom;
    clampPan();
}

// opens the caption font once. Kept separate from setCaption so the caller can
// capture the VRAM mark *after* the font is allocated (so the font lives below
// the mark and survives the per-image resets). The label's text texture is
// rendered above the mark and re-rendered as needed (renderFont self-heals a
// slot whose offset falls past a reset).
static void ensureCaptionFont(void)
{
    if (nameFontReady) return;
    nameFont = openSystemFont(FONT_POP);
    initLabel(&nameLabel, &nameFont, NAME_X, NAME_Y, NAME_MAX_WIDTH, AUTO,
              NAME_SIZE, COLOR_NAME, TEXT_NOWRAP_ELLIPSIS, "");
    nameFontReady = 1;
}

// sets the top-left caption to names[index] with an optional suffix:
// "(loading...)", "(image too large)", etc. Pass NULL for suffix on success.
// Error suffixes (non-NULL, non-loading) persist and never auto-hide.
static void setCaption(int index, const char *suffix)
{
    ensureCaptionFont();
    if (index < 0 || index >= state.imageCount) { setLabelText(&nameLabel, ""); return; }

    char buf[IMAGE_NAME_MAX + 40];
    int o = 0;
    appendStr(buf, sizeof buf, &o, state.names[index]);
    if (suffix) {
        appendStr(buf, sizeof buf, &o, "  ");
        appendStr(buf, sizeof buf, &o, suffix);
    }
    buf[o] = '\0';
    setLabelText(&nameLabel, buf);

    // An error suffix means the caption stays visible permanently; otherwise
    // start the auto-hide timer once the image loads (not while loading).
    int isLoading = (suffix && strCmpICase(suffix, "(loading...)") == 0);
    state.captionIsError = (suffix && !isLoading);
    if (!suffix) state.captionShownUs = sys_time_get_system_time();
}

// drops the caption's text-texture slot so the next setCaption() allocates a
// fresh one. Required after a VRAM reset: renderFont reuses a slot whose offset
// is still < getUsedGfxVram(), and a stale offset can land inside the image we
// just uploaded. Clearing the cached text also forces a re-render.
static void invalidateCaption(void)
{
    if (!nameFontReady) return;
    nameLabel.tt.tex.offset = 0;
    nameLabel.tt.tex.w = 0;
    nameLabel.tt.tex.h = 0;
    nameLabel.tt.slotW = 0;
    nameLabel.tt.slotH = 0;
    nameLabel.text[0] = '\0';
}

// reclaims all image/caption VRAM back to the open-time mark. Waits for the RSX
// first, so we never overwrite or free a texture the GPU is still drawing from
// (doing so corrupts the display and can hang the GPU).
static void reclaimImageVram(void)
{
    finishGfx();
    resetGfxVram(imgVramMark);
    invalidateCaption();
}

// begins an async load of names[index]. dir is how it should appear once ready:
// SLIDE_NONE replaces the current image in place (initial open), otherwise it
// slides in from that direction. supersedes any load already in flight.
static void requestLoad(int index, SlideDirection dir)
{
    char full[MAX_PATH_LEN];
    joinPath(full, MAX_PATH_LEN, state.dir, state.names[index]);

    state.loading     = 1;
    state.pendingIndex = index;
    state.pendingDir  = dir;
    requestImageAsync(full);
    setCaption(index, "(loading...)");
}

int openImageViewer(const char *imagePath)
{
    if (!isSupportedImageFormat(imagePath)) return -1;

    state.screenW = getGfxScreenWidth();
    state.screenH = getGfxScreenHeight();

    getParentPath(imagePath, state.dir, sizeof(state.dir));
    state.imageCount = listSupportedImages(state.dir, state.names, MAX_IMAGES);

    // locate the opened image within the scan
    const char *base = getBaseName(imagePath);
    state.currentIndex = -1;
    for (int i = 0; i < state.imageCount; i++) {
        if (strEq(state.names[i], base)) { state.currentIndex = i; break; }
    }
    if (state.currentIndex < 0) {
        // not found (e.g. listing changed under us): fall back to a 1-item list
        strCopy(state.names[0], IMAGE_NAME_MAX, base);
        state.imageCount   = 1;
        state.currentIndex = 0;
    }

    // Capture the VRAM reclaim point AFTER the caption font is allocated, so the
    // font sits below the mark and survives the per-image resets. Image textures
    // (and the caption's text slot) live above it and are reclaimed by resetting
    // back here before each upload. Captured fresh each open so we never sit
    // below whatever the home screen allocated since we last closed.
    ensureCaptionFont();
    imgVramMark = getUsedGfxVram();

    // start with no image; the first decode lands via the async path below
    memset(&state.currentTex, 0, sizeof(state.currentTex));
    state.slideDir = SLIDE_NONE;
    state.zoom = 1.0f;
    state.panX = state.panY = 0;

    // decode the opening image asynchronously (SLIDE_NONE = drop straight in)
    requestLoad(state.currentIndex, SLIDE_NONE);

    showOverlay(&imageViewerOverlay);
    return 0;
}

static void show(void) { imageViewerOverlay.status = OVERLAY_VISIBLE; }

static void hide(void)
{
    // reclaim the image (and caption-text) VRAM so we don't hold a large
    // texture while the user is back in the file list. The font stays (it lives
    // below the mark); the caption re-renders on the next open.
    reclaimImageVram();
    memset(&state.currentTex, 0, sizeof(state.currentTex));
    imageViewerOverlay.status = OVERLAY_HIDDEN;
}

static void term(void)
{
    // Image VRAM is reclaimed on hide(); the screen teardown that follows resets
    // VRAM anyway. Here we just release the font and clear our state.
    if (nameFontReady) {
        closeFont(&nameFont);
        nameFontReady = 0;
    }
    memset(&state, 0, sizeof(state));
    imageViewerOverlay.status = OVERLAY_TERMINATED;
}

// next/prev index from base, wrapping.
static int wrapIndex(int base, int delta)
{
    int n = base + delta;
    if (n < 0) n = state.imageCount - 1;
    if (n >= state.imageCount) n = 0;
    return n;
}

static void update(void)
{
    // Circle always closes (cancels any in-flight load implicitly: its result
    // is dropped on the next requestImageAsync or on term).
    if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
        hideOverlay(&imageViewerOverlay);
        return;
    }

    // waiting on an async decode
    if (state.loading) {
        // navigation while loading retargets (supersedes) the current load
        if (state.imageCount > 1) {
            if (isPadButtonPressed(PAD_BTN_L1)) { requestLoad(wrapIndex(state.pendingIndex, -1), SLIDE_TO_PREV); return; }
            if (isPadButtonPressed(PAD_BTN_R1)) { requestLoad(wrapIndex(state.pendingIndex, +1), SLIDE_TO_NEXT); return; }
        }

        ImageBuffer buf;
        int r = pollImageAsync(&buf);
        if (r == 1) {
            // reclaim the previous image's VRAM before allocating the new one,
            // so only a single image is ever resident (no growth, no spike).
            reclaimImageVram();
            memset(&state.currentTex, 0, sizeof(state.currentTex));

            GfxTexture tex = uploadImageBuffer(&buf);
            freeImageBuffer(&buf);
            state.loading = 0;

            if (tex.offset == 0) {
                // legal size but the upload failed (e.g. out of VRAM): land on
                // this entry with a blank background (the old image was already
                // released above) so the user can still navigate past it.
                logError("[image-viewer] VRAM upload failed: %s\n", state.names[state.pendingIndex]);
                state.currentIndex = state.pendingIndex;
                state.slideDir     = SLIDE_NONE;
                setCaption(state.currentIndex, "(out of vram)");
                return;
            }

            state.currentTex   = tex;
            state.currentIndex = state.pendingIndex;
            computeFitView(&state.currentTex, &state.zoom, &state.panX, &state.panY);
            state.slideDir     = state.pendingDir;  // SLIDE_NONE on initial open = no motion
            state.slideStartUs = sys_time_get_system_time();
            setCaption(state.currentIndex, NULL);  // success: caption auto-hides
        } else if (r == -1) {
            // can't decode (e.g. rejected as too large for the RSX): land on this
            // entry with a blank background, releasing the previous image. We
            // advance currentIndex so L1/R1 can move past it in either direction
            // instead of repeatedly retrying the same unloadable image.
            state.loading = 0;
            logError("[image-viewer] decode failed: %s\n", state.names[state.pendingIndex]);
            reclaimImageVram();
            memset(&state.currentTex, 0, sizeof(state.currentTex));
            state.currentIndex = state.pendingIndex;
            state.slideDir     = SLIDE_NONE;
            setCaption(state.currentIndex, "(image too large)");
        }
        return;  // no zoom/pan while loading
    }

    // let an in-progress slide-in finish; ignore other input until it settles
    if (state.slideDir != SLIDE_NONE) {
        if (sys_time_get_system_time() - state.slideStartUs >= SLIDE_DURATION_US)
            state.slideDir = SLIDE_NONE;
        return;
    }

    // L1 / R1 navigate (wrapping): kick off an async load, keeping the current
    // image on screen until the new one is ready.
    if (isPadButtonPressed(PAD_BTN_L1) && state.imageCount > 1) {
        requestLoad(wrapIndex(state.currentIndex, -1), SLIDE_TO_PREV);
        return;
    }
    if (isPadButtonPressed(PAD_BTN_R1) && state.imageCount > 1) {
        requestLoad(wrapIndex(state.currentIndex, +1), SLIDE_TO_NEXT);
        return;
    }

    // L2 / R2 zoom, centred on the middle of the view
    if (isPadButtonHeld(PAD_BTN_L2)) {
        float minZoom = minZoomFor(&state.currentTex);
        float z = state.zoom - ZOOM_STEP;
        if (z < minZoom) z = minZoom;
        zoomTo(z);
    }
    if (isPadButtonHeld(PAD_BTN_R2)) {
        float z = state.zoom + ZOOM_STEP;
        if (z > ZOOM_MAX) z = ZOOM_MAX;
        zoomTo(z);
    }

    // D-pad pan
    if (isPadButtonHeld(PAD_BTN_UP))    { state.panY += PAN_STEP; clampPan(); }
    if (isPadButtonHeld(PAD_BTN_DOWN))  { state.panY -= PAN_STEP; clampPan(); }
    if (isPadButtonHeld(PAD_BTN_LEFT))  { state.panX += PAN_STEP; clampPan(); }
    if (isPadButtonHeld(PAD_BTN_RIGHT)) { state.panX -= PAN_STEP; clampPan(); }
}

static void draw(void)
{
    fillGfxRectangle(0, 0, state.screenW, state.screenH, COLOR_SCRIM);

    if (state.currentTex.offset) {
        int scaledW = (int)(state.currentTex.w * state.zoom);
        int scaledH = (int)(state.currentTex.h * state.zoom);

        // during a slide-in the new image enters from the side over the dimmed
        // background (the previous image has already been released).
        int offsetX = 0;
        if (state.slideDir != SLIDE_NONE) {
            uint64_t elapsed = sys_time_get_system_time() - state.slideStartUs;
            float t = (float)elapsed / (float)SLIDE_DURATION_US;
            if (t > 1.0f) t = 1.0f;
            float e = applyEasing(t, SLIDE_EASING);
            float from = (state.slideDir == SLIDE_TO_NEXT) ? (float)state.screenW
                                                           : -(float)state.screenW;
            offsetX = (int)(from * (1.0f - e));
        }

        drawGfxTexture(state.panX + offsetX, state.panY, scaledW, scaledH,
                       state.currentTex, 0.0f, 0.0f, 1.0f, 1.0f,
                       0xFFFFFFFF, GFX_FILTER_LINEAR);
    }

    // filename caption on top: stays while loading, stays permanently on error,
    // otherwise auto-hides a few seconds after it was shown. drawn over a padded
    // semi-transparent black plate so it stays readable on any image.
    if (nameFontReady) {
        uint64_t shownFor = sys_time_get_system_time() - state.captionShownUs;
        if ((state.loading || state.captionIsError || shownFor < CAPTION_VISIBLE_US) && nameLabel.tt.tex.w > 0) {
            fillGfxRectangle(NAME_X - CAPTION_PAD_X, NAME_Y - CAPTION_PAD_Y,
                             nameLabel.tt.tex.w + CAPTION_PAD_X * 2,
                             nameLabel.tt.tex.h + CAPTION_PAD_Y * 2,
                             COLOR_CAPTION_BG);
            drawLabel(&nameLabel);
        }
    }
}
