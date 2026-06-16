#pragma once

// GUI manifest (game.gui) + textbox/CTC geometry and drawing.
//
// The converter bakes a normalized, game-agnostic GUI descriptor into every .rpk; this
// module reads it and renders the textbox/text/choice styling generically (no per-game
// code). All manifest values are in the game's NATIVE pixel space; we scale them into the
// letterboxed content rect. Missing keys fall back to Ren'Py ENGINE defaults (never to
// invented values), so a bundle without a manifest still renders like a classic game.

#include <stdint.h>
#include "gfx.h"
#include "font.h"   // TextShadow

#define GUI_CHARCOL_MAX 64
#define GUI_MMBTN_MAX   8
#define GUI_MENU_MAX    12   // max entries in config.main_menu / config.game_menu (with custom inserts)
#define GUI_SIDEIMG_MAX 6        // distinct characters with a show_side_image=ConditionSwitch
#define GUI_SIDEIMG_COND_MAX 16  // ConditionSwitch branches per such character
#define GUI_TWOWIN_MAX  16       // speakers using the separate say_who_window namebox

// One character's side image: ConditionSwitch(cond,img,...). The player evaluates exprId[] in order
// (first true wins) against the live vars and draws img[] at align within the content rect, while that
// character speaks. exprId references the .rbc expr section (same eval as if/menu/overlay guards).
typedef struct {
    char  name[48];                          // speaker DISPLAY name (matched to the current speaker)
    float alignX, alignY;                    // ConditionSwitch xalign/yalign (e.g. 0 / 1.0 = bottom-left)
    int   count;
    int   exprId[GUI_SIDEIMG_COND_MAX];      // .rbc expr id for each branch's condition
    char  img[GUI_SIDEIMG_COND_MAX][48];     // each branch's image basename
} GuiSideImage;

