// theme-registry - the shared themes.txt reader (see theme-registry.h).

#include "theme-registry.h"
#include "colors.h"             // parseColor
#include "vfs.h"                // readFile / writeFile
#include "string-utilities.h"   // strCopy / appendStr

#define FILE_BUFFER 8192   // one themes.txt, comments and all

static char fileBuffer[FILE_BUFFER];   // scratch, reused across every file this reads or writes

// a display name folded to its slug form, one character at a time: upper->lower, space/'_'->'-'.
static char toSlugChar(char c)
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
      if (toSlugChar(name[i]) != toSlugChar(slugCh)) return 0;
   }
   char slugCh = slug[i];
   return slugCh == '\0' || slugCh == '\n' || slugCh == '\r' || slugCh == ' ';
}

// a display name folded whole ("Original Blue" -> "original-blue"), which is the form both a settings
// value and a [Name] header have to be in before they can be compared or matched.
static void toSlug(const char *name, char *out, int cap)
{
   int i = 0;
   for (; name[i] && i < cap - 1; i++) out[i] = toSlugChar(name[i]);
   out[i] = '\0';
}

static void *getThemeAt(const ThemeRegistry *registry, int index)
{
   return (char *)registry->themes + (size_t)index * registry->themeSize;
}

static void copyTheme(void *destination, const void *source, int size)
{
   for (int i = 0; i < size; i++) ((char *)destination)[i] = ((const char *)source)[i];
}

// text[0..length) equals the NUL-terminated key exactly (for colour-field lookup).
static int keyMatches(const char *text, int length, const char *key)
{
   for (int i = 0; i < length; i++)
      if (key[i] == '\0' || text[i] != key[i]) return 0;
   return key[length] == '\0';
}

// apply one "key=value" pair to the theme under construction.
static void applyField(const ThemeRegistry *registry, void *theme, const char *key, int keyLength, const char *value)
{
   for (int i = 0; i < registry->fieldCount; i++) {
      if (keyMatches(key, keyLength, registry->fields[i].key)) {
         uint32_t *field = (uint32_t *)((char *)theme + registry->fields[i].offset);
         *field = parseColor(value, *field);   // keep the seeded value if the colour is malformed
         return;
      }
   }
}

// starts a [Name] block: overrides an existing theme of the same name, else appends one.
// returns the theme being filled, or NULL when the registry is full.
static void *beginThemeBlock(ThemeRegistry *registry, const char *name)
{
   char slug[THEME_NAME_MAX];
   toSlug(name, slug, sizeof slug);   // a name is only comparable in slug form: "Original Blue" has a space in it
   int index = findThemeBySlug(registry, slug);
   if (index < 0) {
      if (registry->count >= MAX_THEMES) return NULL;
      index = registry->count++;
      strCopy(registry->names[index], THEME_NAME_MAX, name);
   }
   copyTheme(getThemeAt(registry, index), registry->builtIn, registry->themeSize);   // unspecified colours fall back
   return getThemeAt(registry, index);
}

// parse a themes file (mutated in place: line ends become NULs), registering each [Name] block.
static void parseThemes(ThemeRegistry *registry, char *text)
{
   void *current = NULL;
   char *cursor = text;
   while (*cursor) {
      // cut the next line out of the buffer
      char *line = cursor;
      while (*cursor && *cursor != '\n' && *cursor != '\r') cursor++;
      char *lineEnd = cursor;
      while (*cursor == '\n' || *cursor == '\r') cursor++;
      *lineEnd = '\0';

      while (*line == ' ' || *line == '\t') line++;
      if (*line == '#' || *line == '\0') continue;

      // a [Name] header starts a new theme
      if (*line == '[') {
         char *name = line + 1;
         char *close = name;
         while (*close && *close != ']') close++;
         *close = '\0';
         current = beginThemeBlock(registry, name);
         continue;
      }

      // otherwise it's a "key = value" colour line for the block we're in
      if (!current) continue;
      char *equals = line;
      while (*equals && *equals != '=') equals++;
      if (*equals != '=') continue;
      int keyLength = (int)(equals - line);
      while (keyLength > 0 && (line[keyLength - 1] == ' ' || line[keyLength - 1] == '\t')) keyLength--;
      char *value = equals + 1;
      while (*value == ' ' || *value == '\t') value++;
      applyField(registry, current, line, keyLength, value);
   }
}

// "#RRGGBBAA" out of an 0xAARRGGBB colour - the form themes.txt is written and read in.
static void appendHexColor(char *out, int cap, int *offset, uint32_t color)
{
   static const char DIGITS[] = "0123456789ABCDEF";
   static const int  SHIFTS[8] = { 20, 16, 12, 8, 4, 0, 28, 24 };   // red, green, blue, then alpha last
   char text[10];
   text[0] = '#';
   for (int i = 0; i < 8; i++) text[i + 1] = DIGITS[(color >> SHIFTS[i]) & 0xF];
   text[9] = '\0';
   appendStr(out, cap, offset, text);
}

void initThemeRegistry(ThemeRegistry *registry, void *themes, int themeSize, const void *builtIn,
                       const char *builtInName, const ThemeColorField *fields, int fieldCount)
{
   registry->themes     = themes;
   registry->themeSize  = themeSize;
   registry->builtIn    = builtIn;
   registry->fields     = fields;
   registry->fieldCount = fieldCount;
   registry->count      = 1;
   strCopy(registry->names[0], THEME_NAME_MAX, builtInName);
   copyTheme(getThemeAt(registry, 0), builtIn, themeSize);
}

void loadThemeFile(ThemeRegistry *registry, const char *path)
{
   int length = readFile(path, fileBuffer, sizeof fileBuffer - 1);
   if (length <= 0) return;
   fileBuffer[length] = '\0';
   parseThemes(registry, fileBuffer);
}

int writeThemeTemplate(const ThemeRegistry *registry, const char *path, const char *headerComment)
{
   int length = 0;
   if (headerComment) appendStr(fileBuffer, sizeof fileBuffer, &length, headerComment);
   appendStr(fileBuffer, sizeof fileBuffer, &length, "[");
   appendStr(fileBuffer, sizeof fileBuffer, &length, registry->names[0]);
   appendStr(fileBuffer, sizeof fileBuffer, &length, "]\n");
   for (int i = 0; i < registry->fieldCount; i++) {
      appendStr(fileBuffer, sizeof fileBuffer, &length, registry->fields[i].key);
      appendStr(fileBuffer, sizeof fileBuffer, &length, " = ");
      appendHexColor(fileBuffer, sizeof fileBuffer, &length,
                     *(const uint32_t *)((const char *)registry->builtIn + registry->fields[i].offset));
      appendStr(fileBuffer, sizeof fileBuffer, &length, "\n");
   }
   return writeFile(path, fileBuffer, (uint64_t)length) == 0 ? 0 : -1;
}

int findThemeBySlug(const ThemeRegistry *registry, const char *slug)
{
   for (int i = 0; i < registry->count; i++)
      if (nameMatchesSlug(registry->names[i], slug)) return i;
   return -1;
}

void getThemeSlug(const ThemeRegistry *registry, int index, char *out, int cap)
{
   toSlug((index >= 0 && index < registry->count) ? registry->names[index] : "", out, cap);
}

void *getRegisteredTheme(const ThemeRegistry *registry, int index)
{
   return (index >= 0 && index < registry->count) ? getThemeAt(registry, index) : 0;
}
