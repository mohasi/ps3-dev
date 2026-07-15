// theme.c - the theme registry and its on-disk files (see theme.h).
//
// themes come from two files, both read every launch and merged (a later block overrides an earlier
// one of the same slug):
//   - res/themes.txt          the shipped built-ins (Original Blue / Light / Dark): the base layer.
//   - <tmp>/themes.txt        the user's file, seeded on first launch with a copy of the built-ins
//                             and never overwritten after. editing a block here overrides that
//                             built-in; deleting one falls back to its shipped res form; new [Name]
//                             blocks add the user's own themes.
// settings.txt (same tmp folder) holds one line, "theme=<slug>", naming which theme to start in;
// L1/R1 in-app rewrites it so the choice survives a relaunch.
//
// a [Name] block whose slug matches an already-registered theme overrides it in place; any other
// block is added. every block is seeded from the built-in Original Blue, so it only has to list the
// colours it wants to change. an unknown/malformed slug in settings.txt falls back to Original Blue.
#include "theme.h"
#include "vfs.h"                // readFile / writeFile / fileExists / makeDir
#include "colors.h"             // parseColor
#include "settings-file.h"      // loadSettingsFile / findSettingValue
#include "string-utilities.h"   // strCopy / getStrLen
#include <stddef.h>             // offsetof

#define RES_THEMES     "/dev_hdd0/game/FILEMGR01/USRDIR/themes.txt"
#define TMP_DIR        "/dev_hdd0/tmp/file-manager"
#define TMP_THEMES     TMP_DIR "/themes.txt"
#define TMP_SETTINGS   TMP_DIR "/settings.txt"

#define THEME_NAME_MAX 28
#define MAX_THEMES     16
#define THEMES_BUF     8192
#define SETTINGS_BUF   1024

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
   .checkBorder     = 0xFF4A7CC0,
   .checkFill       = 0xFFFFFFFF,   // white tick stays distinct from the blue row highlight
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

// registry (mutable so activeTheme can point straight into it and a switch is a repoint).
static Theme themes[MAX_THEMES];
static char  names[MAX_THEMES][THEME_NAME_MAX];
static int   themeCount;
static int   activeIndex;

const Theme *activeTheme = &themeOriginalBlue;

