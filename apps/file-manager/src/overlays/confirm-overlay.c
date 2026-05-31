#include "overlays/confirm-overlay.h"
#include "dbg.h"
#include "string-utilities.h"

static ConfirmCallback pending;

void askConfirm(const char *title, const char *message, ConfirmCallback onResult)
{
    logInfo("[confirm] %s - %s\n", strOrEmpty(title), strOrEmpty(message));
    pending = onResult;
}

static void show(void)   { confirmOverlay.status = OVERLAY_VISIBLE; }
static void hide(void)   { confirmOverlay.status = OVERLAY_HIDDEN; }
static void update(void) {}
static void draw(void)   {}
static void term(void)   { confirmOverlay.status = OVERLAY_TERMINATED; }

Overlay confirmOverlay = { show, hide, update, draw, term, OVERLAY_TERMINATED };
