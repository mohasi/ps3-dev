#pragma once

// theme.h - the file manager's flat/metro palette as a runtime-swappable struct. all chrome that used
// to be sprites (breadcrumb box, row highlight, separators, checkboxes, dialogs, scrollbar) reads its
// colours from the active theme, so switching themes is a single setActiveThemeIndex() call. draw code
// that reads the theme live (highlights, dialogs, bars) re-themes instantly; colours captured at init
// (checkbox, pre-rendered labels) apply on next init.
//
// themes come from two files, both read each launch and merged: res/themes.txt (the shipped built-in
// base layer) and /dev_hdd0/tmp/file-manager/themes.txt (the user's, seeded with a copy of the
// built-ins on first launch, never overwritten after - editing a block overrides that built-in,
// deleting one falls back to res, new blocks add the user's own). settings.txt beside the user file
// names the one to start in. L1/R1 in-app cycles through getThemeCount() themes via
// setActiveThemeIndex(), which also saves the choice to settings.txt. the file format and the parsing
// are shared with the other apps - see simple-lib-app's theme-registry.h.

#include <stdint.h>

typedef struct {
   // base surfaces
   uint32_t appBg;
   uint32_t divider;

   // raised chrome (breadcrumb box and other panels)
   uint32_t panelFill, panelBorder;

   // sliding side-menu slab
   uint32_t menuFill, menuBorder;

   // row selection highlight (lists + side menu)
   uint32_t highlightFill, highlightBorder;

   // hairline row separators
   uint32_t separator;

   // checkbox
   uint32_t checkFill;   // the whole checkbox colour (box outline + tick)

   // text
   uint32_t textPrimary;       // main labels
   uint32_t textSecondary;     // dim subtitles over dark surfaces
   uint32_t textOnHighlight;   // subtitle brightness that stays legible over the highlight

   // modal dialogs
   uint32_t scrim;             // dims the screen behind a modal
   uint32_t dialogFill, dialogBorder;

   // progress bar
   uint32_t progressTrack, progressFill;

   // scrollbar
   uint32_t scrollTrack, scrollThumb;

   // shared metro border thickness (px)
   int borderThickness;
} Theme;

// the palette in use. never NULL after initThemes(); points at a built-in until then.
extern const Theme *activeTheme;

// register the built-in themes, then merge in any from themes.txt, and select the first. call once at
// startup after the VFS is up (it reads/creates themes.txt).
void initThemes(void);

const char *getSettingsPath(void);   // the app's shared settings.txt (theme + other keys)

int         getThemeCount(void);
int         getActiveThemeIndex(void);
void        setActiveThemeIndex(int index);   // clamps to range; repoints activeTheme
