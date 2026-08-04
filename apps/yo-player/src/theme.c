// theme.c - yo-player's palette and its themes.txt file (see theme.h). The file format and the parsing
// live in simple-lib-app's theme-registry; this file is the built-in palette, the field table that maps
// its colours onto "key=" lines, and the two file paths.

#include "theme.h"
#include "theme-registry.h"
#include "vfs.h"                // readFile / makeDir / fileExists
#include "settings-file.h"      // findSettingValue
#include "dbg.h"                // logInfo / logError

#define TMP_DIR        "/dev_hdd0/tmp/yo-player"
#define THEMES_PATH    TMP_DIR "/themes.txt"
#define SETTINGS_PATH  TMP_DIR "/settings.txt"

#define SETTINGS_BUFFER 4096

// YouTube's own dark palette, taken from the TV app: #0F0F0F page, #212121 surfaces, #AAAAAA
// secondary text, #FF0000 brand red, white focus ring. The translucent blacks and the watched-tile
// tint are yo-player's own, carried over from before this file existed.
static const Theme themeYouTube = {
   .appBg            = 0xFF0F0F0F,
   .surface          = 0xFF212121,
   .accent           = 0xFFFF0000,
   .focusBorder      = 0xFFFFFFFF,
   .textPrimary      = 0xFFFFFFFF,
   .textSecondary    = 0xFFAAAAAA,
   .badgeFill        = 0xCC000000,
   .scrim            = 0xD9000000,
   .rowHighlight     = 0x28FFFFFF,
   .seekTrack        = 0x66FFFFFF,
   .seekNotch        = 0xFF000000,
   .watchedThumbTint = 0xFF4D4D4D,   // opaque so the focus ring can't bleed through a watched tile
   .focusThickness   = 3,
};

// every colour field by name, so a themes.txt "key=value" line maps straight onto the struct.
// focusThickness is not user-tunable - a block inherits the built-in's value via the seed copy.
static const ThemeColorField COLOR_FIELDS[] = {
   { "appBg",            offsetof(Theme, appBg)            },
   { "surface",          offsetof(Theme, surface)          },
   { "accent",           offsetof(Theme, accent)           },
   { "focusBorder",      offsetof(Theme, focusBorder)      },
   { "textPrimary",      offsetof(Theme, textPrimary)      },
   { "textSecondary",    offsetof(Theme, textSecondary)    },
   { "badgeFill",        offsetof(Theme, badgeFill)        },
   { "scrim",            offsetof(Theme, scrim)            },
   { "rowHighlight",     offsetof(Theme, rowHighlight)     },
   { "seekTrack",        offsetof(Theme, seekTrack)        },
   { "seekNotch",        offsetof(Theme, seekNotch)        },
   { "watchedThumbTint", offsetof(Theme, watchedThumbTint) },
};
#define COLOR_FIELD_COUNT ((int)(sizeof COLOR_FIELDS / sizeof COLOR_FIELDS[0]))

static const char *THEMES_FILE_HEADER =
   "# yo-player themes - edit over FTP; changes apply on the next launch.\n"
   "#\n"
   "# Each [Name] block is one theme, and its name is what settings.txt selects with\n"
   "# \"theme=<name>\" (lower case, spaces become hyphens). Colours are #RRGGBB, or #RRGGBBAA\n"
   "# when you want transparency (AA comes last: 00 = clear, FF = solid). Every block inherits\n"
   "# YouTube below, so a new one only has to list the colours it changes.\n\n";

static Theme         themes[MAX_THEMES];
static ThemeRegistry registry;

const Theme *activeTheme = &themeYouTube;

// selects the theme settings.txt names; an absent or unknown name leaves the built-in in place.
static void applySettingsTheme(void)
{
   char text[SETTINGS_BUFFER];
   int length = readFile(SETTINGS_PATH, text, sizeof text - 1);
   if (length <= 0) return;
   text[length] = '\0';

   const char *slug = findSettingValue(text, "theme");
   int themeIndex = slug ? findThemeBySlug(&registry, slug) : -1;
   if (themeIndex >= 0) activeTheme = (const Theme *)getRegisteredTheme(&registry, themeIndex);
}

void initTheme(void)
{
   makeDir(TMP_DIR);

   initThemeRegistry(&registry, themes, sizeof(Theme), &themeYouTube, "YouTube", COLOR_FIELDS, COLOR_FIELD_COUNT);
   activeTheme = (const Theme *)getRegisteredTheme(&registry, 0);   // the guaranteed fallback

   // first launch: write the built-in out in full, so every key is there to edit. never overwritten
   // after, so edits survive; delete the file to get the built-in back.
   if (!fileExists(THEMES_PATH)) {
      int rc = writeThemeTemplate(&registry, THEMES_PATH, THEMES_FILE_HEADER);
      if (rc == 0) logInfo("[theme] created %s\n", THEMES_PATH);
      else         logError("[theme] couldn't create %s\n", THEMES_PATH);
   }

   loadThemeFile(&registry, THEMES_PATH);
   applySettingsTheme();
}
