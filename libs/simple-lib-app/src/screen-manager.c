// screen-manager - owns the active screen + stack and drives the active screen (see screen-manager.h).
#include "screen-manager.h"
#include <stddef.h>

#define SCREEN_MAX_STACK 8

static Screen *screenStack[SCREEN_MAX_STACK];
static int screenDepth = 0;
static Screen *currentScreen = NULL;

void changeScreen(Screen *newScreen)
{
   if (currentScreen && currentScreen->term) currentScreen->term();
   if (currentScreen) currentScreen->status = SCREEN_TERMINATED;
   if (screenDepth > 0) screenDepth--;
   currentScreen = newScreen;
   if (currentScreen) {
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
   if (screenDepth > 0) {
      currentScreen = screenStack[screenDepth - 1];
      if (currentScreen->resume) currentScreen->resume();
      currentScreen->status = SCREEN_ACTIVE;
   } else {
      currentScreen = NULL;
   }
}

void updateScreen(void) { if (currentScreen && currentScreen->update) currentScreen->update(); }
void drawScreen(void)   { if (currentScreen && currentScreen->draw) currentScreen->draw(); }
