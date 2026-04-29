// overlay - lifecycle-managed UI layer drawn on top of a screen
#ifndef APP_SAMPLE_OVERLAY_H
#define APP_SAMPLE_OVERLAY_H

typedef enum OverlayStatus {
    OVERLAY_TERMINATED,
    OVERLAY_INITIALISED,
    OVERLAY_VISIBLE,
    OVERLAY_HIDDEN
} OverlayStatus;

typedef struct Overlay {
    void (*init)(void);
    void (*show)(void);
    void (*hide)(void);
    void (*update)(void);
    void (*draw)(void);
    void (*term)(void);
    OverlayStatus status;
} Overlay;

#endif // APP_SAMPLE_OVERLAY_H
