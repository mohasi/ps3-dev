#pragma once

// The scene compositing layer: draws the letterboxed background + sprites, plays `with`
// transitions (cross-fade / fade-through-black between offscreen scene snapshots), and holds the
// scene during `renpy.pause`. The dialogue/menu UI that sits on top is supplied by the screen as
// an overlay callback, so this layer needs no knowledge of modes, menus, or text.

typedef void (*SceneOverlay)(int cx, int cy, int cw, int ch);   // draws the UI on top of the scene

void initScene(void);   // allocate the offscreen targets (transitions need them)
void freeScene(void);

// The letterboxed frame the game draws within (4:3 art pillarboxed on 16:9, etc). Single source
// of truth for both wrap widths and box geometry; the aspect comes from the game's native res.
void getSceneContentRect(int *cx, int *cy, int *cw, int *ch);

// Render this frame: letterbox + background + sprites into the current target, `overlay` on top,
// then present -- or cross-fade from the previous frame while a transition is active.
void renderSceneFrame(SceneOverlay overlay);

// `with` dissolve/fade. 1 = a transition is now playing (the caller waits for it); 0 = instant.
int  beginSceneTransition(const char *name);
int  hasSceneTransitionElapsed(void);   // has the transition's duration run out?
void endSceneTransition(void);       // finalize: the new scene becomes the next "old"

// `renpy.pause(N)`: hold the current scene. seconds <= 0 = wait for input only.
void beginScenePause(double seconds);
int  hasScenePauseElapsed(void);        // has a timed pause run out? (always 0 for an input-only pause)

// Screenshot: reads the front display buffer (the last flipped frame -- letterbox + scene + dialogue),
// crops to the game content area (drops the letterbox bars) and box-downscales to outW x outH A8R8G8B8
// into `out` (outW*outH*4 bytes). Returns 0 on success. Call inside a gfx frame (e.g. from the draw
// path), before the menu frame flips, so the front buffer still holds the game frame.
int  captureSceneThumb(unsigned char *out, int outW, int outH);
