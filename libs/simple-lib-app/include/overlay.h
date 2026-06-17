#pragma once

// overlay - lifecycle-managed UI layer drawn on top of a screen
// show() is the sole entry point; overlays lazy-init on first show.
// states: TERMINATED -> VISIBLE <-> HIDDEN -> TERMINATED

typedef enum OverlayStatus {
   OVERLAY_TERMINATED,
   OVERLAY_VISIBLE,
   OVERLAY_HIDDEN
} OverlayStatus;

typedef struct Overlay {
   void (*show)(void);
   void (*hide)(void);
   void (*update)(void);
   void (*draw)(void);
   void (*term)(void);
   OverlayStatus status;
} Overlay;

static inline int  isOverlayVisible(const Overlay *o) { return o->status == OVERLAY_VISIBLE; }
static inline void showOverlay(Overlay *o)   { if (o->status != OVERLAY_VISIBLE && o->show) o->show(); }
static inline void hideOverlay(Overlay *o)   { if (o->status == OVERLAY_VISIBLE && o->hide) o->hide(); }
static inline void updateOverlay(Overlay *o) { if (o->status == OVERLAY_VISIBLE && o->update) o->update(); }
static inline void drawOverlay(Overlay *o)   { if (o->status == OVERLAY_VISIBLE && o->draw) o->draw(); }
static inline void termOverlay(Overlay *o)   { if (o->status != OVERLAY_TERMINATED && o->term) o->term(); }
