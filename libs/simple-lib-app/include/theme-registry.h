#pragma once

// theme-registry - the shared machinery behind an app's colour themes: it reads the app's themes.txt
// files and turns each [Name] block into one of the app's own theme structs.
//
// The app owns the struct (its own colour fields), an array of them, and a table saying which "key="
// in the file writes which field. Everything else - the file format, the name/slug matching, the
// override-or-add merge and the "write the built-in out as a template" seed - lives here, so the app's
// theme.c is just its palette plus its file paths. See file-manager/src/theme.c for a full example.
//
// File format: '#' comments, blank lines ignored, a "[Name]" line starts a theme and the "key = value"
// lines under it set its colours (#RRGGBB, or #RRGGBBAA with the alpha last). Every block starts as a
// copy of the built-in, so it only has to list what it changes. A block whose name matches one already
// registered overrides it in place; any other block is added. That is what lets an app read a shipped
// file and the user's file in turn, and have the user's blocks win.

#include <stdint.h>
#include <stddef.h>   // offsetof, size_t

#define THEME_NAME_MAX 28
#define MAX_THEMES     16

// one uint32_t colour field of the app's theme struct, by the name it goes under in themes.txt.
// build the table with offsetof: { "appBg", offsetof(Theme, appBg) }, ...
typedef struct { const char *key; size_t offset; } ThemeColorField;

typedef struct {
   void       *themes;      // the app's array of theme structs: MAX_THEMES entries of themeSize bytes
   int         themeSize;
   const void *builtIn;     // the seed every block inherits (and the template writer's source)
   const ThemeColorField *fields;
   int         fieldCount;
   char        names[MAX_THEMES][THEME_NAME_MAX];
   int         count;
} ThemeRegistry;

// registers the built-in as theme 0 under builtInName - the guaranteed fallback with no theme files.
void initThemeRegistry(ThemeRegistry *registry, void *themes, int themeSize, const void *builtIn,
                       const char *builtInName, const ThemeColorField *fields, int fieldCount);

// merges one themes.txt into the registry (override-or-add by name). a missing file is a no-op, so an
// app can read a shipped file and the user's file in turn without checking either exists.
void loadThemeFile(ThemeRegistry *registry, const char *path);

// writes the built-in out as a full [Name] block with every key listed, under the given header comment
// (pass NULL for none), so a user's file starts out showing everything they can edit. returns 0 on
// success, -1 on a write failure.
int writeThemeTemplate(const ThemeRegistry *registry, const char *path, const char *headerComment);

int  findThemeBySlug(const ThemeRegistry *registry, const char *slug);   // -1 when unknown
void getThemeSlug(const ThemeRegistry *registry, int index, char *out, int cap);   // the name, slug-folded

// the theme at index, for the app to point its activeTheme at (NULL when index is out of range).
void *getRegisteredTheme(const ThemeRegistry *registry, int index);
