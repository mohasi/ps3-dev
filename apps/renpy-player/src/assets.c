#include "assets.h"

#include <stdlib.h>
#include <string.h>
#include <sys/sys_time.h>   // sys_time_get_system_time (ATL start timestamp)

#include "printf.h"
#include "dbg.h"
#include "config.h"
#include "gamepath.h"   // getGameRpkPath()
#include "gui.h"        // gui.dlgFont (converter-resolved dialogue font)
#include "rpk.h"
#include "colors.h"   // parseColor (for Solid-colour scenes like `black`)

// ---- image-name -> displayable map (from IMAGE ops) ----
// An image def is either a FILE (`image bg = "alley.jpg"`) or a SOLID colour
// (`image white = Solid("fff")`). Solids are resolved to an ARGB colour via the general colour
// parser (parseColor handles "#rgb"/"#rrggbb"/... with or without '#'), NOT a per-name special case.

#define IMG_MAX 2048
static char     imgName[IMG_MAX][64];
static char     imgFile[IMG_MAX][64];    // file images: the asset path (empty for solids)
static int      imgIsSolid[IMG_MAX];     // 1 = this image is a Solid colour, not a file
static uint32_t imgSolidColor[IMG_MAX];  // ARGB for solids
static int      imgCount;

static GfxTexture bgTex;
static int        bgLoaded;
static char       curBgFile[64];

// A scene can also be a Solid colour (`scene black`) rather than an image. Ren'Py defines
// `black`/`white` as Solid built-ins; we also accept a literal hex name. When set, the
// scene fills with bgSolidColor instead of a texture.
static int        bgIsSolid;
static uint32_t   bgSolidColor;

static Sprite sprites[SPR_MAX];
static int    sprCount;

// Copies the first quoted substring of src into out (the image file in an IMAGE def).
static void firstQuoted(const char *src, char *out, int cap)
{
   out[0] = '\0';
   const char *p = strchr(src, '"');
   char q = '"';
   const char *p2 = strchr(src, '\'');
   if (!p || (p2 && p2 < p)) { p = p2; q = '\''; }
   if (!p) return;
   p++;
   int j = 0;
   while (*p && *p != q && j < cap - 1) out[j++] = *p++;
   out[j] = '\0';
}

static const char *baseName(const char *path)
{
   const char *s = strrchr(path, '/');
   if (s) return s + 1;
   s = strrchr(path, '\\');
   return s ? s + 1 : path;
}

// First word of an image name ("eileen happy" -> "eileen"); the sprite's identity tag.
static void firstWord(const char *name, char *out, int cap)
{
   int j = 0;
   while (name[j] && name[j] != ' ' && j < cap - 1) { out[j] = name[j]; j++; }
   out[j] = '\0';
}

void initAssets(const RbcProgram *p)
{
   imgCount = 0;
   bgLoaded = 0;
   bgIsSolid = 0;
   curBgFile[0] = '\0';
   sprCount = 0;
   for (int i = 0; i < p->instrCount && imgCount < IMG_MAX; i++)
   {
      if (p->code[i].op != RBC_IMAGE) continue;
      const char *name = getRbcStr(p, p->code[i].a);
      const char *code = getRbcStr(p, p->code[i].b);
      if (!name[0]) continue;
      char token[64];
      firstQuoted(code, token, sizeof token);   // the quoted file path, or a Solid's colour string

      const char *c = code;
      while (*c == ' ' || *c == '\t') c++;
      int isSolid = (strncmp(c, "Solid", 5) == 0);   // `image x = Solid("fff")` -> colour, not a file
      if (!isSolid && !token[0]) continue;           // a non-Solid displayable with no file -> skip

      strncpy(imgName[imgCount], name, 63); imgName[imgCount][63] = '\0';
      imgIsSolid[imgCount] = isSolid;
      if (isSolid)
      {
         imgSolidColor[imgCount] = parseColor(token, 0xFF000000u);   // "fff"/"#000"/... via general parser
         imgFile[imgCount][0] = '\0';
      }
      else
      {
         strncpy(imgFile[imgCount], token, 63); imgFile[imgCount][63] = '\0';
      }
      imgCount++;
   }
   logInfo("[rpp] image map: %d entries\n", imgCount);
}

void freeAssets(void)
{
   clearSprites();
   if (bgLoaded) { freeGfxTexture(&bgTex); bgLoaded = 0; }
   bgIsSolid = 0;
   imgCount = 0;
}

static const char *resolveImageFile(const char *name)
{
   for (int i = 0; i < imgCount; i++)
      if (!imgIsSolid[i] && strcmp(imgName[i], name) == 0) return imgFile[i];
   return (const char *)0;
}