typedef struct {
    int      nativeW, nativeH;     // game design resolution (0 => unknown)
    float    assetScale;           // converter pre-scale of bundled assets (1.0 = native size)
    int      textSize, nameSize;   // native px (0 => engine default)
    uint32_t textColor;
    uint32_t nameColor;
    int      shadowDx, shadowDy;   // style.default.drop_shadow, native px (0,0 => none)
    uint32_t shadowColor;          // style.default.drop_shadow_color (default black)
    int      textAdvanceCeil;      // text_advance=ceil: pre-6.12 SDL_ttf metrics (whole-px
                                   // advances + kerning) -- wrap points match the original
    int      textCps;              // config.default_text_cps typewriter speed (0 = instant)
    char     dlgFont[40];          // dialogue font basename (converter-resolved text_font; "" = none)
    uint32_t choiceColor, choiceHoverColor;
    // in-game choice menu (style.menu_choice_button / menu_choice / menu_window): the button is a 9-slice
    // Frame (choiceTex/hoverTex) sized to xminimum x yminimum, text at menu_choice.size, the vbox placed at
    // menu_window.yalign. native px; 0 => engine/fallback default.
    int      choiceFrameX, choiceFrameY;   // menu_choice_button Frame() insets (0 => natural/stretch)
    int      choiceXmin, choiceYmin;       // menu_choice_button xminimum / yminimum
    int      choiceSize;                   // menu_choice.size (0 => dialogue size)
    int      choiceMargin;                 // menu_choice_button top_margin (vbox spacing)
    float    menuYalign;                   // menu_window.yalign (default 0.5 = centred)
    uint32_t textboxColor;         // solid window bg, used when no frame image is bundled
    int      frameInsetX, frameInsetY;   // Frame() 9-slice insets, native px (0 => stretch)
    int      textboxH;             // explicit textbox height (gui.* games), native px
    int      marginB, marginT, xmargin;  // style.window margins, native px
    int      padX, padY;           // style.window xpadding/ypadding, native px (symmetric fallback)
    int      padL, padR, padT, padB;  // per-edge style.window paddings, native px (-1 => use padX/padY)
    int      ymin;                 // style.window.yminimum (incl. margins+padding), native px
    int      nameSpacing;          // style.say_vbox.spacing
    int      nameBold;             // style.say_label.bold
    int      nvlPadX, nvlPadY, nvlSpacing;
    uint32_t nvlBg;
    int      ctcFixed;             // ctc_position == "fixed" (else nestled/inline)
    int      ctcXpos, ctcYpos, ctcXanchor, ctcYanchor;   // fixed ctc position, native px

    GfxTexture frameTex;  int frameLoaded;
    GfxTexture choiceTex; int choiceLoaded;
    GfxTexture hoverTex;  int hoverLoaded;
    GfxTexture ctcTex;    int ctcLoaded;

    // In-game ("MMO chat") textbox, shared by Character(window_background=Image(..)) speakers:
    // a fixed-size box image bottom-left anchored, with its own paddings + small `what` font/size.
    // Distinct from the main ADV textbox above. (native px; igFont basename.)
    GfxTexture igTex;     int igLoaded;
    int        igPadL, igPadR, igPadB, igPadT;
    char       igFont[40];
    int        igSize;
    float      igAlignX, igAlignY;     // window_background Image xalign/yalign (default 0.0 / 1.0)
    int        igShadowDx, igShadowDy; // what_drop_shadow (native px; 0,0 = none)
    uint32_t   igShadowColor;          // what_drop_shadow_color

    char     charName[GUI_CHARCOL_MAX][48];   // per-speaker name colours
    uint32_t charCol[GUI_CHARCOL_MAX];
    int      charColCount;

    // Classic-theme main menu (game.gui mm_bg / mm_btn.<Label>=idle|hover). The player
    // renders these in canonical config.main_menu order with engine-constant actions, so
    // any classic-theme game's own title art + buttons work with no per-game code.
    char     mmBg[64];                        // title-screen background image (mm_root)
    char     mmMusic[96];                     // config.main_menu_music (played while the menu shows)
    char     gmBg[64];                        // in-game menu background image (gm_root)
    uint32_t mmFrameColor;                    // mm_menu_frame bg = RoundRect(frame); theme
                                              // roundrect default (100,150,200,255)
    int      mmFrameNone;                     // 1 = the game re-parented mm_menu_frame to
                                              // style.default (no frame box / no padding)
    // theme.roundrect button colours (in-game menu nav + any classic roundrect text button): the
    // rr template tinted by `widget` (idle) / `widget_hover`, with `widget_text` / `widget_selected`
    // text. Defaults are the engine's (00themes.rpy). gmTextSize 0 => engine default by native res.
    uint32_t gmBtnIdle, gmBtnHover, gmBtnText, gmBtnSelected;
    uint32_t gmBtnDisabled, gmBtnDisabledText;   // `disabled` / `disabled_text`: an insensitive button's
                                                 // RoundRect(disabled) bg + large_button_text.insensitive_color
    int      gmTextSize;
    int      slotTextSize;   // style.file_picker_text.size (0 => engine small_text_size by res)
    // imagemap_load_save slot content placement (native px, within the slot hotspot rect) + text colour.
    // slotTextColor 0 => not overridden, use large_button_text (gmBtnText). slotSs* position the thumbnail.
    int      slotTextX, slotTextY, slotSsX, slotSsY;
    uint32_t slotTextColor;
    int      thumbW, thumbH;   // config.thumbnail_width/height: the size the file picker draws the save screenshot
    char     mmBtnLabel[GUI_MMBTN_MAX][24];   // button label, e.g. "Start Game"
    char     mmBtnIdle[GUI_MMBTN_MAX][40];    // idle image
    char     mmBtnHover[GUI_MMBTN_MAX][40];   // hover image ("" if none)
    char     mmBtnSelected[GUI_MMBTN_MAX][40];    // selected_idle image (current screen) ("" if none)
    char     mmBtnInsensitive[GUI_MMBTN_MAX][40]; // insensitive image (disabled) ("" if none)
    int      mmBtnCount;

    // config.main_menu / config.game_menu MEMBERSHIP + ORDER (engine default + the game's mutations,
    // computed by the converter -> mm_order/gm_order). The player renders these labels in order, using
    // mmBtn.<label> art + an engine-constant label->action map. Empty => fall back to engine defaults.
    char     mmOrder[GUI_MENU_MAX][24]; int mmOrderCount;
    char     gmOrder[GUI_MENU_MAX][24]; int gmOrderCount;

    // Per-character SIDE IMAGES (show_side_image=ConditionSwitch). The player draws the matching
    // portrait beside the textbox while that character speaks. Textures are lazy-loaded at draw time.
    GuiSideImage sideImg[GUI_SIDEIMG_MAX]; int sideImgCount;

    // show_two_window NAMEBOX (say_who_window): speakers in twoWin[] put their name in a separate box
    // (whoTex bg at who x/ypos with anchors + paddings), not inline in the dialogue box.
    char       twoWin[GUI_TWOWIN_MAX][48]; int twoWinCount;
    char       whoBg[48];                         // say_who_window.background basename ("" => none)
    int        whoXpos, whoYpos, whoLpad, whoTpad;
    float      whoXanchor, whoYanchor;
    GfxTexture whoTex; int whoLoaded;
} Gui;

extern Gui gui;