// every colour field, by name, so a themes.txt "key=value" line maps straight onto the struct.
// borderThickness is not user-tunable - a block inherits Original Blue's value via the seed copy.
static const struct { const char *key; size_t offset; } COLOR_FIELDS[] = {
   { "appBg",           offsetof(Theme, appBg)           },
   { "divider",         offsetof(Theme, divider)         },
   { "panelFill",       offsetof(Theme, panelFill)       },
   { "panelBorder",     offsetof(Theme, panelBorder)     },
   { "menuFill",        offsetof(Theme, menuFill)        },
   { "menuBorder",      offsetof(Theme, menuBorder)      },
   { "highlightFill",   offsetof(Theme, highlightFill)   },
   { "highlightBorder", offsetof(Theme, highlightBorder) },
   { "separator",       offsetof(Theme, separator)       },
   { "checkBorder",     offsetof(Theme, checkBorder)     },
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

// a display name folded to its slug form, one character at a time: upper->lower, space/'_'->'-'.
static char slugChar(char c)
{
   if (c >= 'A' && c <= 'Z') return (char)(c + 32);
   if (c == ' ' || c == '_') return '-';
   return c;
}

// does the display name equal the slug (e.g. "Original Blue" == "original-blue")? both sides are
// compared in slug form; the slug ends at NUL or any run-terminator a settings value can carry.
static int nameMatchesSlug(const char *name, const char *slug)
{
   int i = 0;
   for (; name[i]; i++) {
      char slugCh = slug[i];
      if (slugCh == '\0' || slugCh == '\n' || slugCh == '\r' || slugCh == ' ') return 0;
      if (slugChar(name[i]) != slugChar(slugCh)) return 0;
   }
   char slugCh = slug[i];
   return slugCh == '\0' || slugCh == '\n' || slugCh == '\r' || slugCh == ' ';
}

// index of the theme whose name slugifies to slug, or -1.
static int findThemeBySlug(const char *slug)
{
   for (int i = 0; i < themeCount; i++)
      if (nameMatchesSlug(names[i], slug)) return i;
   return -1;
}

// writes name's slug form into out (NUL-terminated, clamped to cap).
static void slugifyName(const char *name, char *out, int cap)
{
   int i = 0;
   for (; name[i] && i < cap - 1; i++) out[i] = slugChar(name[i]);
   out[i] = '\0';
}

// text[0..length) equals the NUL-terminated key exactly (for colour-field lookup).
static int keyMatches(const char *text, int length, const char *key)
{
   for (int i = 0; i < length; i++)
      if (key[i] == '\0' || text[i] != key[i]) return 0;
   return key[length] == '\0';
}

// apply one "key=value" pair to the theme under construction.
static void applyField(Theme *theme, const char *key, int keyLen, const char *value)
{
   for (int i = 0; i < COLOR_FIELD_COUNT; i++) {
      if (keyMatches(key, keyLen, COLOR_FIELDS[i].key)) {
         uint32_t *field = (uint32_t *)((char *)theme + COLOR_FIELDS[i].offset);
         *field = parseColor(value, *field);   // keep the seeded value if the colour is malformed
         return;
      }
   }
}

// registers a [Name] block: overrides an existing theme of the same slug, else appends one.
// returns the theme being filled, or NULL when the registry is full.
static Theme *beginThemeBlock(const char *name)
{
   char slug[THEME_NAME_MAX];
   slugifyName(name, slug, sizeof slug);

   int themeIndex = findThemeBySlug(slug);
   if (themeIndex < 0) {
      if (themeCount >= MAX_THEMES) return NULL;
      themeIndex = themeCount++;
      strCopy(names[themeIndex], THEME_NAME_MAX, name);
   }
   themes[themeIndex] = themeOriginalBlue;   // seed: unspecified colours fall back to the original
   return &themes[themeIndex];
}

// parse themes.txt (mutated in place: line ends become NULs), registering each [Name] block.
static void parseThemes(char *text)
{
   Theme *current = NULL;
   char *p = text;
   while (*p) {
      char *line = p;
      while (*p && *p != '\n' && *p != '\r') p++;
      char *lineEnd = p;
      while (*p == '\n' || *p == '\r') p++;   // step onto the next line
      *lineEnd = '\0';

      while (*line == ' ' || *line == '\t') line++;
      if (*line == '#' || *line == '\0') continue;

      if (*line == '[') {                     // new theme section
         char *name = line + 1;
         char *close = name;
         while (*close && *close != ']') close++;
         *close = '\0';
         current = beginThemeBlock(name);
         continue;
      }

      if (!current) continue;                 // a key line before any [section]
      char *eq = line;
      while (*eq && *eq != '=') eq++;
      if (*eq != '=') continue;
      int keyLen = (int)(eq - line);
      while (keyLen > 0 && (line[keyLen - 1] == ' ' || line[keyLen - 1] == '\t')) keyLen--;  // trim key
      char *value = eq + 1;
      while (*value == ' ' || *value == '\t') value++;                                        // trim value
      applyField(current, line, keyLen, value);
   }
}

static char fileBuffer[THEMES_BUF];   // scratch, reused across the two theme files

// reads a themes file and merges its [Name] blocks into the registry (override-or-add by slug).
static void loadThemeFile(const char *path)
{
   int length = readFile(path, fileBuffer, sizeof fileBuffer - 1);
   if (length <= 0) return;
   fileBuffer[length] = '\0';
   parseThemes(fileBuffer);
}

// first launch: seed the user file with a copy of the shipped built-ins, so they are right there to
// tweak or delete. never overwritten after; res stays the base layer, so deleting a block here just
// falls back to its shipped form.
static void seedUserThemesFile(void)
{
   if (fileExists(TMP_THEMES)) return;
   int length = readFile(RES_THEMES, fileBuffer, sizeof fileBuffer - 1);
   if (length > 0) writeFile(TMP_THEMES, fileBuffer, (uint64_t)length);
}

// builds settings.txt content for the given theme slug, listing the available slugs inline so the
// file documents its own valid values.
static void buildSettings(const char *slug, char *out, int cap)
{
   int n = 0;
   appendStr(out, cap, &n,
      "# file-manager settings - edit over FTP; changes apply on the next launch\n"
      "# (in-app: press L1 / R1 to switch theme, which also saves your choice here)\n"
      "\ntheme=");
   appendStr(out, cap, &n, slug);
   appendStr(out, cap, &n, "\n# available: ");
   for (int i = 0; i < themeCount; i++) {
      if (i > 0) appendStr(out, cap, &n, ", ");
      char themeSlug[THEME_NAME_MAX];
      slugifyName(names[i], themeSlug, sizeof themeSlug);
      appendStr(out, cap, &n, themeSlug);
   }
   appendStr(out, cap, &n, "\n");
   out[n] = '\0';
}

// rewrites settings.txt so the current theme choice is what the next launch reads.
static void persistThemeSetting(void)
{
   char slug[THEME_NAME_MAX];
   slugifyName(names[activeIndex], slug, sizeof slug);
   char text[SETTINGS_BUF];
   buildSettings(slug, text, sizeof text);
   writeFile(TMP_SETTINGS, text, (uint64_t)getStrLen(text));
}

// reads settings.txt (creating it with a default on first launch) and selects the theme it names.
static void applySettingsTheme(void)
{
   char defaults[SETTINGS_BUF];
   buildSettings("original-blue", defaults, sizeof defaults);

   char text[SETTINGS_BUF];
   loadSettingsFile(TMP_SETTINGS, defaults, text, sizeof text);

   const char *slug = findSettingValue(text, "theme");
   int themeIndex = slug ? findThemeBySlug(slug) : -1;
   activeIndex = themeIndex >= 0 ? themeIndex : 0;   // unknown/absent slug -> Original Blue
   activeTheme = &themes[activeIndex];
}

void initThemes(void)
{
   makeDir(TMP_DIR);

   // Original Blue is always registry slot 0 - the guaranteed fallback even with no theme files.
   themes[0] = themeOriginalBlue;
   strCopy(names[0], THEME_NAME_MAX, "Original Blue");
   themeCount = 1;

   loadThemeFile(RES_THEMES);   // shipped built-ins: the base layer (always present)
   seedUserThemesFile();        // first launch: drop editable copies into the user file
   loadThemeFile(TMP_THEMES);   // user layer: overrides a built-in by name, deletes fall back to res, adds new

   applySettingsTheme();
}

int         getThemeCount(void)       { return themeCount; }
const char *getThemeName(int index)   { return (index >= 0 && index < themeCount) ? names[index] : ""; }
int         getActiveThemeIndex(void) { return activeIndex; }

void setActiveThemeIndex(int index)
{
   if (index < 0) index = 0;
   if (index >= themeCount) index = themeCount - 1;
   activeIndex = index;
   activeTheme = &themes[index];
   persistThemeSetting();
}
