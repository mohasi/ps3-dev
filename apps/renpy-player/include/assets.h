#pragma once

// Bundle assets: the image-name -> file map (from IMAGE ops), the current background,
// the active character sprites (Show/Hide/Scene), and the game's dialogue font.

#include "gfx.h"
#include "font.h"
#include "rbc.h"

#define SPR_MAX 6

typedef struct {
   char tag[32];        // sprite identity = first word of the show name ("eileen happy" -> "eileen")
   char file[64];       // resolved asset basename
   const char *src;     // image name (stable rbc string ptr)
   const char *atSrc;   // `at` clause (stable rbc string ptr), "" if none
   float xalign;        // classic position transforms: xpos == xanchor == this (00definitions.rpy)
   float yalign;        // ypos == yanchor: 1.0 bottom (default), 0.5 truecenter, 0.0 top
   const RbcAtl *atl;   // inline-ATL animation for this show (NULL if none); see atl.h
   int      atlId;      // the atl's id (>=0) so rollback can re-show with the same placement; -1 = none
   uint64_t atlStartUs; // when the ATL started (for elapsed-time playback)
   GfxTexture tex;
} Sprite;

// Builds the image map from IMAGE ops; resets bg + sprites. freeAssets releases everything.
void initAssets(const RbcProgram *p);
void freeAssets(void);

// Reads a bundled asset (by basename) and decodes it straight from memory into *out.
// Returns 1 on success.
int loadAssetTexture(const char *base, GfxTexture *out);

// Resolve a scene image name to its file and load it as the background (cached by file).
void resolveScene(const RbcProgram *p, const char *sceneName);

// Current background texture. Returns 1 when one is loaded.
int getBg(GfxTexture *out);

// Current background when it's a Solid colour (`scene black`) rather than an image.
// Returns 1 and the ARGB colour when so; getBg and getBgSolid are mutually exclusive.
int getBgSolid(uint32_t *colorOut);

// Sprites (tag-keyed; a re-show of the same tag replaces it).
void clearSprites(void);
void showSprite(const RbcProgram *p, const char *name, const char *at, int atlId);
void hideSprite(const char *name);
int           getSpriteCount(void);
const Sprite *getSpriteAt(int i);

// Loads the dialogue font: the script-named one first, else any bundled .ttf that opens.
// On success replaces *font (closing it if *fontReady) and sets *fontReady. Returns 1.
int loadGameFont(const RbcProgram *p, Font *font, int *fontReady);

// Loads a specific bundled font by basename (e.g. the in-game chat font, gui.igFont). On success
// fills *out and returns 1; 0 if the name is empty or the font is not in the bundle / won't open.
int loadNamedFont(const char *base, Font *out);