// Reads game.gui + the GUI images it references from the bundle. Best-effort: anything
// missing falls back to engine defaults. Call once at load; freeGui releases textures.
void loadGui(const char *rpkPath);
void freeGui(void);

// Side image for the given speaker (NULL if that speaker has none). Returned struct holds the
// ConditionSwitch branches; the caller evaluates them against the live program/vars.
const GuiSideImage *getGuiSideImage(const char *who);
// 1 if `who` is a show_two_window speaker (name belongs in the separate say_who_window namebox).
int isGuiTwoWindow(const char *who);

// Native px -> screen px (the content rect width over the game's design width; 1.0 if unknown).
float getGuiScale(int cw);

// Bundled-asset px -> screen px (= getGuiScale / assetScale; differs from getGuiScale only when
// the converter pre-scaled the assets).
float getGuiAssetScale(int cw);

// Scaled dialogue / speaker-name text size (screen px).
int getGuiDlgSize(int cw);
int getGuiGmTextSize(int cw);
int getGuiSlotTextSize(int cw);
int getGuiNameSize(int cw);

// Name colour for a speaker (per-character override, else the default name colour).
// Per-character colours come from Character(color=) == who_color, a property of the
// CHARACTER OBJECT -- literal string speakers ("????" "...") use the plain say_label
// style, so callers must NOT apply this to quoted-literal speakers.
uint32_t getGuiNameColor(const char *who);

// The game's text drop shadow (style.default.drop_shadow), scaled to screen px.
// Returns 1 and fills *out when the game sets one; 0 = no shadow (out untouched).
// style.default is inherited by dialogue, names, NVL text and menu choices alike.
int getGuiTextShadow(int cw, TextShadow *out);

// The textbox border box, from the game's style.window box model: full-width, inset by
// xmargin, bottom-anchored (style.window yalign 1.0), yminimum tall -- GROWING when the
// say content is taller (Window.render: height = max(margins+padding+child, yminimum)).
// contentH = measured height of the say content in SCREEN px (0 = none/unknown).
void getGuiTextboxRect(int cx, int cy, int cw, int ch, int contentH, int *bx, int *by, int *bw, int *bh);

// Text origin + wrap width inside the textbox (style.window xpadding/ypadding).
void getGuiTextboxTextArea(int cx, int cy, int cw, int ch, int contentH, int *tx, int *ty, int *tw);

// The in-game chat WINDOW border rect, computed from the engine's Window box model (renpy Window.render):
// height = max(margins + top/bottom padding + contentH, yminimum); xfill -> full content width inset by
// xmargin; bottom-anchored (style.window yalign 1.0). contentH = measured text height in screen px.
// All inputs are manifest values (ig paddings + the inherited window yminimum/margins) -- no engine at
// runtime. Returns 1 (always, even with no box image, so the text still places correctly).
int  getGuiIngameWindow(int cx, int cy, int cw, int ch, int contentH, int *wx, int *wy, int *ww, int *wh);
// Text X origin + wrap width inside the in-game window (left/right padding). Y = window top + top padding,
// computed by the caller via getGuiIngameWindow with the measured contentH.
void getGuiIngameTextArea(int cx, int cy, int cw, int ch, int *tx, int *tw);
int  getGuiIngamePadT(int cw);   // window_top_padding in screen px
// Scaled in-game `what` text size (screen px).
int  getGuiIngameSize(int cw);
// The in-game chat text's drop shadow (what_drop_shadow). Returns 1 + fills *out if set, else 0.
int  getGuiIngameShadow(int cw, TextShadow *out);
// Draws the in-game box image at the given (natural-size) rect.
void drawGuiIngameBox(int bx, int by, int bw, int bh, int cw);

// Draws the textbox background: the game's Frame() image as a scaled 9-slice, else the
// game/engine solid window colour.
void drawGuiTextbox(int bx, int by, int bw, int bh, int cw);

// CTC blink: restart the anim.Blink clock (call when a new line is shown), then draw.
// The icon's alpha follows the exact anim.Blink curve (0.5s on / 0.5s fade-out / 0.5s
// off / 0.5s fade-in), on the real-time clock.
void restartGuiCtc(void);
void drawGuiCtcAt(int x, int y, int cw);                          // top-left at x,y
void drawGuiCtcFixed(int cx, int cy, int cw);                     // at the icon's own xpos/ypos

// Nestled ctc: inline, directly after the text's final glyph with the icon's base on
// that glyph's bottom (the engine appends the icon to the text). textX/textY = where the
// text texture is drawn; end = the layout report from renderFontEx.
void drawGuiCtcInline(int textX, int textY, const TextEnd *end, int cw);
