// theme.c - the file manager's palette and its theme files (see theme.h). The file format and the
// parsing live in simple-lib-app's theme-registry; this file is the built-in palette, the field table
// that maps its colours onto "key=" lines, the file paths, and the R1 selection.
//
// Two files are read every launch and merged (a later block overrides an earlier one of the same name):
//   - res/themes.txt          the shipped built-ins (Original Blue / Light / Dark): the base layer.
//   - <tmp>/themes.txt        the user's file, seeded on first launch with a copy of the built-ins
//                             and never overwritten after. editing a block here overrides that
//                             built-in; deleting one falls back to its shipped form; new [Name]
//                             blocks add the user's own themes.
// settings.txt (same tmp folder) holds one line, "theme=<slug>", naming which theme to start in;
// R1 in-app rewrites it so the choice survives a relaunch. an unknown/malformed slug falls back to
// Original Blue.
#include "theme.h"
#include "theme-registry.h"
#include "vfs.h"                // readFile / writeFile / fileExists / makeDir
#include "settings-file.h"      // loadSettingsFile / findSettingValue / upsertSettingValue
#include "string-utilities.h"   // appendStr

#define RES_THEMES     "/dev_hdd0/game/FILEMGR01/USRDIR/themes.txt"
#define TMP_DIR        "/dev_hdd0/tmp/file-manager"
#define TMP_THEMES     TMP_DIR "/themes.txt"
#define TMP_SETTINGS   TMP_DIR "/settings.txt"

#define THEMES_BUFFER  8192
#define SETTINGS_BUFFER 1024

// the built-in Original Blue: the seed every themes.txt block inherits, and the guaranteed fallback
// when themes.txt is missing or the settings slug names nothing.
static const Theme themeOriginalBlue = {
   .appBg           = 0xFF001636,
   .divider         = 0xFF232D43,
   .panelFill       = 0xFF04182F,
   .panelBorder     = 0xFF2E4E78,
   .menuFill        = 0xFF01142B,
   .menuBorder      = 0xFF4A566F,
   .highlightFill   = 0xFF12386A,
   .highlightBorder = 0xFF4A7CC0,
   .separator       = 0xFF203350,
   .checkFill       = 0xFFFFFFFF,   // white checkbox stays distinct from the blue row highlight
   .textPrimary     = 0xFFFFFFFF,
   .textSecondary   = 0x80FFFFFF,
   .textOnHighlight = 0xCCFFFFFF,
   .scrim           = 0xC8000000,
   .dialogFill      = 0xFF04182F,
   .dialogBorder    = 0xFF2E4E78,
   .progressTrack   = 0xFF0A2547,
   .progressFill    = 0xFF2F6FD0,
   .scrollTrack     = 0xFF0A2547,
   .scrollThumb     = 0xFF3E6DA8,
   .borderThickness = 2,
};

// every colour field, by name, so a themes.txt "key=value" line maps straight onto the struct.
// borderThickness is not user-tunable - a block inherits Original Blue's value via the seed copy.
static const ThemeColorField COLOR_FIELDS[] = {
   { "appBg",           offsetof(Theme, appBg)           },
   { "divider",         offsetof(Theme, divider)         },
   { "panelFill",       offsetof(Theme, panelFill)       },
   { "panelBorder",     offsetof(Theme, panelBorder)     },
   { "menuFill",        offsetof(Theme, menuFill)        },
   { "menuBorder",      offsetof(Theme, menuBorder)      },
   { "highlightFill",   offsetof(Theme, highlightFill)   },
   { "highlightBorder", offsetof(Theme, highlightBorder) },
   { "separator",       offsetof(Theme, separator)       },
   { "checkFill",       offsetof(Theme, checkFill)       },
   { "textPrimary",     offsetof(Theme, textPrimary)     },
   { "textSecondary",   offsetof(Theme, textSecondary)   },
   { "textOnHighlight", offsetof(Theme, textOnHighlight) },
   { "scrim",           offsetof(Theme, scrim)           },
   { "dialogFill",      offsetof(Theme, dialogFill)      },
   { "dialogBorder",    offsetof(Theme, dialogBorder)    },
   { "progressTrack",   offsetof(Theme, progressTrack)   },
   { "progressFill",    offsetof(Theme, progressFill)    },
   { "scrollTrack",     offsetof(Theme, scrollTrack)     },
   { "scrollThumb",     offsetof(Theme, scrollThumb)     },
};
#define COLOR_FIELD_COUNT ((int)(sizeof COLOR_FIELDS / sizeof COLOR_FIELDS[0]))

