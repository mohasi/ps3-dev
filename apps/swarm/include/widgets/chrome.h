#pragma once

// chrome - what surrounds every view: the title and clock along the top, the list of views and the
// network summary down the left, and the button hints along the bottom.

#include "font.h"

typedef enum {
   VIEW_DOWNLOADS,
   VIEW_COMPLETED,
   VIEW_SEARCH,
   VIEW_LOGS,
   VIEW_SETTINGS,
   VIEW_COUNT
} AppView;

// which half of the screen the pad is driving: the list of views on the left, or the rows on the right
typedef enum { FOCUS_SIDEBAR, FOCUS_LIST } AppFocus;

#define PANEL_TOP     120
#define PANEL_BOTTOM  980

#define CONTENT_X     430
#define CONTENT_WIDTH 1450
#define CONTENT_TOP   PANEL_TOP

void initChrome(Font *font);
void termChrome(void);

// Right and left move between the two halves; up and down pick the view while the left half has the
// pad. Returns the view now showing.
AppView updateChrome(void);
AppView getChromeView(void);
AppFocus getChromeFocus(void);

// the screen owns the rows, so it is what sends the pad back to the list of views when there are none
void setChromeFocus(AppFocus wanted);

// jump straight to a view with the pad on its list, for a search the on-screen keyboard just started
void showChromeView(AppView view);

// the results only appear in the list of views once a search has run
void setChromeSearchShown(int shown);

// what the square button does to whatever is picked right now, e.g. "Stop" or "Start"
void setChromeActionHint(const char *caption);
void setChromeActionShown(int shown);   // out of the footer entirely when it would do nothing
void setChromeDeleteShown(int shown);   // only a torrent can be deleted, so only then is it offered

// counts beside the first two entries, and the tunnel's line at the bottom left
void setChromeCounts(int downloading, int completed);
void setChromeVpn(const char *address, int failed, int usingTunnel);

void drawChrome(void);
