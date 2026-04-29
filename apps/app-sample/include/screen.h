// screen - lifecycle-managed UI element
#ifndef APP_SAMPLE_SCREEN_H
#define APP_SAMPLE_SCREEN_H

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

#endif // APP_SAMPLE_SCREEN_H