// If `name` is an image defined as a Solid colour, returns 1 and its ARGB.
static int resolveImageSolid(const char *name, uint32_t *out)
{
   for (int i = 0; i < imgCount; i++)
      if (imgIsSolid[i] && strcmp(imgName[i], name) == 0) { *out = imgSolidColor[i]; return 1; }
   return 0;
}

int loadAssetTexture(const char *base, GfxTexture *out)
{
   RpkFile r;
   if (openRpk(&r, getGameRpkPath()) != 0) return 0;
   char suffix[80];
   snprintf(suffix, sizeof suffix, "/%s", base);
   char name[256];
   unsigned char *buf = NULL;
   long len = 0;
   int rc = readRpkEntrySuffix(&r, suffix, 0, name, sizeof name, &buf, &len);
   closeRpk(&r);
   if (rc != 0 || !buf) { logWarn("[rpp] img: %s not in bundle\n", base); return 0; }

   GfxTexture t = loadGfxTextureMem(buf, (uint32_t)len);   // decode from memory (PNG/JPEG)
   free(buf);
   if (t.offset == 0) { logWarn("[rpp] img: decode failed %s\n", name); return 0; }
   *out = t;
   logInfo("[rpp] img: %s\n", name);
   return 1;
}

// Load (and cache) a background by its file name into bgTex.
static void loadBg(const char *file)
{
   const char *base = baseName(file);
   if (bgLoaded && !bgIsSolid && strcmp(base, curBgFile) == 0) return;   // already showing it

   GfxTexture t;
   if (!loadAssetTexture(base, &t)) return;
   if (bgLoaded) freeGfxTexture(&bgTex);
   bgTex = t;
   bgLoaded = 1;
   bgIsSolid = 0;
   strncpy(curBgFile, base, sizeof curBgFile - 1); curBgFile[sizeof curBgFile - 1] = '\0';
}

// A scene name with no image def may be a Solid colour: the engine built-ins `black`/
// `white`, or a literal hex like `#000`. Returns 1 and the ARGB colour if recognised.
static int solidColorFor(const char *name, uint32_t *out)
{
   if (strcmp(name, "black") == 0) { *out = 0xFF000000u; return 1; }
   if (strcmp(name, "white") == 0) { *out = 0xFFFFFFFFu; return 1; }
   if (name[0] == '#')             { *out = parseColor(name, 0xFF000000u); return 1; }
   return 0;
}

// Switches the background to a Solid colour fill (releasing any image texture).
static void setSolidBg(uint32_t color)
{
   if (bgLoaded) freeGfxTexture(&bgTex);
   bgLoaded = 0;
   bgIsSolid = 1;
   bgSolidColor = color;
   curBgFile[0] = '\0';
}

void resolveScene(const RbcProgram *p, const char *sceneName)
{
   (void)p;
   if (!sceneName || !sceneName[0]) return;

   uint32_t color;
   if (resolveImageSolid(sceneName, &color)) { setSolidBg(color); return; }   // image x = Solid(c)

   const char *file = resolveImageFile(sceneName);
   if (file) { loadBg(file); return; }

   if (solidColorFor(sceneName, &color)) { setSolidBg(color); return; }       // bare `scene white`/`scene #hex`

   logWarn("[rpp] scene '%s' has no image def\n", sceneName);
}

int getBg(GfxTexture *out)
{
   if (!bgLoaded) return 0;
   *out = bgTex;
   return 1;
}

int getBgSolid(uint32_t *colorOut)
{
   if (!bgIsSolid) return 0;
   *colorOut = bgSolidColor;
   return 1;
}

// ---- sprites ----

void clearSprites(void)
{
   for (int i = 0; i < sprCount; i++) freeGfxTexture(&sprites[i].tex);
   sprCount = 0;
}

void hideSprite(const char *name)
{
   char tag[32];
   firstWord(name, tag, sizeof tag);
   for (int i = 0; i < sprCount; i++)
      if (strcmp(sprites[i].tag, tag) == 0)
      {
         freeGfxTexture(&sprites[i].tex);
         sprites[i] = sprites[sprCount - 1];   // swap-remove
         sprCount--;
         return;
      }
}

