// screen - lifecycle-managed UI element
#include "screen.h"
#include "gfx.h"
#include <stddef.h>

#define SCREEN_MAX_STACK 8

static Screen *screenStack[SCREEN_MAX_STACK];
static size_t  vramMarks[SCREEN_MAX_STACK];
static int screenDepth = 0;
Screen *currentScreen = NULL;

// ============================================================================
// top-level navigation
// ============================================================================

void changeScreen(Screen *newScreen)
{
    if (currentScreen && currentScreen->term) currentScreen->term();
    if (currentScreen) currentScreen->status = SCREEN_TERMINATED;
    if (screenDepth > 0) {
        screenDepth--;
        resetGfxVram(vramMarks[screenDepth]);
    }
    currentScreen = newScreen;
    if (currentScreen) {
        vramMarks[screenDepth] = getUsedGfxVram();
        screenStack[screenDepth++] = currentScreen;
        if (currentScreen->init) currentScreen->init();
        currentScreen->status = SCREEN_ACTIVE;
    } else {
        screenDepth = 0;
    }
}

void pushScreen(Screen *newScreen)
{
    if (!newScreen || screenDepth >= SCREEN_MAX_STACK) return;
    if (currentScreen && currentScreen->suspend) currentScreen->suspend();
    if (currentScreen) currentScreen->status = SCREEN_SUSPENDED;
    currentScreen = newScreen;
    vramMarks[screenDepth] = getUsedGfxVram();
    screenStack[screenDepth++] = currentScreen;
    if (currentScreen->init) currentScreen->init();
    currentScreen->status = SCREEN_ACTIVE;
}

void popScreen(void)
{
    if (screenDepth <= 0) return;
    if (currentScreen->term) currentScreen->term();
    currentScreen->status = SCREEN_TERMINATED;
    screenDepth--;
    resetGfxVram(vramMarks[screenDepth]);
    if (screenDepth > 0) {
        currentScreen = screenStack[screenDepth - 1];
        if (currentScreen->resume) currentScreen->resume();
        currentScreen->status = SCREEN_ACTIVE;
    } else {
        currentScreen = NULL;
    }
}


