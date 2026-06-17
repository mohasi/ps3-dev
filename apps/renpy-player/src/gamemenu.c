#include "gamemenu.h"

#include <string.h>
#include <stdlib.h>        // atoi

#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "printf.h"        // snprintf
#include "gui.h"
#include "assets.h"        // loadAssetTexture
#include "config.h"        // RR12G_PATH / RR6G_PATH
#include "ui/slice.h"
#include "focus.h"         // geometric focus navigation (the engine's, not a per-game mapping)
#include "savestate.h"     // slot existence / label + time, for the file picker labels
#include "dbg.h"           // logWarn (thumbnail load diagnostics)
#include "vm.h"            // getProgram -> baked imagemaps
#include "rbc.h"           // RbcImageMap, getRbcImageMapByKind

#define GM_ITEM_MAX 8
#define GM_SLOT_MAX 60
#define GM_NUM_SLOTS   50  // config.load_save_slots
#define GM_QUICK_SLOTS 5   // config.load_save_quick_slots (shown when config.has_quicksave)
#define GM_AUTO_SLOTS  5   // config.load_save_auto_slots  (shown when config.has_autosave)

// Stable focus ids: nav buttons 0.., slots 1000.., the yes/no prompt 2000/2001.
#define ID_NAV(i)  (i)
#define ID_SLOT(i) (1000 + (i))
#define ID_YES     2000
#define ID_NO      2001

typedef enum { GM_SCREEN_NONE, GM_SCREEN_SAVE, GM_SCREEN_LOAD, GM_SCREEN_PREFS } GmScreen;

// One navigation entry. The game ships per-label button art (theme.image_buttons -> mm_btn.<label> =
// idle|hover|selected|insensitive); with art we draw the right state image, else a roundrect text button.
typedef struct {
   const char *label;        // engine config.game_menu label (also the art lookup key)
   GmAction    action;
   GmScreen    screen;       // sub-screen this opens (NONE for Return/Main Menu/Quit)
   int         needsInGame;  // enabled only when not on the main menu (Save Game / Main Menu)
   GfxTexture  idle, hover, selected, insensitive;
   int         hasIdle, hasHover, hasSelected, hasInsensitive;
   TextTexture text;         // fallback label (only when there's no art)
} NavButton;

static Font      *font;
static GfxTexture gmBgTex;   static int gmBgLoaded;
static GfxTexture frameTex;  static int frameLoaded, frameCap;
static GfxTexture barTex;     static int barLoaded;        // vscrollbar track template
static GfxTexture barThumbTex;static int barThumbLoaded;   // vscrollbar thumb template
static NavButton  nav[GM_ITEM_MAX];
static int        navCount;
static int        textBuilt, textBuiltWidth;

static GmScreen   currentScreen;   // the sub-screen the menu is on (default: Save, in-game)
static int        mainMenu;        // 1 = opened from the title (we only open in-game -> 0)

static struct { char display[8]; TextTexture tex; } slot[GM_SLOT_MAX];
static int slotCount, slotTop;     // slotTop = first visible row (scroll), derived from the focus
static int slotsBuilt, slotsBuiltWidth, slotsBuiltScreen;   // screen tracked: insensitive text colour depends on it

// The focused slot's thumbnail (Ren'Py shows the HOVERED slot's): the slot's screenshot slot-<n>.png.
// Slots with no PNG (e.g. older saves) show the bare frame. thumbSlot tracks which slot is loaded so
// it's rebuilt only on a focus change.
static char       thumbSlot[8];   // which slot the thumbnail currently holds ("" = none)
static GfxTexture thumbTex;       // the slot's screenshot PNG
static int        thumbLoaded;    // 1 = thumbTex valid (draw it)

static int         confirmActive;
static GmAction    confirmAction;
static int         confirmFromId;   // the nav button to refocus when the prompt closes
static TextTexture confirmTex, yesTex, noTex;

static void buildSlotModel(void);
static void lsEnter(void);     // load the imagemap load_save layout (if the game provides one)
static void lsFree(void);      // free imagemap textures + slot text
static void lsRefresh(void);   // re-read saves for the imagemap picker (after a save)

// Engine-constant label -> action/screen/condition map for config.game_menu (00layout.rpy). This is
// NOT a per-game list -- it's Ren'Py's own table; the game's actual buttons + order come from the
// manifest gm_order. "Help" opens the readme (no-op on PS3 -> inert). Unknown/custom labels render
// inert too. GM_ITEM_MAX bounds the rendered set.
static const struct { const char *label; GmAction action; GmScreen screen; int needsInGame; } MENU[] = {
   { "Return",      GM_RETURN,      GM_SCREEN_NONE,  0 },
   { "Preferences", GM_PREFERENCES, GM_SCREEN_PREFS, 0 },
   { "Save Game",   GM_SAVE,        GM_SCREEN_SAVE,  1 },
   { "Load Game",   GM_LOAD,        GM_SCREEN_LOAD,  0 },
   { "Main Menu",   GM_MAINMENU,    GM_SCREEN_NONE,  1 },
   { "Help",        GM_NONE,        GM_SCREEN_NONE,  0 },
   { "Quit",        GM_QUIT,        GM_SCREEN_NONE,  0 },
};

static const char *MSG_MAIN_MENU = "Are you sure you want to return to the main menu?\nThis will lose unsaved progress.";   // layout.MAIN_MENU
static const char *MSG_QUIT      = "Are you sure you want to quit?";
static const char *MSG_OVERWRITE = "Are you sure you want to overwrite your save?";          // layout.OVERWRITE_SAVE
static const char *MSG_LOAD      = "Loading will lose unsaved progress.\nAre you sure you want to do this?";  // layout.LOADING

static char selectedSlot[8];   // the slot display ("1".."a5") picked for GM_DO_SAVE / GM_DO_LOAD
const char *getGameMenuSelectedSlot(void) { return selectedSlot; }
void        refreshGameMenuSlots(void)   // next draw re-reads the save folder (after a save)
{
   slotsBuilt = 0;
   if (thumbLoaded) { freeGfxTexture(&thumbTex); thumbLoaded = 0; }
   thumbSlot[0] = '\0';   // force the just-saved slot's thumbnail to rebuild from the new save
   lsRefresh();           // imagemap picker: re-read saves + newest
}

void initGameMenu(Font *sharedFont) { font = sharedFont; }

void freeGameMenu(void)
{
   if (gmBgLoaded)  { freeGfxTexture(&gmBgTex);  gmBgLoaded = 0; }
   if (frameLoaded) { freeGfxTexture(&frameTex); frameLoaded = 0; }
   if (barLoaded)      { freeGfxTexture(&barTex);      barLoaded = 0; }
   if (barThumbLoaded) { freeGfxTexture(&barThumbTex); barThumbLoaded = 0; }
   for (int i = 0; i < navCount; i++)
   {
      if (nav[i].hasIdle)        freeGfxTexture(&nav[i].idle);
      if (nav[i].hasHover)       freeGfxTexture(&nav[i].hover);
      if (nav[i].hasSelected)    freeGfxTexture(&nav[i].selected);
      if (nav[i].hasInsensitive) freeGfxTexture(&nav[i].insensitive);
      freeTextTexture(&nav[i].text);
   }
   freeTextTexture(&confirmTex); freeTextTexture(&yesTex); freeTextTexture(&noTex);
   for (int i = 0; i < slotCount; i++) freeTextTexture(&slot[i].tex);
   if (thumbLoaded) { freeGfxTexture(&thumbTex); thumbLoaded = 0; }
   thumbSlot[0] = '\0';
   lsFree();
   navCount = 0;
   slotCount = 0;
   textBuilt = 0;
}