// Map a Ren'Py `at` clause to its alignment. TRANSLATED from the engine's classic position
// transforms (renpy/common/00compat.rpy / 00definitions.rpy): left/right/center set xalign only and
// keep the default bottom placement; truecenter = xalign 0.5 + yalign 0.5; top = yalign 0.0. So
// x = xalign*(cw-sw), y = yalign*(ch-sh). (Custom game ATL transforms can't be resolved statically;
// they fall through to the default centre/bottom -- a known generality limit, not an eyeballed value.)
// Read the float after `key` (e.g. "xalign=") in the at string, or dflt if absent. Handles ".5",
// "0.5", "1.0" (atof stops at the trailing ',' or ')'). Used so Position(xalign=.., yalign=..) and
// xpos=/ypos= clauses translate to their actual values, not just the named-position heuristics.
static float atKeyVal(const char *at, const char *key, float dflt)
{
   if (!at) return dflt;
   const char *p = strstr(at, key);
   if (!p) return dflt;
   p += strlen(key);
   while (*p == ' ') p++;
   return (float)atof(p);
}
static float atToXalign(const char *at)
{
   if (!at || !at[0]) return 0.5f;            // no `at` -> default transform (centre x)
   if (strstr(at, "xalign=")) return atKeyVal(at, "xalign=", 0.5f);   // Position(xalign=..)
   if (strstr(at, "xpos="))   return atKeyVal(at, "xpos=", 0.5f);     // Position(xpos=..) fraction
   if (strstr(at, "left"))  return 0.0f;
   if (strstr(at, "right")) return 1.0f;
   return 0.5f;                               // center / truecenter / unknown
}
static float atToYalign(const char *at)
{
   if (!at || !at[0]) return 1.0f;            // default: bottom-anchored
   if (strstr(at, "yalign=")) return atKeyVal(at, "yalign=", 1.0f);   // Position(yalign=..)
   if (strstr(at, "ypos="))   return atKeyVal(at, "ypos=", 1.0f);     // Position(ypos=..) fraction
   if (strstr(at, "truecenter")) return 0.5f; // xalign 0.5 + yalign 0.5
   if (strstr(at, "top"))        return 0.0f; // top / topleft / topright
   return 1.0f;
}

void showSprite(const RbcProgram *p, const char *name, const char *at, int atlId)
{
   const char *file = resolveImageFile(name);
   if (!file) { logWarn("[rpp] show '%s' has no image def\n", name); return; }
   const char *base = baseName(file);
   const RbcAtl *atl = (atlId >= 0) ? getRbcAtl(p, atlId) : (const RbcAtl *)0;

   char tag[32];
   firstWord(name, tag, sizeof tag);

   int slot = -1;
   for (int i = 0; i < sprCount; i++)
      if (strcmp(sprites[i].tag, tag) == 0) { slot = i; break; }

   // Does THIS show specify a placement? Ren'Py: `show tag` with no at-clause and no positioning
   // transform KEEPS the tag's current placement; only an explicit `at` or a transform replaces it.
   // (at-clause present) OR (a compiled positioning ATL, atlId >= 0) => this show sets the position.
   int hasPos = (at && at[0]) || atlId >= 0;

   // Same tag + same file: keep the texture. Apply a new placement only if this show gave one;
   // otherwise preserve the existing position/animation (a bare re-show must not recentre it).
   if (slot >= 0 && strcmp(sprites[slot].file, base) == 0)
   {
      sprites[slot].src = name;
      if (hasPos)
      {
         sprites[slot].atSrc  = at;
         sprites[slot].xalign = atToXalign(at);
         sprites[slot].yalign = atToYalign(at);
         sprites[slot].atl    = atl;
         sprites[slot].atlId  = atlId;
         sprites[slot].atlStartUs = sys_time_get_system_time();
      }
      return;
   }

   GfxTexture t;
   if (!loadAssetTexture(base, &t)) return;

   int isNew = (slot < 0);
   if (isNew)
   {
      if (sprCount >= SPR_MAX) { freeGfxTexture(&t); logWarn("[rpp] sprite slots full\n"); return; }
      slot = sprCount++;
   }
   else freeGfxTexture(&sprites[slot].tex);   // replacing the same tag with a different image

   sprites[slot].tex = t;
   sprites[slot].src = name;   // stable pointer into the bytecode string table
   // New sprite, or a show that gives a placement: set position/atl. A bare re-show of an existing
   // tag with a NEW image keeps the tag's current placement (Ren'Py preserves it across re-shows).
   if (hasPos || isNew)
   {
      sprites[slot].atSrc = at;
      sprites[slot].xalign = atToXalign(at);
      sprites[slot].yalign = atToYalign(at);
      sprites[slot].atl = atl;
      sprites[slot].atlId = atlId;
      sprites[slot].atlStartUs = sys_time_get_system_time();
   }
   strncpy(sprites[slot].tag,  tag,  sizeof sprites[slot].tag  - 1); sprites[slot].tag [sizeof sprites[slot].tag  - 1] = '\0';
   strncpy(sprites[slot].file, base, sizeof sprites[slot].file - 1); sprites[slot].file[sizeof sprites[slot].file - 1] = '\0';
}

