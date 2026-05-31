#include "overlays/progress-overlay.h"
#include "dbg.h"
#include "string-utilities.h"

void beginProgress(const char *title)
{
    logInfo("[progress] begin: %s\n", strOrEmpty(title));
    progressOverlay.status = OVERLAY_VISIBLE;
}

void setProgress(int done, int total, const char *currentItem)
{
    logInfo("[progress] %d/%d: %s\n", done, total, strOrEmpty(currentItem));
}

void endProgress(void)
{
    logInfo("[progress] end\n");
    progressOverlay.status = OVERLAY_HIDDEN;
}

static void show(void)   { progressOverlay.status = OVERLAY_VISIBLE; }
static void hide(void)   { progressOverlay.status = OVERLAY_HIDDEN; }
static void update(void) {}
static void draw(void)   {}
static void term(void)   { progressOverlay.status = OVERLAY_TERMINATED; }

Overlay progressOverlay = { show, hide, update, draw, term, OVERLAY_TERMINATED };
