#pragma once

// screen - lifecycle-managed UI element

typedef enum ScreenStatus {
    SCREEN_TERMINATED,
    SCREEN_INITIALISED,
    SCREEN_ACTIVE,
    SCREEN_SUSPENDED
} ScreenStatus;

typedef struct Screen {
    void (*init)(void);
    void (*resume)(void);
    void (*update)(void);
    void (*draw)(void);
    void (*suspend)(void);
    void (*term)(void);
    ScreenStatus status;
} Screen;

// top-level navigation
extern Screen *currentScreen;

void changeScreen(Screen *newScreen);
void pushScreen(Screen *newScreen);
void popScreen(void);

static inline void updateScreen(void) { if (currentScreen && currentScreen->update) currentScreen->update(); }
static inline void drawScreen(void)   { if (currentScreen && currentScreen->draw) currentScreen->draw(); }
