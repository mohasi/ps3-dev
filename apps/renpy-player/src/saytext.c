#include "saytext.h"

#include <string.h>   // strlen, memmove

#include "gfx.h"
#include "colors.h"
#include "printf.h"   // snprintf
#include "dbg.h"      // logInfo
#include "ui/slice.h" // NineSlice (choice-button Frame 9-slice)
#include "gui.h"
#include "vars.h"     // interpolate
#include "config.h"   // MAX_CHOICES
#include "typewriter.h"

#define HINT_GRAY 0xFFA0A0A0   // engine UI only (navigation hints), not a game/Ren'Py value

static Font *font;     // opened and owned by the screen; injected via initSay
static Font *igFont;   // in-game chat font; injected via initSayIngame (NULL => use `font`)

// The controller-hint line + end/error screen are port-added chrome (no Ren'Py source). They were
// authored at the 800x600 native scale, so scale them with the UI to stay proportional at any output
// resolution (a native px -> drawn px, same factor as the game text).
static int hpx(int cw, int nativePx) { int v = (int)(nativePx * getGuiScale(cw) + 0.5f); return v < 1 ? 1 : v; }

static TextTexture nameTex, textTex, hintTex;
static TextEnd nameEnd;   // line metrics of the rendered name (lineHeight/lineTop) for faithful namebox seating
static TextTexture menuItemTex[MAX_CHOICES];   // one per in-game choice (styled per manifest)

// NVL page: lines from kind=nvl characters stack on a full-screen page. Strings are stable rbc
// pointers. One rendered block per entry; the last one types, the earlier ones are complete.
static const char *nvlWho[NVL_MAX];
static const char *nvlWhat[NVL_MAX];
static int         nvlCount;
static TextTexture nvlTex[NVL_MAX];
static int         currentIsNvl;     // is the current line an NVL page? (drives drawSayLine)
static int         currentIsIngame;  // is the current line an in-game chat-box line? (drives drawSayLine)
static int         currentTwoWin;    // speaker uses show_two_window: name goes in the say_who_window namebox

// Where the dialogue / last NVL entry's final glyph landed (renderFontEx layout report); drives
// the inline ("nestled") ctc placement.
static TextEnd sayEnd, nvlEnd;

// The current line is rendered once (renderFontTyped) and revealed by clipping at draw time;
// typingTex is the texture being revealed (ADV dialogue or the last NVL block).
static Typewriter   typewriter;
static TextTexture *typingTex;

void initSay(Font *sharedFont) { font = sharedFont; }
void initSayIngame(Font *f) { igFont = f; }

static int currentIsBlank;   // a no-dialogue frame (e.g. a rolled-back renpy.pause): draw no textbox
void showSayBlank(void) { currentIsBlank = 1; currentIsNvl = 0; currentIsIngame = 0; }

// ---- helpers ----