static void loadButtonArt(NavButton *button)
{
   for (int i = 0; i < gui.mmBtnCount; i++)
   {
      if (strcmp(gui.mmBtnLabel[i], button->label) != 0) continue;
      button->hasIdle = loadAssetTexture(gui.mmBtnIdle[i], &button->idle);
      if (gui.mmBtnHover[i][0])       button->hasHover       = loadAssetTexture(gui.mmBtnHover[i], &button->hover);
      if (gui.mmBtnSelected[i][0])    button->hasSelected    = loadAssetTexture(gui.mmBtnSelected[i], &button->selected);
      if (gui.mmBtnInsensitive[i][0]) button->hasInsensitive = loadAssetTexture(gui.mmBtnInsensitive[i], &button->insensitive);
      return;
   }
}

void enterGameMenu(void)
{
   freeGameMenu();
   // Build the nav column from the game's actual config.game_menu (manifest gm_order); fall back to
   // the engine-default table when the manifest doesn't carry it. Each label's action/condition comes
   // from the engine-constant MENU table; the art comes from mmBtn.<label> (else a roundrect text button).
   int useOrder = gui.gmOrderCount > 0;
   int count = useOrder ? gui.gmOrderCount : (int)(sizeof MENU / sizeof MENU[0]);
   for (int i = 0; i < count && navCount < GM_ITEM_MAX; i++)
   {
      const char *label = useOrder ? gui.gmOrder[i] : MENU[i].label;
      NavButton *button = &nav[navCount];
      memset(button, 0, sizeof *button);
      button->label       = label;
      button->action      = GM_NONE;          // unknown/custom label -> inert
      button->screen      = GM_SCREEN_NONE;
      button->needsInGame = 0;
      for (int m = 0; m < (int)(sizeof MENU / sizeof MENU[0]); m++)
         if (strcmp(MENU[m].label, label) == 0)
         { button->action = MENU[m].action; button->screen = MENU[m].screen; button->needsInGame = MENU[m].needsInGame; break; }
      loadButtonArt(button);
      navCount++;
   }
   buildSlotModel();
   lsEnter();                          // load the imagemap load_save layout if the game provides one
   clearFocus();
   confirmActive = 0;
   mainMenu = 0;                       // we only open the menu during play
   currentScreen = GM_SCREEN_SAVE;     // the in-game menu lands on the save screen by default

   gmBgLoaded = gui.gmBg[0] ? loadAssetTexture(gui.gmBg, &gmBgTex) : 0;
   if (gui.nativeW > 0 && gui.nativeW <= 640) { frameTex = loadGfxTexture(RR6G_PATH);  frameCap = 6;  }
   else                                       { frameTex = loadGfxTexture(RR12G_PATH); frameCap = 12; }
   frameLoaded = (frameTex.w > 0 && frameTex.h > 0);

   barTex      = loadGfxTexture(RRVSCROLLBAR_PATH);        barLoaded      = (barTex.w > 0 && barTex.h > 0);
   barThumbTex = loadGfxTexture(RRVSCROLLBAR_THUMB_PATH);  barThumbLoaded = (barThumbTex.w > 0 && barThumbTex.h > 0);
}

// Open the in-game menu on a specific sub-screen, chosen by the engine screen label the quick-menu
// button targets (config.game_menu: Save Game -> save_screen, Load Game -> load_screen, Preferences ->
// preferences_screen). Without this every quick-menu button landed on Save (enterGameMenu's default).
void enterGameMenuOn(const char *engineScreen)
{
   enterGameMenu();
   if (!engineScreen) return;
   if      (strstr(engineScreen, "load_screen"))  currentScreen = GM_SCREEN_LOAD;
   else if (strstr(engineScreen, "save_screen"))  currentScreen = GM_SCREEN_SAVE;
   else if (strstr(engineScreen, "preferences"))  currentScreen = GM_SCREEN_PREFS;
}

// Open from the title via config.main_menu's "Load Game" (_intra_jumps("load_screen")). Same screen
// as the in-game menu, but with main_menu = True: the engine's `not main_menu` conditions then grey
// Save Game and Main Menu, and the menu lands on the Load sub-screen.
void enterGameMenuFromTitle(void)
{
   enterGameMenu();
   mainMenu      = 1;
   currentScreen = GM_SCREEN_LOAD;
}

// roundrect button/large_button xpadding is 6 when less_rounded (config.screen_width <= 640) else 12
// (00themes.rpy). Same test that picks the rr6g/rr12g frame template.
static int btnXpad(void) { return (gui.nativeW > 0 && gui.nativeW <= 640) ? 6 : 12; }

static int buttonEnabled(const NavButton *button) { return !(button->needsInGame && mainMenu); }
static int buttonSelected(const NavButton *button) { return button->screen != GM_SCREEN_NONE && button->screen == currentScreen; }

// The art for a button's state (idle / focused-hover / current-screen "selected" / insensitive).
static const GfxTexture *buttonArt(const NavButton *button, int focused)
{
   if (!buttonEnabled(button) && button->hasInsensitive) return &button->insensitive;
   if (buttonSelected(button) && button->hasSelected)    return &button->selected;
   if (focused && button->hasHover)                      return &button->hover;
   return &button->idle;
}

static void buildSlotModel(void)
{
   // scrolling_load_save builds the list in this order: numbered, then quick (if has_quicksave),
   // then auto (if has_autosave). Labels: "1".."50", "q1".., "a1"... has_quicksave/has_autosave
   // default True and this game overrides neither (a converter-extracted manifest flag should drive
   // these per-game once we support titles that turn them off).
   slotCount = 0;
   for (int i = 1; i <= GM_NUM_SLOTS   && slotCount < GM_SLOT_MAX; i++) snprintf(slot[slotCount++].display, 8, "%d",  i);
   for (int i = 1; i <= GM_QUICK_SLOTS && slotCount < GM_SLOT_MAX; i++) snprintf(slot[slotCount++].display, 8, "q%d", i);
   for (int i = 1; i <= GM_AUTO_SLOTS  && slotCount < GM_SLOT_MAX; i++) snprintf(slot[slotCount++].display, 8, "a%d", i);
   slotTop = 0; slotsBuilt = 0;
}

// Every theme text inherits style.default.drop_shadow (the roundrect theme never overrides it on
// button_text / large_button_text), so menu + slot text carries the same global shadow as dialogue.
static const TextShadow *menuShadow(int cw, TextShadow *out) { return getGuiTextShadow(cw, out) ? out : NULL; }

// Line-box metrics for the button text size (constant for a given font + size): the line pitch
// and the line-cell top in texture coords (negative -- the texture is cropped to the ink, above
// which the cell top sits). Captured once per build; used to place a single line in a button the
// way the engine does (size to the line box, centre it -- button_text yalign 0.5).
static int gmLineH, gmLineTop;