int getSpriteCount(void) { return sprCount; }

const Sprite *getSpriteAt(int i) { return (i >= 0 && i < sprCount) ? &sprites[i] : (const Sprite *)0; }

// ---- font ----

// Find the dialogue font name the script configures (gui.text_font, or old
// style.default.font / style.say_dialogue.font) in the bytecode's python strings.
static int findFontName(const RbcProgram *p, char *out, int cap)
{
   static const char *keys[] = { "gui.text_font", "style.say_dialogue.font", "style.default.font" };
   for (int k = 0; k < 3; k++)
      for (int i = 0; i < p->stringCount; i++)
      {
         const char *h = strstr(p->strings[i], keys[k]);
         if (!h) continue;
         const char *eq = strchr(h, '=');
         if (!eq) continue;
         const char *q = eq;
         while (*q && *q != '"' && *q != '\'' && *q != '\n') q++;
         if (*q != '"' && *q != '\'') continue;
         char quote = *q++;
         int j = 0;
         while (*q && *q != quote && j < cap - 1) out[j++] = *q++;
         out[j] = '\0';
         if (j > 0) { logInfo("[rpp] font: script uses %s = %s\n", keys[k], out); return 1; }
      }
   return 0;
}

// Try one font entry (matched by '/'+basename suffix) into gf. Returns 1 on open.
static int tryFont(RpkFile *r, const char *suffix, Font *gf)
{
   char name[256];
   unsigned char *buf = NULL;
   long len = 0;
   if (readRpkEntrySuffix(r, suffix, 0, name, sizeof name, &buf, &len) != 0 || !buf) return 0;
   logInfo("[rpp] font: trying %s (%ld bytes)\n", name, len);
   *gf = openFontMemory(buf, (uint32_t)len);
   free(buf);
   if (gf->open) { logInfo("[rpp] font: using %s\n", name); return 1; }
   logWarn("[rpp] font: %s did not open\n", name);
   return 0;
}

int loadNamedFont(const char *base, Font *out)
{
   if (!base || !base[0]) return 0;
   RpkFile r;
   if (openRpk(&r, getGameRpkPath()) != 0) return 0;
   const char *slash = strrchr(base, '/');
   const char *b = slash ? slash + 1 : base;
   char suffix[160];
   snprintf(suffix, sizeof suffix, "/%s", b);
   Font gf; memset(&gf, 0, sizeof gf);
   int ok = tryFont(&r, suffix, &gf);
   closeRpk(&r);
   if (!ok || !gf.open) return 0;
   *out = gf;
   return 1;
}

int loadGameFont(const RbcProgram *p, Font *font, int *fontReady)
{
   // Prefer the converter-resolved dialogue font (manifest text_font); fall back to scraping the
   // script only if the manifest didn't carry one.
   char preferred[160];
   int havePreferred;
   if (gui.dlgFont[0]) { snprintf(preferred, sizeof preferred, "%s", gui.dlgFont); havePreferred = 1; }
   else                  havePreferred = findFontName(p, preferred, sizeof preferred);

   RpkFile r;
   if (openRpk(&r, getGameRpkPath()) != 0) { logWarn("[rpp] font: openRpk failed\n"); return 0; }

   Font gf;
   memset(&gf, 0, sizeof gf);

   if (havePreferred && preferred[0])
   {
      const char *slash = strrchr(preferred, '/');
      const char *base = slash ? slash + 1 : preferred;
      char suffix[160];
      snprintf(suffix, sizeof suffix, "/%s", base);
      if (!tryFont(&r, suffix, &gf)) logWarn("[rpp] font: preferred %s not usable\n", preferred);
   }
   for (int idx = 0; !gf.open && idx < 16; idx++)
   {
      char name[256];
      unsigned char *buf = NULL;
      long len = 0;
      int rc = readRpkEntrySuffix(&r, ".ttf", idx, name, sizeof name, &buf, &len);
      if (rc != 0 || !buf) break;
      logInfo("[rpp] font: fallback trying %s (%ld bytes)\n", name, len);
      gf = openFontMemory(buf, (uint32_t)len);
      free(buf);
      if (gf.open) logInfo("[rpp] font: using %s\n", name);
   }

   closeRpk(&r);
   if (!gf.open) return 0;
   if (*fontReady) closeFont(font);
   *font = gf;
   *fontReady = 1;
   return 1;
}
