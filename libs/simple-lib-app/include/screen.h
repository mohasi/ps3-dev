#pragma once

// screen - the contract a UI screen implements (lifecycle vtable + status).
// the navigation/stack that drives screens lives in screen-manager.h.

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