static void buildText(int cw)
{
   int size = getGuiGmTextSize(cw);
   TextShadow sb; const TextShadow *shadow = menuShadow(cw, &sb);
   for (int i = 0; i < navCount; i++)
      if (!nav[i].hasIdle) renderFontEx(&nav[i].text, font, size, nav[i].label, gui.gmBtnText, cw, TEXT_NOWRAP, shadow, NULL);
   TextEnd te;
   renderFontEx(&yesTex, font, size, "Yes", gui.gmBtnText, cw, TEXT_NOWRAP, shadow, &te);
   renderFontEx(&noTex,  font, size, "No",  gui.gmBtnText, cw, TEXT_NOWRAP, shadow, NULL);
   gmLineH   = te.valid ? te.lineHeight : size;
   gmLineTop = te.valid ? te.lineTop    : 0;
   textBuilt = 1;
   textBuiltWidth = cw;
}

// A file-picker slot is INSENSITIVE (RoundRect(disabled) bg + insensitive_color text, not activatable)
// exactly when the engine sets clicked=None in scrolling_load_save.rpym's _file_picker: on the SAVE
// screen for auto-slots (read-only), and on the LOAD screen for empty slots (nothing to load). This is
// why the empty-slot colour differs between the Save and Load (Continue) screens.
static int isSlotInsensitive(const char *display)
{
   if (currentScreen == GM_SCREEN_SAVE) return display[0] == 'a';        // auto- not writable on save
   if (currentScreen == GM_SCREEN_LOAD) return !saveSlotExists(display); // empty not loadable on load
   return 0;
}

static void buildSlotText(int cw)
{
   int size = getGuiSlotTextSize(cw);
   TextShadow sb; const TextShadow *shadow = menuShadow(cw, &sb);   // file_picker_text -> large_button_text -> default drop_shadow

   // The engine gives role="selected_" to the NEWEST numbered save (max mtime among "0-9" slots);
   // a selected large_button renders its text in large_button_text.selected_color (widget_selected).
   int newestIdx = -1; long newestMt = -1;
   for (int i = 0; i < slotCount; i++)
   {
      long mt;
      if (slot[i].display[0] >= '0' && slot[i].display[0] <= '9' &&
          saveSlotMtime(slot[i].display, &mt) && mt > newestMt) { newestMt = mt; newestIdx = i; }
   }

   for (int i = 0; i < slotCount; i++)
   {
      char line[128], time[32], name[64];
      // config.file_entry_format "%(time)s\n%(save_name)s" for a saved slot, else "Empty Slot."
      if (saveSlotInfo(slot[i].display, time, sizeof time, name, sizeof name))
         snprintf(line, sizeof line, "%s. %s\n%s", slot[i].display, time, name);
      else
         snprintf(line, sizeof line, "%s. Empty Slot.", slot[i].display);
      // large_button_text colour: insensitive_color (disabled) > selected_color (newest) > widget_text.
      uint32_t color = isSlotInsensitive(slot[i].display) ? gui.gmBtnDisabledText
                  : (i == newestIdx) ? gui.gmBtnSelected : gui.gmBtnText;
      renderFontEx(&slot[i].tex, font, size, line, color, cw, TEXT_WRAP, shadow, NULL);
   }
   slotsBuilt = 1; slotsBuiltWidth = cw; slotsBuiltScreen = currentScreen;
}

// Load slot `display`'s screenshot thumbnail (cached -- a no-op when it's already the loaded slot;
// pass "" to clear). Slots with no PNG (e.g. older saves) show the bare frame. Decodes the image, so
// this runs only when the focused slot changes, not per frame.
static void ensureThumb(const char *display)
{
   if (strcmp(thumbSlot, display) == 0) return;
   snprintf(thumbSlot, sizeof thumbSlot, "%s", display);
   if (thumbLoaded) { freeGfxTexture(&thumbTex); thumbLoaded = 0; }
   if (!display[0]) return;

   char path[256]; saveThumbPath(display, path, sizeof path);
   GfxTexture t = loadGfxTexture(path);
   if (t.w > 0 && t.h > 0) { thumbTex = t; thumbLoaded = 1; }
}

static void drawRoundRect(int x, int y, int w, int h, float scale, uint32_t tint)
{
   if (!frameLoaded) return;
   SpriteRegion whole = { 0, 0, frameTex.w, frameTex.h };
   NineSlice slice;
   initNineSliceScaled(&slice, frameTex, x, y, w, h, whole, frameCap, frameCap, scale);
   slice.tint = tint;
   drawNineSlice(&slice);
}