static void drawTex(const TextTexture *tex, int x, int y)
{
   if (tex->valid)
      drawGfxTexture(x, y, tex->tex.w, tex->tex.h, tex->tex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
}

// Removes Ren'Py text tags ({i}, {b}, {color=..}, {w}, ...) and unescapes {{ / [[. Used where
// styled rendering isn't wanted (names for colour lookup, choice captions).
static void stripTags(const char *text, char *out, int cap)
{
   int j = 0;
   if (!text) { out[0] = '\0'; return; }
   for (const char *p = text; *p && j < cap - 1; )
   {
      if (p[0] == '{' && p[1] == '{') { out[j++] = '{'; p += 2; }
      else if (p[0] == '{') { p++; while (*p && *p != '}') p++; if (*p == '}') p++; }
      else if (p[0] == '[' && p[1] == '[') { out[j++] = '['; p += 2; }
      else out[j++] = *p++;
   }
   out[j] = '\0';
}

// One-off literal speakers store who as a quoted expression, e.g.  "????" "Eh?"  -> the who string
// is literally "????" (with quotes). Strip a single layer of matching quotes.
static void unquote(char *text)
{
   int n = (int)strlen(text);
   if (n >= 2 && ((text[0] == '"' && text[n - 1] == '"') || (text[0] == '\'' && text[n - 1] == '\'')))
   {
      memmove(text, text + 1, n - 2);
      text[n - 2] = '\0';
   }
}

// The game's text metrics (gui.textAdvanceCeil: pre-6.12 SDL_ttf whole-pixel advances + kerning,
// composed at native resolution then scaled; else modern fractional advances). Applied before
// every game-text render; the grid depends on the content width.
static void applyFontMetricsTo(Font *f, int cw)
{
   if (gui.textAdvanceCeil) setFontMetrics(f, FONT_METRICS_GRID_CEIL, getGuiScale(cw));
   else                     setFontMetrics(f, FONT_METRICS_NATIVE, 0.0f);
}

static void applyFontMetrics(int cw) { applyFontMetricsTo(font, cw); }

// Render `text` into `tex` (with its per-char reveal map) and start the typewriter. typing = type
// it out; 0 reveals it whole immediately. `f` is the font to use (the in-game lines use igFont).
static void typeLineFont(Font *f, TextTexture *tex, const char *text, int size, uint32_t color, int width,
                         int typing, const TextShadow *shadow, TextEnd *end)
{
   renderFontTyped(tex, f, size, text, color, width, TEXT_WRAP, shadow, end, &typewriter.reveal);
   startTypewriter(&typewriter, gui.textCps, !typing);
   typingTex = tex;
}

static void typeLine(TextTexture *tex, const char *text, int size, uint32_t color, int width,
                     int typing, const TextShadow *shadow, TextEnd *end)
{
   typeLineFont(font, tex, text, size, color, width, typing, shadow, end);
}

static const TextShadow *gameTextShadow(int cw, TextShadow *out)
{
   return getGuiTextShadow(cw, out) ? out : NULL;   // style.default.drop_shadow (all game text)
}

// ---- building textures ----

// Render the accumulated NVL page; only the last (current) block types out.
static void renderNvlPage(int cw, int typing, const TextShadow *shadow)
{
   int padX  = (int)(gui.nvlPadX * getGuiScale(cw) + 0.5f);
   int wrapW = cw - 2 * padX; if (wrapW < 100) wrapW = 100;
   for (int i = 0; i < nvlCount; i++)
   {
      char body[2048], line[2200];
      interpolate(nvlWhat[i] ? nvlWhat[i] : "", body, sizeof body);
      if (nvlWho[i])
      {
         char name[128];
         interpolate(nvlWho[i], name, sizeof name);
         snprintf(line, sizeof line, "%s  %s", name, body);
      }
      else snprintf(line, sizeof line, "%s", body);
      if (i == nvlCount - 1) typeLine(&nvlTex[i], line, getGuiDlgSize(cw), gui.textColor, wrapW, typing, shadow, &nvlEnd);
      else                   renderFontEx(&nvlTex[i], font, getGuiDlgSize(cw), line, gui.textColor, wrapW, TEXT_WRAP, shadow, NULL);
   }
   for (int i = nvlCount; i < NVL_MAX; i++) freeTextTexture(&nvlTex[i]);   // drop stale blocks
}

// Render the ADV name + dialogue (the classic textbox line).
static void renderAdvLine(int cx, int cy, int cw, int ch, const char *who, const char *what, int typing, const TextShadow *shadow)
{
   static char interpWho[256], whoBuf[256], interpWhat[2048];
   interpolate(who, interpWho, sizeof interpWho);
   stripTags(interpWho, whoBuf, sizeof whoBuf);
   // A who still QUOTED here is a literal speaker ("????" "..."): the converter leaves the quotes;
   // resolved Character names arrive bare. Literals use the plain say_label style, so the
   // character's who_color must NOT apply to one that merely matches a display name.
   int literalWho = (whoBuf[0] == '"' || whoBuf[0] == '\'');
   unquote(whoBuf);
   // show_two_window: this speaker's name belongs in the separate say_who_window namebox, not inline
   // in the dialogue box (classic two-window say). Literal quoted speakers never use it.
   currentTwoWin = !literalWho && isGuiTwoWindow(whoBuf);
   interpolate(what ? what : "", interpWhat, sizeof interpWhat);

   // Wrap to the exact text area (style.window padding); the width doesn't depend on box height.
   int tx, ty, tw; getGuiTextboxTextArea(cx, cy, cw, ch, 0, &tx, &ty, &tw);
   // style.say_label.bold -> wrap the name in {b} so the rasterizer's synthetic bold applies.
   char nameOut[300];
   if (gui.nameBold && whoBuf[0]) snprintf(nameOut, sizeof nameOut, "{b}%s{/b}", whoBuf);
   else snprintf(nameOut, sizeof nameOut, "%s", whoBuf);
   uint32_t nameCol = literalWho ? gui.nameColor : getGuiNameColor(whoBuf);
   renderFontEx(&nameTex, font, getGuiNameSize(cw), nameOut, nameCol, tw, TEXT_NOWRAP_ELLIPSIS, shadow, &nameEnd);
   typeLine(&textTex, interpWhat, getGuiDlgSize(cw), gui.textColor, tw, typing, shadow, &sayEnd);
}

// Render an in-game ("MMO chat") line: no name (Character(None); the speaker label is baked into
// the text via what_prefix), small `what` font/size, wrapped to the chat box's interior width.
static void renderIngameLine(int cx, int cy, int cw, const char *what, int typing, const TextShadow *shadow)
{
   (void)shadow;   // chat uses the CHARACTER's what_drop_shadow (#000 here -> ~invisible on the dark
               // box), NOT the game's global style.default shadow (which is white -> the old halo).
   TextShadow igShadowBuf;
   const TextShadow *igShadow = getGuiIngameShadow(cw, &igShadowBuf) ? &igShadowBuf : NULL;
   Font *f = igFont ? igFont : font;
   applyFontMetricsTo(f, cw);
   static char interpWhat[2048];
   interpolate(what ? what : "", interpWhat, sizeof interpWhat);
   int tx, tw; getGuiIngameTextArea(cx, cy, cw, 0, &tx, &tw);
   nameTex.valid = 0;   // chat lines have no separate speaker label
   typeLineFont(f, &textTex, interpWhat, getGuiIngameSize(cw), gui.textColor, tw, typing, igShadow, &sayEnd);
}

void showSayLine(int cx, int cy, int cw, int ch, const char *who, const char *what, int nvl, int ingame, int freshPage, int typing)
{
   applyFontMetrics(cw);
   TextShadow shadowBuf;
   const TextShadow *shadow = gameTextShadow(cw, &shadowBuf);
   currentIsNvl = nvl;
   currentIsIngame = ingame && !nvl;
   currentIsBlank = 0;
   currentTwoWin = 0;   // only an ADV named line (renderAdvLine) turns this on
   if (currentIsIngame) { renderIngameLine(cx, cy, cw, what, typing, shadow); return; }
   if (!nvl) { renderAdvLine(cx, cy, cw, ch, who, what, typing, shadow); return; }

   if (freshPage) nvlCount = 0;
   if (nvlCount >= NVL_MAX)   // page full: scroll the oldest line off
   {
      for (int i = 1; i < NVL_MAX; i++) { nvlWho[i-1] = nvlWho[i]; nvlWhat[i-1] = nvlWhat[i]; }
      nvlCount = NVL_MAX - 1;
   }
   nvlWho[nvlCount] = who; nvlWhat[nvlCount] = what; nvlCount++;
   renderNvlPage(cw, typing, shadow);
}

void showSayNvlPage(int cw, const char *const *who, const char *const *what, int count, int typing)
{
   applyFontMetrics(cw);
   TextShadow shadowBuf;
   const TextShadow *shadow = gameTextShadow(cw, &shadowBuf);
   currentIsNvl = 1;
   currentIsIngame = 0;
   nvlCount = count > NVL_MAX ? NVL_MAX : count;
   for (int i = 0; i < nvlCount; i++) { nvlWho[i] = who[i]; nvlWhat[i] = what[i]; }
   renderNvlPage(cw, typing, shadow);
}

void showSayMenu(int cw, const char *const *captions, int count, int selected)
{
   applyFontMetrics(cw);
   float s = getGuiScale(cw);
   int size = gui.choiceSize > 0 ? (int)(gui.choiceSize * s + 0.5f) : getGuiDlgSize(cw);   // menu_choice.size
   // Caption wraps to the button interior = the button width (menu_choice_button.xminimum) less padding.
   int btnW = gui.choiceXmin > 0 ? (int)(gui.choiceXmin * s + 0.5f) : (int)(cw * 0.5f);
   if (btnW > cw) btnW = cw;
   int maxW = btnW - 2 * (int)(12 * s + 0.5f);
   if (maxW < 100) maxW = 100;
   TextShadow shadowBuf;
   const TextShadow *shadow = gameTextShadow(cw, &shadowBuf);   // menu_choice inherits style.default
   char interp[512], caption[512];
   for (int i = 0; i < count; i++)
   {
      interpolate(captions[i], interp, sizeof interp);
      stripTags(interp, caption, sizeof caption);
      uint32_t color = (i == selected) ? gui.choiceHoverColor : gui.choiceColor;
      renderFontEx(&menuItemTex[i], font, size, caption, color, maxW, TEXT_WRAP, shadow, NULL);
   }
}

void showSayEnd(int cw, const char *message)
{
   int textW = cw - hpx(cw, 180); if (textW < 100) textW = 100;
   renderFont(&textTex, font, 30, message, COLOR_WHITE, textW, TEXT_WRAP);
   renderFont(&hintTex, font, hpx(cw, 20), "O: back", HINT_GRAY, hpx(cw, 300), TEXT_NOWRAP);
}

void clearSayNvl(void) { nvlCount = 0; currentIsNvl = 0; }

// ---- typewriter ----

void tickSay(void)           { tickTypewriter(&typewriter); }
int  isSayTypingDone(void)     { return isTypewriterDone(&typewriter); }
void completeSayTyping(void) { completeTypewriter(&typewriter); }

// ---- drawing ----

void drawSayLine(int cx, int cy, int cw, int ch)
{
   if (currentIsBlank) return;   // no-dialogue frame (e.g. a rolled-back pause): scene only, no box
   if (currentIsIngame)
   {
      // In-game chat: emulate the engine's Window layout. The window is bottom-anchored and
      // >= yminimum tall; the background image is placed within it by its own xalign/yalign; the
      // text sits at (left_padding, top_padding) from the window top-left -- all engine-derived,
      // no per-game fudge.
      int th = textTex.valid ? textTex.tex.h : 0;
      int wx, wy, ww, wh; getGuiIngameWindow(cx, cy, cw, ch, th, &wx, &wy, &ww, &wh);
      if (gui.igLoaded)
      {
         float as = getGuiAssetScale(cw);
         int iw = (int)(gui.igTex.w * as + 0.5f), ih = (int)(gui.igTex.h * as + 0.5f);
         int ix = wx + (int)((ww - iw) * gui.igAlignX + 0.5f);   // background place() by xalign/yalign
         int iy = wy + (int)((wh - ih) * gui.igAlignY + 0.5f);
         drawGuiIngameBox(ix, iy, iw, ih, cw);
      }
      float s = getGuiScale(cw);
      int tx = wx + (int)(gui.igPadL * s + 0.5f);   // child placed at left_padding / top_padding
      int ty = wy + getGuiIngamePadT(cw);
      drawTypewriter(&typewriter, &textTex, tx, ty);
      if (isSayTypingDone())
      {
         if (gui.ctcFixed) drawGuiCtcFixed(cx, cy, cw);
         else              drawGuiCtcInline(tx, ty, &sayEnd, cw);
      }
      return;
   }
   if (currentIsNvl)
   {
      // NVL page: nvl_window bg (#0008), entries stacked from top-left with nvl_vbox box_spacing;
      // ctc is "nestled" inline right after the last entry's text.
      float scale = getGuiScale(cw);
      fillGfxRectangle(cx, cy, cw, ch, gui.nvlBg);
      int x = cx + (int)(gui.nvlPadX * scale + 0.5f);
      int y = cy + (int)(gui.nvlPadY * scale + 0.5f);
      int spacing = (int)(gui.nvlSpacing * scale + 0.5f);
      int lastX = x, lastY = y, haveLast = 0;
      for (int i = 0; i < nvlCount; i++)
      {
         if (!nvlTex[i].valid) continue;
         if (&nvlTex[i] == typingTex) drawTypewriter(&typewriter, &nvlTex[i], x, y);   // current block types
         else                         drawTex(&nvlTex[i], x, y);
         lastX = x; lastY = y; haveLast = 1;
         y += nvlTex[i].tex.h + spacing;
      }
      if (haveLast && isSayTypingDone()) drawGuiCtcInline(lastX, lastY, &nvlEnd, cw);   // ctc only once typed out
      return;
   }

   // show_two_window: the name is drawn in a SEPARATE say_who_window namebox (above/left of the box),
   // so the dialogue box reserves NO name height -- only the dialogue. The namebox is the whoTex bg
   // placed at say_who_window xpos/ypos (anchored), with the name text inset by left/top padding.
   int twoWin = currentTwoWin && gui.whoLoaded && nameTex.valid && nameTex.tex.h > 0;

   // ADV: the box grows to fit the measured name + dialogue (Window past yminimum).
   int nameH = (!twoWin && nameTex.valid && nameTex.tex.h > 0) ? nameTex.tex.h + (int)(gui.nameSpacing * getGuiScale(cw) + 0.5f) : 0;
   int contentH = nameH + (textTex.valid ? textTex.tex.h : 0);
   int bx, by, bw, bh; getGuiTextboxRect(cx, cy, cw, ch, contentH, &bx, &by, &bw, &bh);
   drawGuiTextbox(bx, by, bw, bh, cw);
   int tx, ty, tw; getGuiTextboxTextArea(cx, cy, cw, ch, contentH, &tx, &ty, &tw);
   if (twoWin)
   {
      // Two-window namebox, translated from the layout (character.py + display/layout.py + 00style.rpy):
      //   say_two_window_vbox (yalign 1.0 -> BOTTOM-anchored) stacks [say_who_window, say_window]; its
      //   height = nameboxH + dialogueH, so its bottom is the content bottom and the dialogue window is
      //   our textbox (top = by). The vbox's vertical layout places each child with child.place(rv, 0,
      //   yo, ...), and place() DOES apply the child's own xpos/ypos (layout.py:629). So say_who_window's
      //   xpos=172 / ypos=96 are offsets: x = vbox_left + xpos, y = (cell top) + ypos. Working the
      //   bottom-anchored geometry through, the namebox TOP = by - nameboxH + who_ypos (scaled). xanchor/
      //   yanchor are honoured. (Earlier I dropped who_ypos on a wrong assumption -- restored per source.)
      // say_who_window is a WINDOW; its position in the bottom-anchored vbox is set by the window's
      // box-model HEIGHT (renpy Window.render: top_padding + child height + bottom_padding, >= yminimum),
      // NOT the RLname.png image height. bottom_padding is inherited from style.window (= gui.padB; the
      // namebox only overrides left/top padding); yminimum 34 doesn't bind once the name+padding exceed
      // it. namebox window top = by - windowH + who_ypos (the vbox bottom-anchored geometry). RLname.png
      // is the window's background Image, drawn at NATURAL size at the window top-left; the name child is
      // at (left_padding, top_padding) from the window top-left.
      float s = getGuiScale(cw), as = getGuiAssetScale(cw);
      int nbW = (int)(gui.whoTex.w * as + 0.5f), nbH = (int)(gui.whoTex.h * as + 0.5f);   // RLname.png natural size (the bg)
      // say_who_window box-model height = top_padding + child content height + bottom_padding (Window.render,
      // display/layout.py). The child content height is the text's LINE BOX (ascent - descent = TextEnd.lineHeight,
      // mirroring sizes() in display/text.py = get_ascent - get_descent), NOT the rasterized texture height --
      // tex.h carries surface slack (+fsize/+skip), which inflated winH and lifted the window top (the pill) a
      // few px too high. Fall back to tex.h only if no line metric was captured.
      int nameLineH = nameEnd.valid ? nameEnd.lineHeight : nameTex.tex.h;
      int winH = (int)(gui.whoTpad * s + 0.5f) + nameLineH + (int)(gui.padB * s + 0.5f);   // box-model window height
      int nbX = cx + (int)(gui.whoXpos * s + 0.5f) - (int)(gui.whoXanchor * nbW + 0.5f);   // vbox_left + xpos - xanchor*w
      int nbY = by - winH + (int)(gui.whoYpos * s + 0.5f);                                  // window top in the vbox (yanchor 0)
      drawGfxTexture(nbX, nbY, nbW, nbH, gui.whoTex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
      // Seat the name by its LINE BOX top at top_padding (Window.render places the child's line cell at
      // top_padding from the window top), not by the texture's pixel top-left. TextEnd.lineTop is the line-cell
      // top within the texture, so draw the texture at (top_padding - lineTop) to land the line box on top_padding
      // -- same convention as the inline ctc placement (gui.c drawGuiCtcInline). Texture-top anchoring sat the
      // glyphs ~lineTop px too low inside the pill.
      drawTex(&nameTex, nbX + (int)(gui.whoLpad * s + 0.5f),
              nbY + (int)(gui.whoTpad * s + 0.5f) - (nameEnd.valid ? nameEnd.lineTop : 0));   // say_who_window left/top padding
   }
   else drawTex(&nameTex, tx, ty);
   drawTypewriter(&typewriter, &textTex, tx, ty + nameH);   // dialogue types in under the name (nameH=0 for two_window)
   if (isSayTypingDone())   // ctc only after the line has typed out
   {
      if (gui.ctcFixed) drawGuiCtcFixed(cx, cy, cw);
      else              drawGuiCtcInline(tx, ty + nameH, &sayEnd, cw);
   }
}

void drawSayMenu(int cx, int cy, int cw, int ch, int selected, int count)
{
   // Classic in-game choice menu, translated from the styles:
   //   menu_choice_button.background/hover_background = Frame(img, L, T) -> a 9-SLICE button (choiceTex/
   //     hoverTex), sized to xminimum (FULL width here, 800 native) x yminimum, NOT the image's natural size.
   //   menu_choice_button.top_margin -> the vbox spacing; xalign 0.5 -> centred horizontally.
   //   menu_window.yalign -> the choices block's vertical placement (e.g. 0.4, not centred).
   //   menu_choice.size -> the caption text size (centred in the button).
   float scale = getGuiScale(cw);
   int btnW = gui.choiceXmin > 0 ? (int)(gui.choiceXmin * scale + 0.5f) : (int)(cw * 0.5f);
   if (btnW > cw) btnW = cw;                                   // clamp a full-width button to the content rect
   int size = gui.choiceSize > 0 ? (int)(gui.choiceSize * scale + 0.5f) : getGuiDlgSize(cw);
   int btnH = gui.choiceYmin > 0 ? (int)(gui.choiceYmin * scale + 0.5f) : size + 2 * (int)(8 * scale + 0.5f);
   int gap  = (int)(gui.choiceMargin * scale + 0.5f);         // menu_choice_button top_margin (vbox spacing)
   int bx = cx + (cw - btnW) / 2;                             // xalign 0.5
   int totalH = count * btnH + (count > 0 ? (count - 1) * gap : 0);
   int y = cy + (int)((ch - totalH) * gui.menuYalign + 0.5f);  // menu_window.yalign (default 0.5 = centred)
   for (int i = 0; i < count; i++)
   {
      const GfxTexture *bg = (i == selected && gui.hoverLoaded) ? &gui.hoverTex
                      : (gui.choiceLoaded ? &gui.choiceTex : NULL);
      if (bg && gui.choiceFrameX > 0)   // Frame() -> scaled 9-slice sized to the button (caps + edges preserved)
      {
         SpriteRegion whole = { 0, 0, bg->w, bg->h };
         NineSlice nine; initNineSliceScaled(&nine, *bg, bx, y, btnW, btnH, whole, gui.choiceFrameX, gui.choiceFrameY, scale);
         drawNineSlice(&nine);
      }
      else if (bg)
         drawGfxTexture(bx, y, btnW, btnH, *bg, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
      else
         fillGfxRectangle(bx, y, btnW, btnH, gui.textboxColor);   // no image -> selection shows via the text colour
      if (menuItemTex[i].valid)
         drawTex(&menuItemTex[i], bx + (btnW - menuItemTex[i].tex.w) / 2, y + (btnH - menuItemTex[i].tex.h) / 2);
      y += btnH + gap;
   }
}

void drawSayEnd(int cx, int cy, int cw, int ch)
{
   drawTex(&textTex, cx + hpx(cw, 90), cy + ch / 2 - hpx(cw, 40));
   drawTex(&hintTex, cx + cw - hpx(cw, 200), cy + ch - hpx(cw, 40));
}

void freeSay(void)
{
   freeTextTexture(&nameTex);
   freeTextTexture(&textTex);
   freeTextTexture(&hintTex);
   for (int i = 0; i < MAX_CHOICES; i++) freeTextTexture(&menuItemTex[i]);
   for (int i = 0; i < NVL_MAX; i++)     freeTextTexture(&nvlTex[i]);
}