static Theme         themes[MAX_THEMES];
static ThemeRegistry registry;
static int           activeIndex;

const Theme *activeTheme = &themeOriginalBlue;

// first launch: seed the user file with a copy of the shipped built-ins, so they are right there to
// tweak or delete. never overwritten after; res stays the base layer, so deleting a block here just
// falls back to its shipped form.
static void seedUserThemesFile(void)
{
   if (fileExists(TMP_THEMES)) return;
   static char shipped[THEMES_BUFFER];
   int length = readFile(RES_THEMES, shipped, sizeof shipped);
   if (length > 0) writeFile(TMP_THEMES, shipped, (uint64_t)length);
}

// builds settings.txt content for the given theme slug, listing the available slugs inline so the
// file documents its own valid values.
static void buildSettings(const char *slug, char *out, int cap)
{
   int length = 0;
   appendStr(out, cap, &length,
      "# file-manager settings - edit over FTP; changes apply on the next launch\n"
      "# (in-app: press R1 to switch theme, which also saves your choice here)\n"
      "\ntheme=");
   appendStr(out, cap, &length, slug);
   appendStr(out, cap, &length, "\n# available: ");
   for (int i = 0; i < registry.count; i++) {
      if (i > 0) appendStr(out, cap, &length, ", ");
      char themeSlug[THEME_NAME_MAX];
      getThemeSlug(&registry, i, themeSlug, sizeof themeSlug);
      appendStr(out, cap, &length, themeSlug);
   }
   appendStr(out, cap, &length, "\n");
   out[length] = '\0';
}

// saves the current theme choice, preserving any other keys (e.g. Google Drive) in settings.txt.
static void persistThemeSetting(void)
{
   char slug[THEME_NAME_MAX];
   getThemeSlug(&registry, activeIndex, slug, sizeof slug);
   upsertSettingValue(TMP_SETTINGS, "theme", slug);
}

const char *getSettingsPath(void) { return TMP_SETTINGS; }

// points activeTheme at the theme at index (clamped). the saved choice is a separate step, so the
// startup selection doesn't rewrite the file it just read.
static void selectTheme(int index)
{
   if (index < 0) index = 0;
   if (index >= registry.count) index = registry.count - 1;
   activeIndex = index;
   activeTheme = (const Theme *)getRegisteredTheme(&registry, index);
}

// reads settings.txt (creating it with a default on first launch) and selects the theme it names.
static void applySettingsTheme(void)
{
   char defaults[SETTINGS_BUFFER];
   buildSettings("original-blue", defaults, sizeof defaults);

   char text[SETTINGS_BUFFER];
   loadSettingsFile(TMP_SETTINGS, defaults, text, sizeof text);

   const char *slug = findSettingValue(text, "theme");
   int themeIndex = slug ? findThemeBySlug(&registry, slug) : -1;
   selectTheme(themeIndex >= 0 ? themeIndex : 0);   // unknown/absent slug -> Original Blue
}

void initThemes(void)
{
   makeDir(TMP_DIR);

   // Original Blue is always registry slot 0 - the guaranteed fallback even with no theme files.
   initThemeRegistry(&registry, themes, sizeof(Theme), &themeOriginalBlue, "Original Blue", COLOR_FIELDS, COLOR_FIELD_COUNT);

   loadThemeFile(&registry, RES_THEMES);   // shipped built-ins: the base layer (always present)
   seedUserThemesFile();                   // first launch: drop editable copies into the user file
   loadThemeFile(&registry, TMP_THEMES);   // user layer: overrides a built-in by name, adds new ones

   applySettingsTheme();
}

int getThemeCount(void)       { return registry.count; }
int getActiveThemeIndex(void) { return activeIndex; }

void setActiveThemeIndex(int index)
{
   selectTheme(index);
   persistThemeSetting();
}