static void drawTex(const TextTexture *tex, int x, int y)
{
   if (tex->valid)
      drawGfxTexture(x, y, tex->tex.w, tex->tex.h, tex->tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
}

// Draw a single-line label centred in a roundrect button. The engine sizes a button to its text's
// LINE BOX + padding and centres the text (button_text xalign/yalign 0.5). We place the line cell
// top so the line box is centred in the button, rather than centring the asymmetrically-cropped
// texture (whose top hugs the ink and whose bottom carries the descent slack -> text rode high).
static void drawBtnLabel(const TextTexture *tex, int btnX, int btnY, int btnW, int btnH)
{
   if (!tex->valid) return;
   int x = btnX + (btnW - tex->tex.w) / 2;                 // xalign 0.5
   int y = btnY + (btnH - gmLineH) / 2 - gmLineTop;        // line box centred in the button
   drawTex(tex, x, y);
}

// The scrolling slot list (file_picker, left half). Registers every slot as a focusable at its
// scrolled position (so the d-pad can reach off-screen rows, scrolling to follow), draws the visible
// ones. `register_` is 0 while the prompt is up (slots aren't focusable then).
static void drawSlots(int cx, int cy, int cw, int ch, int register_)
{
   if (!slotsBuilt || slotsBuiltWidth != cw || slotsBuiltScreen != currentScreen) buildSlotText(cw);
   float scale = getGuiScale(cw);
   int margin  = (int)(6 * scale + 0.5f);   // file_picker_frame xmargin/ymargin
   int pad     = (int)(6 * scale + 0.5f);   // style.frame xpadding/ypadding
   int slotM   = (int)(1 * scale + 0.5f);   // large_button xmargin/ymargin = 1 (small gap around each slot rect)
   int rowH    = (int)(58 * scale + 0.5f);  // file_picker_entry yminimum 58 = the vbox row pitch
   int btnH    = rowH - 2 * slotM;          // the button rect sits inside its row, inset by the 1px margin
   int barW    = (int)(12 * scale + 0.5f);  // vscrollbar xmaximum 12

   int frameX = cx + margin, frameY = cy + margin;
   int frameW = (int)(cw * 0.5f) - 2 * margin;
   int frameH = ch - 2 * margin;
   drawRoundRect(frameX, frameY, frameW, frameH, scale, gui.mmFrameColor);   // file_picker_frame

   int listX = frameX + pad, listY = frameY + pad;
   int listW = frameW - 2 * pad - barW;
   int listH = frameH - 2 * pad;
   int visible = listH / rowH; if (visible < 1) visible = 1;   // fully-visible rows (the viewport clips a partial one more)

   int fid = getFocusId();                                              // scroll to keep the focused slot visible
   int fslot = (fid >= ID_SLOT(0) && fid < ID_SLOT(slotCount)) ? fid - ID_SLOT(0) : -1;
   if (fslot >= 0)
   {
      if (fslot < slotTop) slotTop = fslot;
      else if (fslot >= slotTop + visible) slotTop = fslot - visible + 1;
   }

   // The engine viewport shows the rows that fit plus a partial last one; with pitch 58 the 10th
   // overflows the 576px viewport by only ~3px, which lands inside the frame's own 6px bottom
   // padding -- so we draw it without a hard clip and never touch the file_picker_frame border.
   for (int i = 0; i < slotCount; i++)
   {
      int cellTop = listY + (i - slotTop) * rowH;
      // Only SENSITIVE slots are focusable -- Ren'Py sets focusable=False on an insensitive button
      // (behavior.py: clicked is None -> not focusable). So the d-pad skips empty Load slots / auto
      // Save slots and lands on the nearest real one; an all-empty Load list can't be entered (there
      // is nothing to load). This is the faithful behaviour AND fixes "Left lands on nothing".
      if (register_ && !isSlotInsensitive(slot[i].display))
         addFocus(ID_SLOT(i), listX + slotM, cellTop + slotM, listW - 2 * slotM, btnH);
      if (cellTop + rowH > listY && cellTop < listY + listH)
      {
         int rx = listX + slotM, ry = cellTop + slotM, rw = listW - 2 * slotM;   // large_button rect (inset by xmargin/ymargin)
         // RoundRect background: insensitive_background (disabled) > hover (focused) > idle. An
         // insensitive slot stays disabled even under the focus cursor (the engine can't hover it).
         uint32_t slotBg = isSlotInsensitive(slot[i].display) ? gui.gmBtnDisabled
                     : (isFocused(ID_SLOT(i)) ? gui.gmBtnHover : gui.gmBtnIdle);
         drawRoundRect(rx, ry, rw, btnH, scale, slotBg);
         // large_button_text: xalign 0 / yalign 0 (top-left), button xpadding (6/12) / ypadding 1.
         drawTex(&slot[i].tex, rx + (int)(btnXpad() * scale + 0.5f), ry + (int)(1 * scale + 0.5f));
      }
   }

   if (slotCount > visible)   // file_picker_scrollbar = vscrollbar: xmaximum 12, left/right_bar img("vscrollbar",widget),
   {                          // thumb img("vscrollbar_thumb",widget) -- a plain Image (natural size), not stretched
      int barX = listX + listW;                       // the 'r' column of ui.side ['c','r']
      if (barLoaded)                                  // track: rrvscrollbar (ycap 12), tinted widget=idle, full bar height
      {
         SpriteRegion whole = { 0, 0, barTex.w, barTex.h };
         NineSlice s; initNineSliceScaled(&s, barTex, barX, listY, barW, listH, whole, 0, 12, scale);
         s.tint = gui.gmBtnIdle; drawNineSlice(&s);
      }
      int thumbH = (int)(barThumbTex.h * scale + 0.5f);            // natural thumb height (engine draws the Image unstretched)
      int travel = listH - thumbH; if (travel < 0) travel = 0;
      int denom  = slotCount - visible; if (denom < 1) denom = 1;
      int thumbY = listY + travel * slotTop / denom;              // flush to the top at slotTop 0
      if (barThumbLoaded)
         drawGfxTexture(barX, thumbY, barW, thumbH, barThumbTex, 0.0f, 0.0f, 1.0f, 1.0f, gui.gmBtnIdle, GFX_FILTER_LINEAR);
   }

   // thumbnail_frame = Style(style.frame): xpos 0.75, xanchor 0.5, ymargin 6. It sets NO ypos/yanchor,
   // so it inherits style.frame's default (ypos 0, yanchor 0) -> pinned to the TOP, not centred. The
   // frame wraps the 320x240 thumbnail with style.frame's 6px xpadding/ypadding (GM4 fills the image).
   int thumbPad = (int)(6 * scale + 0.5f), thumbMargin = (int)(6 * scale + 0.5f);
   int innerW = (int)(320 * scale + 0.5f), innerH = (int)(240 * scale + 0.5f);
   int tfW = innerW + 2 * thumbPad, tfH = innerH + 2 * thumbPad;
   int tfX = cx + (int)(cw * 0.75f + 0.5f) - tfW / 2;   // xpos 0.75, xanchor 0.5
   int tfY = cy + thumbMargin;                          // ypos 0 (default) + ymargin
   drawRoundRect(tfX, tfY, tfW, tfH, scale, gui.mmFrameColor);

   // The hovered (focused) slot's screenshot inside the frame's padding. Only occupied slots with a
   // saved PNG have one; focus off the list, an empty slot, or a save with no PNG shows the bare frame.
   const char *fdisp = (fslot >= 0) ? slot[fslot].display : "";
   if (fdisp[0] && !saveSlotExists(fdisp)) fdisp = "";
   ensureThumb(fdisp);
   if (thumbLoaded)
      drawGfxTexture(tfX + thumbPad, tfY + thumbPad, innerW, innerH, thumbTex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
}

// ===================== imagemap load/save layout (imagemap_load_save.rpym) =====================
// Faithful alternative to the roundrect/scrolling picker, used when the game provides an imagemap
// load_save screen. Same generic file-picker MECHANISM as the engine (_file_picker / _file_picker_init):
// hotspot NAMES carry the structure -- slot_0..slot_{per_page-1} are the slots, page_1..page_N +
// previous/next the pager, page_auto/page_quick the special pages, and any config.game_menu label
// (Return, ...) a nav button. We list saves, map the current page's files onto the slots, draw each
// slot's state image + text, and dispatch the chosen name. No per-game geometry -- all from the rbc.
#define ID_IMHS(i) (3000 + (i))

enum { LS_GROUND = 0, LS_IDLE, LS_HOVER, LS_SEL_IDLE, LS_SEL_HOVER };

static const RbcImageMap *lsImap;                     // baked load_save imagemap (NULL => roundrect)
static GfxTexture lsTex[5]; static int lsTexOk[5];    // ground, idle, hover, selected_idle, selected_hover
static int lsHasAuto, lsHasQuick, lsPerPage, lsPageCount;   // derived from the hotspot names
static int lsPage;                                    // current page index (into the pages list)
static char lsNewest[12];                             // newest save filename (gets the "selected" image)
static struct { char fn[12]; TextTexture tex; int occupied; GfxTexture thumb; int thumbOk; } lsSlot[16];
static int lsSlotsW, lsSlotsPage, lsSlotsValid;

static void askConfirm(GmAction action, const char *message, int cw);   // defined below

static int lsActive(void) { return lsImap && (currentScreen == GM_SCREEN_SAVE || currentScreen == GM_SCREEN_LOAD); }

static const char *lsBase(const char *p)   // asset basename (rpk entries are basenames)
{
   if (!p) return "";
   const char *s = strrchr(p, '/'); if (s) return s + 1;
   s = strrchr(p, '\\'); return s ? s + 1 : p;
}

static int imHotspot(const RbcImageMap *im, const char *name)
{
   if (!im) return -1;
   for (int i = 0; i < im->hotspotCount; i++)
      if (im->hotspots[i].name && strcmp(im->hotspots[i].name, name) == 0) return i;
   return -1;
}

// Native-px hotspot rect -> letterboxed content rect (same mapping as the simple imagemap renderer).
static void imRect(const RbcHotspot *h, int cx, int cy, int cw, int ch, int *x, int *y, int *w, int *hh)
{
   float nw = gui.nativeW > 0 ? (float)gui.nativeW : 800.0f;
   float nh = gui.nativeH > 0 ? (float)gui.nativeH : 600.0f;
   *x  = cx + (int)((float)h->x0 / nw * cw + 0.5f);
   *y  = cy + (int)((float)h->y0 / nh * ch + 0.5f);
   *w  = (int)((float)(h->x1 - h->x0) / nw * cw + 0.5f);
   *hh = (int)((float)(h->y1 - h->y0) / nh * ch + 0.5f);
}

static int lsResolve(int slot)   // engine fallback chain to a loaded texture (or -1)
{
   for (;;) {
      if (slot < 0) return -1;
      if (lsTexOk[slot]) return slot;
      switch (slot) {
         case LS_IDLE:      slot = LS_GROUND; break;
         case LS_HOVER:     slot = LS_IDLE;   break;
         case LS_SEL_IDLE:  slot = LS_IDLE;   break;
         case LS_SEL_HOVER: slot = LS_HOVER;  break;
         default: return -1;
      }
   }
}

// Draw a hotspot's state image cropped to its rect (LiveCrop) at its screen position.
static void lsDrawHotspot(const RbcHotspot *h, int slot, int cx, int cy, int cw, int ch)
{
   int t = lsResolve(slot); if (t < 0) return;
   float nw = gui.nativeW > 0 ? (float)gui.nativeW : 800.0f;
   float nh = gui.nativeH > 0 ? (float)gui.nativeH : 600.0f;
   int x, y, w, hh; imRect(h, cx, cy, cw, ch, &x, &y, &w, &hh);
   drawGfxTexture(x, y, w, hh, lsTex[t],
                  (float)h->x0 / nw, (float)h->y0 / nh, (float)h->x1 / nw, (float)h->y1 / nh,
                  COLOR_WHITE, GFX_FILTER_LINEAR);
}

// Map page index + slot offset to a save filename, per _file_picker_page_files. `ro` = read-only (auto/quick).
static void lsPageFile(int page, int offset, char *fn, int fnsz, int *ro)
{
   int idx = page; *ro = 0;
   if (lsHasAuto)  { if (idx == 0) { snprintf(fn, fnsz, "auto-%d",  offset + 1); *ro = 1; return; } idx--; }
   if (lsHasQuick) { if (idx == 0) { snprintf(fn, fnsz, "quick-%d", offset + 1); *ro = 1; return; } idx--; }
   snprintf(fn, fnsz, "%d", lsPerPage * idx + offset + 1);
}

// Page index for a page-name hotspot ("page_auto"/"page_quick"/"page_K"), or -1.
static int lsPageOfName(const char *name)
{
   int base = (lsHasAuto ? 1 : 0) + (lsHasQuick ? 1 : 0);
   if (lsHasAuto  && strcmp(name, "page_auto")  == 0) return 0;
   if (lsHasQuick && strcmp(name, "page_quick") == 0) return (lsHasAuto ? 1 : 0);
   if (strncmp(name, "page_", 5) == 0) { int k = atoi(name + 5); if (k >= 1) return base + (k - 1); }
   return -1;
}

// A slot's display label (number, or aN/qN) from its filename.
static void lsDisplay(const char *fn, char *out, int outsz)
{
   if (strncmp(fn, "auto-", 5) == 0)       snprintf(out, outsz, "a%s", fn + 5);
   else if (strncmp(fn, "quick-", 6) == 0) snprintf(out, outsz, "q%s", fn + 6);
   else                                    snprintf(out, outsz, "%s", fn);
}

static void lsFreeSlots(void)
{
   for (int i = 0; i < 16; i++) {
      if (lsSlot[i].tex.valid) freeTextTexture(&lsSlot[i].tex);
      if (lsSlot[i].thumbOk) { freeGfxTexture(&lsSlot[i].thumb); lsSlot[i].thumbOk = 0; }
   }
   lsSlotsValid = 0;
}

// Scan the saves reachable from this imagemap (per_page * numbered pages, plus auto/quick) for the
// newest non-auto save -- it gets the "selected" image, exactly like role="selected_" in the engine.
static void lsFindNewest(void)
{
   lsNewest[0] = '\0';
   long newestMt = -1;
   int numberedPages = lsPageCount - (lsHasAuto ? 1 : 0) - (lsHasQuick ? 1 : 0);
   if (numberedPages < 1) numberedPages = 1;
   int maxN = lsPerPage * numberedPages;
   for (int i = 1; i <= maxN; i++) {
      char fn[12]; long mt;
      snprintf(fn, sizeof fn, "%d", i);
      if (saveSlotMtime(fn, &mt) && mt > newestMt) { newestMt = mt; snprintf(lsNewest, sizeof lsNewest, "%s", fn); }
   }
   if (lsHasQuick)
      for (int i = 1; i <= lsPerPage; i++) {
         char fn[12]; long mt; snprintf(fn, sizeof fn, "quick-%d", i);
         if (saveSlotMtime(fn, &mt) && mt > newestMt) { newestMt = mt; snprintf(lsNewest, sizeof lsNewest, "%s", fn); }
      }
}

// Load the load_save imagemap (textures + per_page/pages from the names). Called from enterGameMenu.
static void lsEnter(void)
{
   lsImap = getRbcImageMapByKind(getProgram(), "load_save");
   for (int i = 0; i < 5; i++) lsTexOk[i] = 0;
   lsHasAuto = lsHasQuick = lsPerPage = lsPageCount = lsPage = 0;
   lsNewest[0] = '\0';
   if (!lsImap) return;

   const char *src[5] = { lsImap->ground, lsImap->idle, lsImap->hover, lsImap->selectedIdle, lsImap->selectedHover };
   for (int i = 0; i < 5; i++)
      if (src[i] && src[i][0]) lsTexOk[i] = loadAssetTexture(lsBase(src[i]), &lsTex[i]);

   // _file_picker_init: per_page = count of slot_N; pages list = page_auto? page_quick? page_1..page_M.
   lsHasAuto  = imHotspot(lsImap, "page_auto")  >= 0;
   lsHasQuick = imHotspot(lsImap, "page_quick") >= 0;
   while (lsPerPage < 16) { char n[16]; snprintf(n, sizeof n, "slot_%d", lsPerPage); if (imHotspot(lsImap, n) < 0) break; lsPerPage++; }
   int numbered = 0;
   for (;;) { char n[16]; snprintf(n, sizeof n, "page_%d", numbered + 1); if (imHotspot(lsImap, n) < 0) break; numbered++; }
   lsPageCount = (lsHasAuto ? 1 : 0) + (lsHasQuick ? 1 : 0) + numbered;
   if (lsPageCount < 1) lsPageCount = 1;

   lsFindNewest();
   // Land on the page holding the newest save (_file_picker: fpp = _file_picker_file_page(newest|"1")).
   int base = (lsHasAuto ? 1 : 0) + (lsHasQuick ? 1 : 0);
   int newestNum = lsNewest[0] ? atoi(lsNewest) : 1;
   if (newestNum < 1) newestNum = 1;
   if (lsPerPage > 0) lsPage = base + (newestNum - 1) / lsPerPage;
   if (lsPage >= lsPageCount) lsPage = lsPageCount - 1;
   if (lsPage < 0) lsPage = 0;
}

static void lsFree(void)
{
   for (int i = 0; i < 5; i++) if (lsTexOk[i]) { freeGfxTexture(&lsTex[i]); lsTexOk[i] = 0; }
   lsFreeSlots();
   lsImap = NULL;
}

static void lsRefresh(void)   // after a save: re-list and recompute the newest, rebuild slot text
{
   if (!lsImap) return;
   lsFindNewest();
   lsSlotsValid = 0;
}

// Build the current page's slot text (number/aN/qN + ". " + time + "\n" + name, or ". Empty Slot.").
static void lsBuildSlots(int cw)
{
   lsFreeSlots();
   int size = getGuiSlotTextSize(cw);
   TextShadow sb; const TextShadow *shadow = menuShadow(cw, &sb);
   uint32_t color = gui.slotTextColor ? gui.slotTextColor : gui.gmBtnText;   // file_picker_text (override of large_button_text)
   float nw = gui.nativeW > 0 ? (float)gui.nativeW : 800.0f;
   float scale = (float)cw / nw;

   for (int o = 0; o < lsPerPage && o < 16; o++) {
      char fn[12], disp[12], line[160], time[32], name[64]; int ro;
      lsPageFile(lsPage, o, fn, sizeof fn, &ro);
      snprintf(lsSlot[o].fn, sizeof lsSlot[o].fn, "%s", fn);
      lsDisplay(fn, disp, sizeof disp);
      lsSlot[o].occupied = saveSlotExists(fn);
      if (lsSlot[o].occupied && saveSlotInfo(fn, time, sizeof time, name, sizeof name))
         snprintf(line, sizeof line, "%s. %s\n%s", disp, time, name);
      else
         snprintf(line, sizeof line, "%s. Empty Slot.", disp);
      // file_picker_ss_window: the slot's saved screenshot (no thumbnail for empty / no-PNG saves).
      if (lsSlot[o].thumbOk) { freeGfxTexture(&lsSlot[o].thumb); lsSlot[o].thumbOk = 0; }
      if (lsSlot[o].occupied) {
         char tp[256]; saveThumbPath(fn, tp, sizeof tp);
         GfxTexture t = loadGfxTexture(tp);
         if (t.w > 0 && t.h > 0) { lsSlot[o].thumb = t; lsSlot[o].thumbOk = 1; }
      }
      // Wrap within the slot interior (slot width minus the text-window x inset each side).
      char hn[16]; snprintf(hn, sizeof hn, "slot_%d", o);
      int hi = imHotspot(lsImap, hn);
      int wrapW = 0;
      if (hi >= 0) { int sw = (int)((lsImap->hotspots[hi].x1 - lsImap->hotspots[hi].x0) * scale + 0.5f);
                  wrapW = sw - 2 * (int)(gui.slotTextX * scale + 0.5f); }
      if (wrapW < 16) wrapW = 16;
      renderFontEx(&lsSlot[o].tex, font, size, line, color, wrapW, TEXT_WRAP, shadow, NULL);
   }
   lsSlotsValid = 1; lsSlotsW = cw; lsSlotsPage = lsPage;
}

static void drawImagemapPicker(int cx, int cy, int cw, int ch)
{
   clearGfx(0xFF000000);
   if (lsTexOk[LS_GROUND])
      drawGfxTexture(cx, cy, cw, ch, lsTex[LS_GROUND], 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
   if (!lsSlotsValid || lsSlotsW != cw || lsSlotsPage != lsPage) lsBuildSlots(cw);

   float nw = gui.nativeW > 0 ? (float)gui.nativeW : 800.0f;
   float scale = (float)cw / nw;
   int register_ = !confirmActive;
   if (register_) beginFocusFrame();

   for (int i = 0; i < lsImap->hotspotCount; i++) {
      const RbcHotspot *h = &lsImap->hotspots[i];
      const char *nm = h->name ? h->name : "";
      int x, y, w, hh; imRect(h, cx, cy, cw, ch, &x, &y, &w, &hh);
      int focused = isFocused(ID_IMHS(i));

      if (strncmp(nm, "slot_", 5) == 0) {
         int o = atoi(nm + 5);
         int selected = (o < lsPerPage) && lsSlot[o].fn[0] && strcmp(lsSlot[o].fn, lsNewest) == 0 && lsNewest[0];
         int st = selected ? (focused ? LS_SEL_HOVER : LS_SEL_IDLE) : (focused ? LS_HOVER : LS_IDLE);
         lsDrawHotspot(h, st, cx, cy, cw, ch);
         if (register_) addFocus(ID_IMHS(i), x, y, w, hh);
         if (o < lsPerPage && o < 16) {
            // file_picker_ss_window: the save screenshot drawn at config.thumbnail_width/height (the
            // size the engine renders the screenshot Displayable), at ss_window xpos/ypos in the slot.
            if (lsSlot[o].thumbOk) {
               int tw = gui.thumbW > 0 ? gui.thumbW : lsSlot[o].thumb.w;
               int th = gui.thumbH > 0 ? gui.thumbH : lsSlot[o].thumb.h;
               drawGfxTexture(x + (int)(gui.slotSsX * scale + 0.5f), y + (int)(gui.slotSsY * scale + 0.5f),
                              (int)(tw * scale + 0.5f), (int)(th * scale + 0.5f),
                              lsSlot[o].thumb, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
            }
            if (lsSlot[o].tex.valid)   // slot text at file_picker_text_window xpos/ypos
               drawTex(&lsSlot[o].tex, x + (int)(gui.slotTextX * scale + 0.5f), y + (int)(gui.slotTextY * scale + 0.5f));
         }
      } else {
         // pager (previous/next/page_*) + nav (Return, ...) buttons: idle, or hover when focused.
         lsDrawHotspot(h, focused ? LS_HOVER : LS_IDLE, cx, cy, cw, ch);
         if (register_) addFocus(ID_IMHS(i), x, y, w, hh);
      }
   }
}

static GmAction updateImagemapPicker(int curVisible, int curX, int curY)
{
   int hoverId = curVisible ? focusAt(curX, curY) : -1;
   if (hoverId >= 0) setFocus(hoverId);
   if (isPadButtonPressed(PAD_BTN_UP))    moveFocus(0, -1);
   if (isPadButtonPressed(PAD_BTN_DOWN))  moveFocus(0,  1);
   if (isPadButtonPressed(PAD_BTN_LEFT))  moveFocus(-1, 0);
   if (isPadButtonPressed(PAD_BTN_RIGHT)) moveFocus(1,  0);

   if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
      if (confirmActive) { confirmActive = 0; setFocus(confirmFromId); return GM_NONE; }
      return GM_RETURN;
   }
   if (!isPadButtonPressed(PAD_BTN_CROSS)) return GM_NONE;
   int id = getFocusId();   // capture BEFORE any setFocus (the yes/no prompt reads this)
   if (confirmActive) { confirmActive = 0; setFocus(confirmFromId); return (id == ID_YES) ? confirmAction : GM_NONE; }
   if (curVisible && hoverId < 0) return GM_NONE;

   if (id < ID_IMHS(0) || id >= ID_IMHS(lsImap->hotspotCount)) return GM_NONE;
   const char *nm = lsImap->hotspots[id - ID_IMHS(0)].name;
   if (!nm) return GM_NONE;

   if (strcmp(nm, "previous") == 0) { if (lsPage > 0) lsPage--; lsSlotsValid = 0; return GM_NONE; }
   if (strcmp(nm, "next")     == 0) { if (lsPage < lsPageCount - 1) lsPage++; lsSlotsValid = 0; return GM_NONE; }
   int pg = lsPageOfName(nm);
   if (pg >= 0) { lsPage = pg; lsSlotsValid = 0; return GM_NONE; }

   if (strncmp(nm, "slot_", 5) == 0) {
      int o = atoi(nm + 5); char fn[12]; int ro;
      lsPageFile(lsPage, o, fn, sizeof fn, &ro);
      snprintf(selectedSlot, sizeof selectedSlot, "%s", fn);
      int occupied = saveSlotExists(fn);
      if (currentScreen == GM_SCREEN_SAVE) {
         if (ro) return GM_NONE;                                  // auto/quick not writable on save
         if (occupied) { askConfirm(GM_DO_SAVE, MSG_OVERWRITE, textBuiltWidth); return GM_NONE; }
         return GM_DO_SAVE;
      }
      if (!occupied) return GM_NONE;                               // nothing to load
      askConfirm(GM_DO_LOAD, MSG_LOAD, textBuiltWidth); return GM_NONE;
   }

   // A config.game_menu nav label (Return, Preferences, ...): use the engine-constant MENU table.
   for (int m = 0; m < (int)(sizeof MENU / sizeof MENU[0]); m++)
      if (strcmp(MENU[m].label, nm) == 0) {
         if (MENU[m].needsInGame && mainMenu) return GM_NONE;
         if (MENU[m].screen != GM_SCREEN_NONE) { currentScreen = MENU[m].screen; lsSlotsValid = 0; return GM_NONE; }
         if (MENU[m].action == GM_MAINMENU) { askConfirm(GM_MAINMENU, MSG_MAIN_MENU, textBuiltWidth); return GM_NONE; }
         if (MENU[m].action == GM_QUIT)     { askConfirm(GM_QUIT, MSG_QUIT, textBuiltWidth); return GM_NONE; }
         return MENU[m].action;
      }
   return GM_NONE;
}

// The engine yes/no prompt (classic_yesno_prompt.rpym), drawn over either layout. Only Yes/No are
// focusable while it's up, so it begins its own focus frame.
static void drawConfirmPrompt(int cx, int cy, int cw, int ch)
{
   if (!confirmTex.valid) return;
   float scale = getGuiScale(cw);
   beginFocusFrame();

   // yesno_frame = menu_frame (RoundRect(frame)) with xfill, xmargin .05, ypos .1, yanchor 0,
   // ypadding .05. A vbox (box_spacing 30, centred) holds the prompt above an hbox (spacing 100) of
   // two equal-width (size_group "yesno") Yes/No buttons (roundrect buttons, xpadding 12/6, ypadding 1).
   int xmar    = (int)(cw * 0.05f + 0.5f);
   int frameX  = cx + xmar, frameW = cw - 2 * xmar;
   int fYpad   = (int)(ch * 0.05f + 0.5f);
   int spV     = (int)(30 * scale + 0.5f);
   int spH     = (int)(100 * scale + 0.5f);
   int bXpad   = (int)(btnXpad() * scale + 0.5f), bYpad = (int)(1 * scale + 0.5f);

   int msgW = confirmTex.tex.w, msgH = confirmTex.tex.h;
   int yesTW = yesTex.tex.w, noTW = noTex.tex.w;
   int ynW = (yesTW > noTW ? yesTW : noTW) + 2 * bXpad;
   int ynH = gmLineH + 2 * bYpad;

   int frameH = (msgH + spV + ynH) + 2 * fYpad;
   int frameY = cy + (int)(ch * 0.1f + 0.5f);
   drawRoundRect(frameX, frameY, frameW, frameH, scale, gui.mmFrameColor);

   int top = frameY + fYpad;
   drawTex(&confirmTex, frameX + (frameW - msgW) / 2, top);

   int hboxY = top + msgH + spV;
   int hboxX = frameX + (frameW - (ynW + spH + ynW)) / 2;
   int yesX = hboxX, noX = hboxX + ynW + spH;
   addFocus(ID_YES, yesX, hboxY, ynW, ynH);
   addFocus(ID_NO,  noX,  hboxY, ynW, ynH);
   drawRoundRect(yesX, hboxY, ynW, ynH, scale, isFocused(ID_YES) ? gui.gmBtnHover : gui.gmBtnIdle);
   drawRoundRect(noX,  hboxY, ynW, ynH, scale, isFocused(ID_NO)  ? gui.gmBtnHover : gui.gmBtnIdle);
   drawBtnLabel(&yesTex, yesX, hboxY, ynW, ynH);
   drawBtnLabel(&noTex,  noX,  hboxY, ynW, ynH);
}

void drawGameMenu(int cx, int cy, int cw, int ch)
{
   // Imagemap file-picker layout (the game provides imagemap_load_save): replace the roundrect
   // nav+slots with the full-screen imagemap. The yes/no prompt still overlays via the block below.
   if (lsActive()) {
      if (!textBuilt || textBuiltWidth != cw) buildText(cw);   // keeps textBuiltWidth for prompts
      drawImagemapPicker(cx, cy, cw, ch);
      if (confirmActive) drawConfirmPrompt(cx, cy, cw, ch);
      return;
   }
   clearGfx(0xFF000000);
   if (gmBgLoaded)
      drawGfxTexture(cx, cy, cw, ch, gmBgTex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
   if (!textBuilt || textBuiltWidth != cw) buildText(cw);
   beginFocusFrame();

   // gm_nav_frame: xpos 5/6, ypos 0.95 (classic_navigation.rpym). Image buttons abut, text gets padding.
   float assetScale = getGuiAssetScale(cw);
   float scale = getGuiScale(cw);
   int xpad = (int)(btnXpad() * scale + 0.5f), ypad = (int)(6 * scale + 0.5f);

   int btnW[GM_ITEM_MAX], btnH[GM_ITEM_MAX], totalH = 0, maxBtnW = 0;
   for (int i = 0; i < navCount; i++)
   {
      if (nav[i].hasIdle) { btnW[i] = (int)(nav[i].idle.w * assetScale + 0.5f); btnH[i] = (int)(nav[i].idle.h * assetScale + 0.5f); }
      // A text button (style.button, no yfill/yminimum) sizes to its CHILD's LINE BOX + 2*padding
      // (00themes.rpy roundrect_buttons: ypadding 1) -- gmLineH is the line pitch, not the font
      // point size (which is shorter than the glyph box, so a centred label overflowed).
      else                { btnW[i] = nav[i].text.tex.w + 2 * xpad; btnH[i] = gmLineH + 2 * ypad; }
      if (btnW[i] > maxBtnW) maxBtnW = btnW[i];
      totalH += btnH[i];
   }
   // gm_nav_frame = Style(menu_frame) -> RoundRect(frame), with style.frame xpadding/ypadding = 6.
   // This game KEEPS it (it only re-parented mm_menu_frame, for the MAIN menu), so the engine draws
   // a flat-blue rounded box behind the nav column, bottom-anchored at xpos 5/6, ypos 0.95.
   int navPad      = (int)(6 * scale + 0.5f);
   int colCenterX  = cx + (int)(cw * (5.0f / 6.0f) + 0.5f);    // xpos 5/6, xanchor 0.5
   int frameBottom = cy + (int)(ch * 0.95f + 0.5f);            // ypos 0.95, yanchor 1.0
   int navFrameW = maxBtnW + 2 * navPad, navFrameH = totalH + 2 * navPad;
   int navFrameX = colCenterX - navFrameW / 2, navFrameY = frameBottom - navFrameH;
   drawRoundRect(navFrameX, navFrameY, navFrameW, navFrameH, scale, gui.mmFrameColor);
   int rowY = navFrameY + navPad;
   for (int i = 0; i < navCount; i++)
   {
      int btnX = colCenterX - btnW[i] / 2;
      if (!confirmActive) addFocus(ID_NAV(i), btnX, rowY, btnW[i], btnH[i]);
      int focused = isFocused(ID_NAV(i));
      if (nav[i].hasIdle)
      {
         const GfxTexture *art = buttonArt(&nav[i], focused);
         drawGfxTexture(btnX, rowY, btnW[i], btnH[i], *art, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
      }
      else
      {
         uint32_t tint = (!buttonEnabled(&nav[i]) || buttonSelected(&nav[i])) ? gui.gmBtnSelected
                    : focused ? gui.gmBtnHover : gui.gmBtnIdle;
         drawRoundRect(btnX, rowY, btnW[i], btnH[i], scale, tint);
         drawBtnLabel(&nav[i].text, btnX, rowY, btnW[i], btnH[i]);
      }
      rowY += btnH[i];
   }

   // The yes/no prompt is its own interaction: it draws the nav (above) + its frame, NOT the file
   // picker, and does not dim. So the picker only shows when we're not prompting.
   if (!confirmActive)
   {
      if (currentScreen == GM_SCREEN_SAVE || currentScreen == GM_SCREEN_LOAD) drawSlots(cx, cy, cw, ch, 1);
      return;
   }
   drawConfirmPrompt(cx, cy, cw, ch);
}

// Open the yes/no prompt for an action the engine gates (Main Menu / Quit).
static void askConfirm(GmAction action, const char *message, int cw)
{
   confirmActive  = 1;
   confirmAction  = action;
   confirmFromId  = getFocusId();
   setFocus(ID_NO);   // default to No, like the engine prompt
   TextShadow sb; const TextShadow *shadow = menuShadow(cw, &sb);
   // prompt_text wraps within the yesno_frame interior (xmargin .05 each side, frame xpadding 6),
   // and is CENTRED per line (roundrect_prompts: prompt_text.text_align 0.5, layout "subtitle").
   int wrapW = (int)(cw * 0.90f) - 2 * (int)(6 * getGuiScale(cw) + 0.5f);
   renderFontAligned(&confirmTex, font, getGuiGmTextSize(cw), message, gui.gmBtnText, wrapW, TEXT_WRAP, shadow, NULL, TEXT_ALIGN_CENTER);
}

GmAction updateGameMenu(int curVisible, int curX, int curY)
{
   // Imagemap load/save layout: its own hotspot dispatch (slots/paging/nav by name).
   if (lsActive()) return updateImagemapPicker(curVisible, curX, curY);

   // Cursor hover: focus the widget under the pointer (Ren'Py mouse focus = point-in-rect). Only the
   // active surface registers focus rects each frame -- nav column + file-picker slots normally, or
   // just the Yes/No buttons while the prompt is up -- so focusAt naturally hits only what's clickable
   // now. Hovering a slot also previews its thumbnail (drawSlots keys off the focused slot). The rects
   // are one frame old (registered in drawGameMenu); the lag is invisible.
   int hoverId = curVisible ? focusAt(curX, curY) : -1;
   if (hoverId >= 0) setFocus(hoverId);

   // d-pad = Ren'Py's geometric focus navigation over whatever's drawn (nav column / slots / prompt).
   if (isPadButtonPressed(PAD_BTN_UP))    moveFocus(0, -1);
   if (isPadButtonPressed(PAD_BTN_DOWN))  moveFocus(0,  1);
   if (isPadButtonPressed(PAD_BTN_LEFT))  moveFocus(-1, 0);
   if (isPadButtonPressed(PAD_BTN_RIGHT)) moveFocus(1,  0);

   if (isPadButtonPressed(PAD_BTN_CIRCLE))   // dismiss
   {
      if (confirmActive) { confirmActive = 0; setFocus(confirmFromId); return GM_NONE; }
      return GM_RETURN;
   }

   if (isPadButtonPressed(PAD_BTN_CROSS))     // activate the focused widget
   {
      if (curVisible && hoverId < 0) return GM_NONE;   // click over empty space = nothing (like a mouse)
      int id = getFocusId();
      if (confirmActive)
      {
         confirmActive = 0;
         setFocus(confirmFromId);
         return (id == ID_YES) ? confirmAction : GM_NONE;
      }
      if (id >= 0 && id < navCount)
      {
         NavButton *button = &nav[id];
         if (!buttonEnabled(button)) return GM_NONE;                          // disabled
         if (button->screen != GM_SCREEN_NONE) { currentScreen = button->screen; return GM_NONE; }   // show that sub-screen
         if (button->action == GM_MAINMENU) { askConfirm(GM_MAINMENU, MSG_MAIN_MENU, textBuiltWidth); return GM_NONE; }
         if (button->action == GM_QUIT)     { askConfirm(GM_QUIT, MSG_QUIT, textBuiltWidth); return GM_NONE; }
         return button->action;   // Return
      }
      if (id >= ID_SLOT(0) && id < ID_SLOT(slotCount))   // a file-picker slot
      {
         int idx = id - ID_SLOT(0);
         snprintf(selectedSlot, sizeof selectedSlot, "%s", slot[idx].display);
         int occupied = saveSlotExists(selectedSlot);
         if (currentScreen == GM_SCREEN_SAVE)
         {
            if (selectedSlot[0] == 'a') return GM_NONE;   // auto slots aren't writable on the save screen
            if (occupied) { askConfirm(GM_DO_SAVE, MSG_OVERWRITE, textBuiltWidth); return GM_NONE; }
            return GM_DO_SAVE;                       // empty slot: save straight away
         }
         if (currentScreen == GM_SCREEN_LOAD)
         {
            if (!occupied) return GM_NONE;           // nothing to load from an empty slot
            askConfirm(GM_DO_LOAD, MSG_LOAD, textBuiltWidth); return GM_NONE;
         }
      }
      return GM_NONE;
   }
   return GM_NONE;
}
