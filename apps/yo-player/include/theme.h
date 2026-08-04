#pragma once

// theme.h - yo-player's palette as one runtime-swappable struct, so every colour the UI draws comes
// from one place (nothing is a sprite; all chrome is flat rectangles + text). The shipped theme is
// YouTube's own dark palette.
//
// Themes live in /dev_hdd0/tmp/yo-player/themes.txt, written on first launch with the built-in
// YouTube block so every key is visible to edit over FTP. Edit that block to restyle the app, or add
// a [Name] block of your own and point settings.txt at it with "theme=<name>" (lower case, spaces ->
// hyphens). There is no in-app theme switcher: colours are read at startup and apply on next launch.

#include <stdint.h>

typedef struct {
   uint32_t appBg;              // whole-screen background
   uint32_t surface;            // raised surface: thumbnail placeholder before the image lands
   uint32_t accent;             // youtube red: seek-bar fill, LIVE badge, volume pills, chapter accent
   uint32_t focusBorder;        // ring around the selected thumbnail

   uint32_t textPrimary;        // titles, durations, times
   uint32_t textSecondary;      // channel / views / age lines, button hints, sort label

   uint32_t badgeFill;          // translucent black behind every small overlay box (badges, stats, subtitles)
   uint32_t scrim;              // dims the video behind a full-screen overlay (description, chapters)
   uint32_t rowHighlight;       // selected row in the chapter picker

   uint32_t seekTrack;          // unplayed part of the seek bar
   uint32_t seekNotch;          // chapter boundary marks on the seek bar
   uint32_t watchedThumbTint;   // already-watched tiles: opaque darkening applied to the thumbnail

   int focusThickness;          // how far the focus ring extends past the thumbnail (px)
} Theme;

// the palette in use; never NULL (points at the built-in YouTube theme until initTheme runs).
extern const Theme *activeTheme;

// reads themes.txt (creating it from the built-in when missing) and selects the theme settings.txt
// names. call once at startup, after the VFS is up and before anything draws or rasterises a label.
void initTheme(void);
