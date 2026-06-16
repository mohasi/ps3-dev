#pragma once

// Geometric focus navigation, translated from Ren'Py's renpy/display/focus.py. A screen registers
// each focusable widget's on-screen rectangle (with a stable id) as it lays out; the d-pad then
// moves focus to the NEAREST focusable in the pressed direction -- candidates off the movement axis
// are penalised by config.focus_crossrange_penalty (1024), exactly as Ren'Py navigates any UI with
// a keyboard/controller. This is layout-agnostic: a button column, a scrolling slot list, a grid,
// an imagemap -- whatever a game's screen draws -- is navigated the same way, so the menu code never
// assumes a particular shape. (Maps the engine, not one game.)

void clearFocus(void);                              // forget everything incl. the current focus (new screen)
void beginFocusFrame(void);                         // start re-registering this frame's focusables
void addFocus(int id, int x, int y, int w, int h);  // register a focusable widget's rect (id is stable across frames)
int  getFocusId(void);                                 // the focused widget's id, or -1 if none
int  isFocused(int id);                               // is `id` currently focused?
int  focusAt(int px, int py);                       // id of the focusable whose rect holds (px,py), else -1 (mouse focus)
void setFocus(int id);                              // force focus to a specific id
void moveFocus(int dx, int dy);                     // dx,dy in {-1,0,1}: move focus one step that direction
