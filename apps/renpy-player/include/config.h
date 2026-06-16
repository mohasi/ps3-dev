#pragma once

// Player-wide configuration shared by the screens.

#define MAX_CHOICES 16   // most in-game menu choices shown at once (VM, screen, and text layer)

// Games live OUTSIDE the app: drop any number of .rpk files into RENPY_ROOT and the player picks
// one up at startup (see gamepath.c). Each game's saves go in RENPY_ROOT/<rpk-name>/ (created on
// first save). The rr*g.png are Ren'Py ENGINE assets (RoundRect templates) -- identical for every
// game, so they ship with the PLAYER (res\ -> USRDIR), not per-game.
#define RENPY_ROOT  "/dev_hdd0/renpy"           // where games (.rpk) and their save folders live
#define USRDIR_PATH "/dev_hdd0/game/RENPLAY01/USRDIR"
#define SPRITES_SHEET_PATH USRDIR_PATH "/sprites.png"  // packed sprite sheet (sprite-packer); see sprite-regions.h (hand cursor, ...)
#define RR12G_PATH  USRDIR_PATH "/rr12g.png"   // RoundRect template, size 12 (native > 640)
#define RR6G_PATH   USRDIR_PATH "/rr6g.png"    // RoundRect template, size 6  (native <= 640)
#define RRVSCROLLBAR_PATH       USRDIR_PATH "/rrvscrollbar.png"        // vscrollbar track (img _roundrect/rrvscrollbar, ycap 12)
#define RRVSCROLLBAR_THUMB_PATH USRDIR_PATH "/rrvscrollbar_thumb.png"  // vscrollbar thumb (img _roundrect/rrvscrollbar_thumb)
