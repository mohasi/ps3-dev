#include "screens/home.h"

#include <string.h>
#include <stdlib.h>

#include "gfx.h"
#include "colors.h"
#include "font.h"
#include "pad.h"
#include "printf.h"
#include "dbg.h"
#include "rpk.h"         // read each game's game.gui for its title
#include "screen-manager.h"
#include "screens/play.h"
#include "config.h"
#include "gamepath.h"   // listGames() / selectGame()

static Font        font;
static int         fontReady;

static char        games[GAME_LIST_MAX][GAME_NAME_MAX];   // bare .rpk filenames in RENPY_ROOT
static int         gameCount;
static int         sel;                                   // highlighted entry

static TextTexture title, hint, message;
static TextTexture item[GAME_LIST_MAX];

// Strip a trailing ".rpk"/".RPK" for display (the picker shows clean game names, not filenames).
static void stripRpk(char *s)
{
   int n = (int)strlen(s);
   if (n > 4 && (strcmp(s + n - 4, ".rpk") == 0 || strcmp(s + n - 4, ".RPK") == 0)) s[n - 4] = '\0';
}

// Read a game's display title from its game.gui manifest (`title=...`, the game's config.window_title).
// Returns 1 + fills out on success; 0 if the rpk/title can't be read (caller falls back to the filename).
static int readGameTitle(const char *file, char *out, int cap)
{
   char path[256];
   snprintf(path, sizeof path, "%s/%s", RENPY_ROOT, file);
   RpkFile r;
   if (openRpk(&r, path) != 0) return 0;
   unsigned char *buf = NULL;
   long len = 0;
   int rc = readRpkEntry(&r, "game.gui", &buf, &len);
   closeRpk(&r);
   if (rc != 0 || !buf) return 0;

   int found = 0;
   const char *s = (const char *)buf;
   for (long i = 0; i < len; )
   {
      if ((i == 0 || s[i - 1] == '\n') && i + 6 <= len && strncmp(s + i, "title=", 6) == 0)
      {
         long j = i + 6; int k = 0;
         while (j < len && s[j] != '\n' && s[j] != '\r' && k < cap - 1) out[k++] = s[j++];
         out[k] = '\0';
         found = (k > 0);
         break;
      }
      while (i < len && s[i] != '\n') i++;   // skip to next line
      i++;
   }
   free(buf);
   return found;
}

static void buildLabels(void)
{
   renderFont(&title, &font, 40, "Ren'Py Player", COLOR_WHITE, 1740, TEXT_WRAP);
   for (int i = 0; i < gameCount; i++)
   {
      char disp[GAME_NAME_MAX];
      if (!readGameTitle(games[i], disp, sizeof disp))   // prefer the game's real title
      {
         snprintf(disp, sizeof disp, "%s", games[i]);   // fall back to the .rpk filename
         stripRpk(disp);
      }
      renderFont(&item[i], &font, 32, disp, COLOR_WHITE, 1500, TEXT_WRAP);
   }
   if (gameCount == 0)
      renderFont(&message, &font, 28,
                 "No games found in " RENPY_ROOT "\nCopy a .rpk there and restart.",
                 COLOR_WHITE, 1600, TEXT_WRAP);
   else
      renderFont(&hint, &font, 26, "Up / Down: choose      X: play", COLOR_WHITE, 1600, TEXT_WRAP);
}

static void homeInit(void)
{
   logInfo("[rpp] init\n");
   font = openSystemFont(FONT_SANS);
   fontReady = 1;

   gameCount = listGames(games, GAME_LIST_MAX);
   sel = 0;
   logInfo("[rpp] home: %d game(s) in %s\n", gameCount, RENPY_ROOT);
   for (int i = 0; i < gameCount; i++) logInfo("[rpp]   game[%d]: %s\n", i, games[i]);

   buildLabels();
}

static void homeResume(void) {}
static void homeSuspend(void) {}

static void homeUpdate(void)
{
   if (gameCount <= 0) return;
   if (isPadButtonPressed(PAD_BTN_UP))    sel = (sel - 1 + gameCount) % gameCount;
   if (isPadButtonPressed(PAD_BTN_DOWN))  sel = (sel + 1) % gameCount;
   if (isPadButtonPressed(PAD_BTN_CROSS))
   {
      selectGame(games[sel]);
      pushScreen(&playScreen);
   }
}

static void drawLabel(const TextTexture *t, int x, int y, uint32_t tint)
{
   if (t->valid)
      drawGfxTexture(x, y, t->tex.w, t->tex.h, t->tex, 0.0f, 0.0f, 1.0f, 1.0f, tint, GFX_FILTER_LINEAR);
}

static void homeDraw(void)
{
   clearGfx(0xFF101018);
   int x = 90, y = 80;

   drawLabel(&title, x, y, COLOR_WHITE);
   y += (title.valid ? title.tex.h : 48) + 40;

   if (gameCount == 0)
   {
      drawLabel(&message, x, y, COLOR_WHITE);
      return;
   }

   int rowH = (item[0].valid ? item[0].tex.h : 36) + 18;   // uniform row pitch
   int rowW = 1500;
   for (int i = 0; i < gameCount; i++)
   {
      int ry = y + i * rowH;
      if (i == sel)
         fillGfxRectangle(x - 20, ry - 6, rowW, rowH - 6, 0xFF2A3550);   // selection highlight bar
      drawLabel(&item[i], x + 20, ry, i == sel ? COLOR_WHITE : 0xFF8E96A8);   // dim the unselected
   }

   drawLabel(&hint, x, y + gameCount * rowH + 30, 0xFFB0B0B0);
}

static void homeTerm(void)
{
   freeTextTexture(&title);
   freeTextTexture(&hint);
   freeTextTexture(&message);
   for (int i = 0; i < GAME_LIST_MAX; i++) freeTextTexture(&item[i]);
   if (fontReady) { closeFont(&font); fontReady = 0; }
   logInfo("[rpp] term\n");
}

Screen homeScreen = {
   .init    = homeInit,
   .resume  = homeResume,
   .update  = homeUpdate,
   .draw    = homeDraw,
   .suspend = homeSuspend,
   .term    = homeTerm,
   .status  = SCREEN_TERMINATED,
};
