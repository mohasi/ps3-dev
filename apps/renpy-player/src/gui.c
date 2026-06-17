#include "gui.h"

#include <stdlib.h>
#include <string.h>
#include <sys/sys_time.h>

#include "colors.h"
#include "printf.h"
#include "dbg.h"
#include "ui/slice.h"
#include "rpk.h"

Gui gui;

static uint64_t blinkStartUs;   // anim.Blink clock origin (the moment the line appeared)

// Split a "A|B|C" manifest value into dst[0..], each capped at 24 bytes. Returns the count.
static int splitPipe(const char *v, char dst[][24], int max)
{
   int n = 0;
   while (v && *v && n < max)
   {
      const char *bar = strchr(v, '|');
      int len = bar ? (int)(bar - v) : (int)strlen(v);
      if (len > 23) len = 23;
      memcpy(dst[n], v, len); dst[n][len] = '\0';
      n++;
      v = bar ? bar + 1 : NULL;
   }
   return n;
}

// Loads a GUI image from the rpk by basename suffix (e.g. "frame.png" -> assets/frame.png).
static int loadGuiImage(RpkFile *r, const char *base, GfxTexture *out)
{
   char suffix[80];
   snprintf(suffix, sizeof suffix, "/%s", base);
   char name[256];
   unsigned char *buf = NULL; long len = 0;
   if (readRpkEntrySuffix(r, suffix, 0, name, sizeof name, &buf, &len) != 0 || !buf) return 0;
   *out = loadGfxTextureMem(buf, (uint32_t)len);
   free(buf);
   return out->w > 0 && out->h > 0;
}

// Resets every field to the Ren'Py ENGINE defaults (classic-game constants from the engine
// source -- see the citations). Manifest values override these.
static void guiDefaults(void)
{
   memset(&gui, 0, sizeof gui);
   gui.assetScale = 1.0f;
   gui.menuYalign = 0.5f;   // style.menu_window default: choices centred (games may set yalign, e.g. 0.4)
   // say_vbox.spacing 8, say_label inherits the dialogue colour (00style.rpy);
   // nvl_window #0008 pad 20/30, nvl_vbox box_spacing 10 (00nvl_mode.rpy);
   // classic window background Solid((0,0,128,128)) (_compat/styles.rpym);
   // menu_choice idle "#0ff" / hover "#ff0" (00style.rpy).
   gui.nameSpacing = 8;
   gui.nvlPadX = 20; gui.nvlPadY = 30; gui.nvlSpacing = 10;
   gui.nvlBg = 0x88000000u;
   gui.textboxColor = 0x80000080u;
   gui.textColor = COLOR_WHITE;
   gui.nameColor = COLOR_WHITE;        // resolved to the text colour after parsing
   gui.choiceColor = COLOR_WHITE;        // resolved to text colour (menu_choice inherits style.default)
   gui.choiceHoverColor = COLOR_WHITE;   // hover == idle (roundrect hover shows via the button background)
   gui.shadowColor = 0xFF000000u;      // style.default.drop_shadow_color default "#000"
   gui.mmFrameColor = 0xFF6496C8u;     // theme.roundrect default frame = (100,150,200,255)
   gui.gmBtnIdle     = 0xFF003C78u;    // theme.roundrect default widget          = (0,60,120,255)
   gui.gmBtnHover    = 0xFF0050A0u;    // theme.roundrect default widget_hover    = (0,80,160,255)
   gui.gmBtnText     = 0xFFC8E1FFu;    // theme.roundrect default widget_text     = (200,225,255,255)
   gui.gmBtnSelected = 0xFFFFFFC8u;    // theme.roundrect default widget_selected = (255,255,200,255)
   gui.gmBtnDisabled     = 0xFF404040u; // theme.roundrect default disabled      = (64,64,64,255)
   gui.gmBtnDisabledText = 0xFFC8C8C8u; // theme.roundrect default disabled_text = (200,200,200,255)
   gui.ctcXpos = gui.ctcYpos = -1;
   gui.padL = gui.padR = gui.padT = gui.padB = -1;   // -1 => fall back to padX/padY (resolved after parse)
   gui.igAlignX = 0.0f; gui.igAlignY = 1.0f;   // default chat box anchor: bottom-left (overridden by manifest)
   gui.igShadowColor = 0xFF000000u;            // what_drop_shadow_color default "#000"
}

void loadGui(const char *rpkPath)
{
   guiDefaults();

   RpkFile r;
   if (openRpk(&r, rpkPath) != 0) return;

   unsigned char *buf = NULL; long len = 0;
   char frameBg[64] = "", choiceBg[64] = "", hoverBg[64] = "", ctcImg[64] = "", igBg[64] = "";
   int nameColorSet = 0;   // did the manifest give a name colour? else inherit text colour
   int choiceColorSet = 0, choiceHoverSet = 0;   // else menu_choice inherits style.default.color (text colour)
   if (readRpkEntry(&r, "game.gui", &buf, &len) == 0 && buf)
   {
      // line-by-line key=value parse
      long i = 0;
      while (i < len)
      {
         char line[256]; int n = 0;
         while (i < len && buf[i] != '\n' && n < (int)sizeof(line) - 1) line[n++] = (char)buf[i++];
         if (i < len) i++;                       // skip newline
         line[n] = '\0';
         char *eq = strchr(line, '=');
         if (!eq) continue;
         *eq = '\0';
         const char *k = line, *v = eq + 1;
         if      (strcmp(k, "native_w") == 0) gui.nativeW = atoi(v);
         else if (strcmp(k, "native_h") == 0) gui.nativeH = atoi(v);
         else if (strcmp(k, "asset_scale") == 0) { float f = (float)atof(v); if (f > 0.0f) gui.assetScale = f; }
         else if (strcmp(k, "text_size") == 0) gui.textSize = atoi(v);
         else if (strcmp(k, "name_size") == 0) gui.nameSize = atoi(v);
         else if (strcmp(k, "text_color") == 0) gui.textColor = parseColor(v, gui.textColor);
         else if (strcmp(k, "name_color") == 0) { gui.nameColor = parseColor(v, gui.nameColor); nameColorSet = 1; }
         else if (strcmp(k, "text_shadow") == 0)
         {
            // "dx,dy" (style.default.drop_shadow; negative offsets allowed)
            gui.shadowDx = atoi(v);
            const char *c = strchr(v, ','); gui.shadowDy = c ? atoi(c + 1) : gui.shadowDx;
         }
         else if (strcmp(k, "text_shadow_color") == 0) gui.shadowColor = parseColor(v, gui.shadowColor);
         else if (strcmp(k, "text_advance") == 0) gui.textAdvanceCeil = (strcmp(v, "ceil") == 0);
         else if (strcmp(k, "text_cps") == 0) gui.textCps = atoi(v);
         else if (strcmp(k, "choice_color") == 0) { gui.choiceColor = parseColor(v, gui.choiceColor); choiceColorSet = 1; }
         else if (strcmp(k, "choice_hover_color") == 0) { gui.choiceHoverColor = parseColor(v, gui.choiceHoverColor); choiceHoverSet = 1; }
         else if (strcmp(k, "choice_frame") == 0) { gui.choiceFrameX = atoi(v); const char *c = strchr(v, ','); gui.choiceFrameY = c ? atoi(c + 1) : gui.choiceFrameX; }
         else if (strcmp(k, "choice_xmin") == 0) gui.choiceXmin = atoi(v);
         else if (strcmp(k, "choice_ymin") == 0) gui.choiceYmin = atoi(v);
         else if (strcmp(k, "choice_size") == 0) gui.choiceSize = atoi(v);
         else if (strcmp(k, "choice_margin") == 0) gui.choiceMargin = atoi(v);
         else if (strcmp(k, "menu_yalign") == 0) gui.menuYalign = (float)atof(v);
         else if (strcmp(k, "textbox_color") == 0) gui.textboxColor = parseColor(v, gui.textboxColor);
         else if (strcmp(k, "textbox_height") == 0) gui.textboxH = atoi(v);
         else if (strcmp(k, "textbox_margin_b") == 0) gui.marginB = atoi(v);
         else if (strcmp(k, "textbox_margin_t") == 0) gui.marginT = atoi(v);
         else if (strcmp(k, "textbox_xmargin") == 0) gui.xmargin = atoi(v);
         else if (strcmp(k, "textbox_pad_x") == 0) gui.padX = atoi(v);
         else if (strcmp(k, "textbox_pad_y") == 0) gui.padY = atoi(v);
         else if (strcmp(k, "textbox_pad_l") == 0) gui.padL = atoi(v);
         else if (strcmp(k, "textbox_pad_r") == 0) gui.padR = atoi(v);
         else if (strcmp(k, "textbox_pad_t") == 0) gui.padT = atoi(v);
         else if (strcmp(k, "textbox_pad_b") == 0) gui.padB = atoi(v);
         else if (strcmp(k, "textbox_ymin") == 0) gui.ymin = atoi(v);
         else if (strcmp(k, "textbox_bg") == 0) snprintf(frameBg, sizeof frameBg, "%s", v);
         else if (strcmp(k, "choice_bg") == 0) snprintf(choiceBg, sizeof choiceBg, "%s", v);
         else if (strcmp(k, "choice_hover_bg") == 0) snprintf(hoverBg, sizeof hoverBg, "%s", v);
         else if (strcmp(k, "ctc") == 0) snprintf(ctcImg, sizeof ctcImg, "%s", v);
         else if (strcmp(k, "ctc_xpos") == 0) gui.ctcXpos = atoi(v);
         else if (strcmp(k, "ctc_ypos") == 0) gui.ctcYpos = atoi(v);
         else if (strcmp(k, "ctc_xanchor") == 0) gui.ctcXanchor = atoi(v);
         else if (strcmp(k, "ctc_yanchor") == 0) gui.ctcYanchor = atoi(v);
         else if (strcmp(k, "ctc_fixed") == 0) gui.ctcFixed = atoi(v);
         else if (strcmp(k, "name_spacing") == 0) gui.nameSpacing = atoi(v);
         else if (strcmp(k, "name_bold") == 0) gui.nameBold = atoi(v);
         else if (strcmp(k, "nvl_pad_x") == 0) gui.nvlPadX = atoi(v);
         else if (strcmp(k, "nvl_pad_y") == 0) gui.nvlPadY = atoi(v);
         else if (strcmp(k, "nvl_spacing") == 0) gui.nvlSpacing = atoi(v);
         else if (strcmp(k, "nvl_bg") == 0) gui.nvlBg = parseColor(v, gui.nvlBg);
         else if (strcmp(k, "textbox_frame") == 0)
         {
            // "l,t,r,b" -> we use l as inset X, t as inset Y (corners are symmetric in practice)
            int a = atoi(v); gui.frameInsetX = a;
            const char *c = strchr(v, ','); gui.frameInsetY = c ? atoi(c + 1) : a;
         }
         else if (strcmp(k, "text_font") == 0) snprintf(gui.dlgFont, sizeof gui.dlgFont, "%s", v);
         else if (strcmp(k, "ig_textbox_bg") == 0) snprintf(igBg, sizeof igBg, "%s", v);
         else if (strcmp(k, "ig_pad_l") == 0) gui.igPadL = atoi(v);
         else if (strcmp(k, "ig_pad_r") == 0) gui.igPadR = atoi(v);
         else if (strcmp(k, "ig_pad_b") == 0) gui.igPadB = atoi(v);
         else if (strcmp(k, "ig_pad_t") == 0) gui.igPadT = atoi(v);
         else if (strcmp(k, "ig_font") == 0) snprintf(gui.igFont, sizeof gui.igFont, "%s", v);
         else if (strcmp(k, "ig_size") == 0) gui.igSize = atoi(v);
         else if (strcmp(k, "ig_align_x") == 0) gui.igAlignX = (float)atof(v);
         else if (strcmp(k, "ig_align_y") == 0) gui.igAlignY = (float)atof(v);
         else if (strcmp(k, "ig_shadow") == 0) { gui.igShadowDx = atoi(v); const char *c = strchr(v, ','); gui.igShadowDy = c ? atoi(c + 1) : gui.igShadowDx; }
         else if (strcmp(k, "ig_shadow_color") == 0) gui.igShadowColor = parseColor(v, gui.igShadowColor);
         else if (strcmp(k, "mm_order") == 0) gui.mmOrderCount = splitPipe(v, gui.mmOrder, GUI_MENU_MAX);
         else if (strcmp(k, "gm_order") == 0) gui.gmOrderCount = splitPipe(v, gui.gmOrder, GUI_MENU_MAX);
         else if (strcmp(k, "mm_bg") == 0) snprintf(gui.mmBg, sizeof gui.mmBg, "%s", v);
         else if (strcmp(k, "mm_music") == 0) snprintf(gui.mmMusic, sizeof gui.mmMusic, "%s", v);
         else if (strcmp(k, "gm_bg") == 0) snprintf(gui.gmBg, sizeof gui.gmBg, "%s", v);
         else if (strcmp(k, "mm_frame_color") == 0) gui.mmFrameColor = parseColor(v, gui.mmFrameColor);
         else if (strcmp(k, "mm_frame_none") == 0) gui.mmFrameNone = atoi(v);
         else if (strcmp(k, "gm_btn_idle") == 0) gui.gmBtnIdle = parseColor(v, gui.gmBtnIdle);
         else if (strcmp(k, "gm_btn_hover") == 0) gui.gmBtnHover = parseColor(v, gui.gmBtnHover);
         else if (strcmp(k, "gm_btn_text") == 0) gui.gmBtnText = parseColor(v, gui.gmBtnText);
         else if (strcmp(k, "gm_btn_selected") == 0) gui.gmBtnSelected = parseColor(v, gui.gmBtnSelected);
         else if (strcmp(k, "gm_btn_disabled") == 0) gui.gmBtnDisabled = parseColor(v, gui.gmBtnDisabled);
         else if (strcmp(k, "gm_btn_disabled_text") == 0) gui.gmBtnDisabledText = parseColor(v, gui.gmBtnDisabledText);
         else if (strcmp(k, "gm_text_size") == 0) gui.gmTextSize = atoi(v);
         else if (strcmp(k, "slot_text_size") == 0) gui.slotTextSize = atoi(v);
         else if (strcmp(k, "slot_text_x") == 0) gui.slotTextX = atoi(v);
         else if (strcmp(k, "slot_text_y") == 0) gui.slotTextY = atoi(v);
         else if (strcmp(k, "slot_ss_x") == 0) gui.slotSsX = atoi(v);
         else if (strcmp(k, "slot_ss_y") == 0) gui.slotSsY = atoi(v);
         else if (strcmp(k, "slot_text_color") == 0) gui.slotTextColor = parseColor(v, gui.slotTextColor);
         else if (strcmp(k, "thumb_w") == 0) gui.thumbW = atoi(v);
         else if (strcmp(k, "thumb_h") == 0) gui.thumbH = atoi(v);
         else if (strncmp(k, "mm_btn.", 7) == 0 && gui.mmBtnCount < GUI_MMBTN_MAX)
         {
            snprintf(gui.mmBtnLabel[gui.mmBtnCount], sizeof gui.mmBtnLabel[0], "%s", k + 7);
            // value is idle|hover|selected|insensitive (trailing fields may be empty).
            char *field[4] = { gui.mmBtnIdle[gui.mmBtnCount], gui.mmBtnHover[gui.mmBtnCount],
                           gui.mmBtnSelected[gui.mmBtnCount], gui.mmBtnInsensitive[gui.mmBtnCount] };
            int cap = (int)sizeof gui.mmBtnIdle[0];
            const char *p = v;
            for (int i = 0; i < 4; i++)
            {
               const char *bar = p ? strchr(p, '|') : NULL;
               int len = !p ? 0 : (bar ? (int)(bar - p) : (int)strlen(p));
               if (len >= cap) len = cap - 1;
               if (p) memcpy(field[i], p, len);
               field[i][len] = '\0';
               p = bar ? bar + 1 : NULL;
            }
            gui.mmBtnCount++;
         }
         else if (strcmp(k, "two_window_names") == 0)
         {
            gui.twoWinCount = 0;                       // pipe-split into the 48-wide name table
            const char *q = v;
            while (q && *q && gui.twoWinCount < GUI_TWOWIN_MAX)
            {
               const char *bar = strchr(q, '|');
               int len = bar ? (int)(bar - q) : (int)strlen(q);
               int cap = (int)sizeof gui.twoWin[0];
               if (len >= cap) len = cap - 1;
               memcpy(gui.twoWin[gui.twoWinCount], q, len);
               gui.twoWin[gui.twoWinCount][len] = '\0';
               gui.twoWinCount++;
               q = bar ? bar + 1 : NULL;
            }
         }
         else if (strcmp(k, "who_bg") == 0) snprintf(gui.whoBg, sizeof gui.whoBg, "%s", v);
         else if (strcmp(k, "who_xpos") == 0) gui.whoXpos = atoi(v);
         else if (strcmp(k, "who_ypos") == 0) gui.whoYpos = atoi(v);
         else if (strcmp(k, "who_lpad") == 0) gui.whoLpad = atoi(v);
         else if (strcmp(k, "who_tpad") == 0) gui.whoTpad = atoi(v);
         else if (strcmp(k, "who_xanchor") == 0) gui.whoXanchor = (float)atof(v);
         else if (strcmp(k, "who_yanchor") == 0) gui.whoYanchor = (float)atof(v);
         else if (strncmp(k, "side_image.", 11) == 0 && gui.sideImgCount < GUI_SIDEIMG_MAX)
         {
            // value = "<xalign>,<yalign>;<exprId>:<img>|<exprId>:<img>|..."
            GuiSideImage *si = &gui.sideImg[gui.sideImgCount];
            snprintf(si->name, sizeof si->name, "%s", k + 11);
            si->alignX = (float)atof(v);
            const char *comma = strchr(v, ',');
            si->alignY = comma ? (float)atof(comma + 1) : 1.0f;
            const char *semi = strchr(v, ';');
            si->count = 0;
            const char *p = semi ? semi + 1 : NULL;
            while (p && *p && si->count < GUI_SIDEIMG_COND_MAX)
            {
               si->exprId[si->count] = atoi(p);            // "<id>:<img>"
               const char *colon = strchr(p, ':');
               const char *bar = strchr(p, '|');
               if (!colon) break;
               const char *imgStart = colon + 1;
               int len = bar ? (int)(bar - imgStart) : (int)strlen(imgStart);
               int cap = (int)sizeof si->img[0];
               if (len >= cap) len = cap - 1;
               memcpy(si->img[si->count], imgStart, len);
               si->img[si->count][len] = '\0';
               si->count++;
               p = bar ? bar + 1 : NULL;
            }
            if (si->count > 0) gui.sideImgCount++;
         }
         else if (strncmp(k, "char_color.", 11) == 0 && gui.charColCount < GUI_CHARCOL_MAX)
         {
            snprintf(gui.charName[gui.charColCount], sizeof gui.charName[0], "%s", k + 11);
            gui.charCol[gui.charColCount] = parseColor(v, gui.textColor);
            gui.charColCount++;
         }
      }
      free(buf);
   }
   if (!nameColorSet) gui.nameColor = gui.textColor;   // say_label inherits the dialogue colour
   // menu_choice = Style(style.default) -> inherits style.default.color (text colour); roundrect hover is
   // via the button background, not the text, so hover == idle.
   if (!choiceColorSet) gui.choiceColor = gui.textColor;
   if (!choiceHoverSet) gui.choiceHoverColor = gui.choiceColor;
   // Resolve per-edge window paddings: a game sets either the symmetric xpadding/ypadding or the
   // asymmetric {left,right,top,bottom}_padding. Fall back to the symmetric value for any edge unset.
   if (gui.padL < 0) gui.padL = gui.padX;
   if (gui.padR < 0) gui.padR = gui.padX;
   if (gui.padT < 0) gui.padT = gui.padY;
   if (gui.padB < 0) gui.padB = gui.padY;
   if (frameBg[0])  gui.frameLoaded  = loadGuiImage(&r, frameBg, &gui.frameTex);
   if (choiceBg[0]) gui.choiceLoaded = loadGuiImage(&r, choiceBg, &gui.choiceTex);
   if (hoverBg[0])  gui.hoverLoaded  = loadGuiImage(&r, hoverBg, &gui.hoverTex);
   if (ctcImg[0])   gui.ctcLoaded    = loadGuiImage(&r, ctcImg, &gui.ctcTex);
   if (igBg[0])     gui.igLoaded     = loadGuiImage(&r, igBg, &gui.igTex);
   if (gui.whoBg[0]) gui.whoLoaded   = loadGuiImage(&r, gui.whoBg, &gui.whoTex);   // say_who_window namebox bg
   closeRpk(&r);
   logInfo("[rpp] gui: native=%dx%d assetScaleX1000=%d textSize=%d frame=%d(%dx%d) choices=%d charCols=%d\n",
           gui.nativeW, gui.nativeH, (int)(gui.assetScale * 1000 + 0.5f), gui.textSize, gui.frameLoaded,
           gui.frameLoaded ? gui.frameTex.w : 0, gui.frameLoaded ? gui.frameTex.h : 0,
           gui.choiceLoaded, gui.charColCount);   // no %f: homebrew printf can't format floats (misaligned the rest)
}

void freeGui(void)
{
   if (gui.frameLoaded)  { freeGfxTexture(&gui.frameTex);  gui.frameLoaded = 0; }
   if (gui.choiceLoaded) { freeGfxTexture(&gui.choiceTex); gui.choiceLoaded = 0; }
   if (gui.hoverLoaded)  { freeGfxTexture(&gui.hoverTex);  gui.hoverLoaded = 0; }
   if (gui.ctcLoaded)    { freeGfxTexture(&gui.ctcTex);    gui.ctcLoaded = 0; }
   if (gui.igLoaded)     { freeGfxTexture(&gui.igTex);     gui.igLoaded = 0; }
   if (gui.whoLoaded)    { freeGfxTexture(&gui.whoTex);    gui.whoLoaded = 0; }
}

const GuiSideImage *getGuiSideImage(const char *who)
{
   if (!who || !who[0]) return NULL;
   for (int i = 0; i < gui.sideImgCount; i++)
      if (strcmp(gui.sideImg[i].name, who) == 0) return &gui.sideImg[i];
   return NULL;
}

int isGuiTwoWindow(const char *who)
{
   if (!who || !who[0]) return 0;
   for (int i = 0; i < gui.twoWinCount; i++)
      if (strcmp(gui.twoWin[i], who) == 0) return 1;
   return 0;
}

float getGuiScale(int cw) { return gui.nativeW > 0 ? (float)cw / (float)gui.nativeW : 1.0f; }

float getGuiAssetScale(int cw) { return getGuiScale(cw) / (gui.assetScale > 0.0f ? gui.assetScale : 1.0f); }

int getGuiDlgSize(int cw)
{
   // Engine default dialogue size is 22 (style.default.size); used when the game sets none.
   int base = gui.textSize > 0 ? gui.textSize : (gui.nativeW > 0 ? 22 : 28);
   int s = (int)(base * getGuiScale(cw) + 0.5f);
   return s < 10 ? 10 : s;
}

int getGuiGmTextSize(int cw)
{
   // theme.roundrect button text_size: engine default 18 (native <= 640) else 22.
   int base = gui.gmTextSize > 0 ? gui.gmTextSize : (gui.nativeW > 0 && gui.nativeW <= 640 ? 18 : 22);
   int s = (int)(base * getGuiScale(cw) + 0.5f);
   return s < 10 ? 10 : s;
}

int getGuiSlotTextSize(int cw)
{
   // file_picker_text = large_button_text; engine default is small_text_size (16 native > 640, else 12).
   int base = gui.slotTextSize > 0 ? gui.slotTextSize : (gui.nativeW > 0 && gui.nativeW <= 640 ? 12 : 16);
   int s = (int)(base * getGuiScale(cw) + 0.5f);
   return s < 8 ? 8 : s;
}

int getGuiNameSize(int cw)
{
   // say_label inherits the dialogue size unless the game overrides it.
   int base = gui.nameSize > 0 ? gui.nameSize : (gui.textSize > 0 ? gui.textSize : (gui.nativeW > 0 ? 22 : 26));
   int s = (int)(base * getGuiScale(cw) + 0.5f);
   return s < 10 ? 10 : s;
}

uint32_t getGuiNameColor(const char *who)
{
   if (who)
      for (int i = 0; i < gui.charColCount; i++)
         if (strcmp(gui.charName[i], who) == 0) return gui.charCol[i];
   return gui.nameColor;
}

int getGuiTextShadow(int cw, TextShadow *out)
{
   if (gui.shadowDx == 0 && gui.shadowDy == 0) return 0;
   float s = getGuiScale(cw);
   out->dx = (int)(gui.shadowDx * s + (gui.shadowDx >= 0 ? 0.5f : -0.5f));
   out->dy = (int)(gui.shadowDy * s + (gui.shadowDy >= 0 ? 0.5f : -0.5f));
   out->color = gui.shadowColor;
   // a non-zero native offset must survive scaling (a 1px shadow at 0.4x is still 1px)
   if (out->dx == 0 && gui.shadowDx != 0) out->dx = gui.shadowDx > 0 ? 1 : -1;
   if (out->dy == 0 && gui.shadowDy != 0) out->dy = gui.shadowDy > 0 ? 1 : -1;
   return 1;
}

// native->screen X/Y inside the content rect.
static int nx2sx(int cx, int cw, int nx) { return cx + (int)(nx * getGuiScale(cw) + 0.5f); }

// The textbox border box, computed EXACTLY from the game's style.window box model
// (renpy/display/layout.py Window.render):
//   window height H = max(margins + padding + child, yminimum)   [yfill False]
//   background box  = H - margins, inset by the margins
//   placement       = yalign 1.0 (bottom of the screen), xfill True inset by xmargin.
// Falls back to a frame-aspect/proportional box only when the native model is unavailable
// (e.g. a bundle with no manifest).
void getGuiTextboxRect(int cx, int cy, int cw, int ch, int contentH, int *bx, int *by, int *bw, int *bh)
{
   float s = getGuiScale(cw);
   if (gui.nativeW > 0 && gui.nativeH > 0 && gui.ymin > 0)
   {
      int marT = (int)(gui.marginT * s + 0.5f), marB = (int)(gui.marginB * s + 0.5f);
      int padT = (int)(gui.padT * s + 0.5f), padB = (int)(gui.padB * s + 0.5f);
      int yminS = (int)(gui.ymin * s + 0.5f);
      int winH = marT + marB + padT + padB + (contentH > 0 ? contentH : 0);
      if (winH < yminS) winH = yminS;                      // grow past yminimum with content
      *bh = winH - marT - marB;                            // border box excludes margins
      *bx = nx2sx(cx, cw, gui.xmargin);
      *bw = nx2sx(cx, cw, gui.nativeW - gui.xmargin) - *bx;
      *by = cy + ch - marB - *bh;                          // yalign 1.0: bottom-anchored
      return;
   }
   // fallback (no manifest): bottom-anchored, height from frame image aspect / 26% default.
   int h;
   if (gui.textboxH > 0)                       h = (int)(gui.textboxH * s + 0.5f);
   else if (gui.frameLoaded && gui.frameTex.w > 0) h = (int)((float)gui.frameTex.h * (float)cw / (float)gui.frameTex.w + 0.5f);
   else                                        h = (int)(ch * 0.26f);
   if (h > ch) h = ch;
   int marginB = (int)(gui.marginB * s + 0.5f);
   *bw = cw; *bx = cx; *bh = h; *by = cy + ch - h - marginB;
}

void getGuiTextboxTextArea(int cx, int cy, int cw, int ch, int contentH, int *tx, int *ty, int *tw)
{
   int bx, by, bw, bh; getGuiTextboxRect(cx, cy, cw, ch, contentH, &bx, &by, &bw, &bh);
   float s = getGuiScale(cw);
   if (gui.nativeW > 0 && gui.ymin > 0)
   {
      // child placed at (left_padding, top_padding); wrap to interior (width - left - right padding).
      int padL = (int)(gui.padL * s + 0.5f);
      int padR = (int)(gui.padR * s + 0.5f);
      int padT = (int)(gui.padT * s + 0.5f);
      *tx = bx + padL; *ty = by + padT; *tw = bw - padL - padR;
   }
   else
   {
      int padX = (int)((gui.frameInsetX + 6) * s); if (padX < 10) padX = 10;
      int padY = (int)((gui.frameInsetY + 6) * s); if (padY < 6) padY = 6;
      *tx = bx + padX; *ty = by + padY; *tw = bw - 2 * padX;
   }
   if (*tw < 100) *tw = 100;
}

// ---- in-game chat textbox ----
//
// The box is the igTex image at its NATURAL size (window_background = Image(), not Frame, so it is
// NOT stretched), anchored bottom-left of the content rect (window xalign 0.0 / yalign 1.0). The
// text sits inside it inset by window_left/right_padding (X + wrap width) and lifted off the bottom
// by window_bottom_padding (the caller places Y from the measured text height).
// The in-game chat WINDOW border rect, emulating Ren'Py Window.render (renpy/display/layout.py):
//   height = max(top_margin + bottom_margin + top_padding + bottom_padding + child, yminimum)  [yfill False]
//   width  = full content width inset by xmargin                                                [xfill True]
//   placement = style.window yalign 1.0 -> bottom-anchored.
// The chat Character overrides left/right/bottom padding + background only; it INHERITS the window's
// top_padding (igPadT), yminimum (gui.ymin) and margins -- exactly as the engine resolves the style.
// All values come from the manifest, so this is engine-version-agnostic with no engine at runtime.
int getGuiIngameWindow(int cx, int cy, int cw, int ch, int contentH, int *wx, int *wy, int *ww, int *wh)
{
   float s = getGuiScale(cw);
   int marT = (int)(gui.marginT * s + 0.5f), marB = (int)(gui.marginB * s + 0.5f);
   int xm   = (int)(gui.xmargin * s + 0.5f);
   int padT = (int)(gui.igPadT * s + 0.5f), padB = (int)(gui.igPadB * s + 0.5f);
   int yminS = (int)(gui.ymin * s + 0.5f);
   int winH = marT + marB + padT + padB + (contentH > 0 ? contentH : 0);
   if (winH < yminS) winH = yminS;            // shrink-to-fit but never below yminimum
   *wh = winH - marT - marB;                  // border box excludes margins
   *wx = cx + xm;
   *ww = cw - 2 * xm;                         // xfill
   *wy = cy + ch - marB - *wh;                // yalign 1.0 (bottom of the content rect)
   return 1;
}

void getGuiIngameTextArea(int cx, int cy, int cw, int ch, int *tx, int *tw)
{
   int wx, wy, ww, wh; getGuiIngameWindow(cx, cy, cw, ch, 0, &wx, &wy, &ww, &wh);
   float s = getGuiScale(cw);
   int padL = (int)(gui.igPadL * s + 0.5f);
   int padR = (int)(gui.igPadR * s + 0.5f);
   *tx = wx + padL;                            // left_padding from the window's left edge
   *tw = ww - padL - padR;                     // wrap to the window interior (xfill window)
   if (*tw < 40) *tw = 40;
}

int getGuiIngamePadT(int cw) { return (int)(gui.igPadT * getGuiScale(cw) + 0.5f); }

int getGuiIngameShadow(int cw, TextShadow *out)
{
   if (gui.igShadowDx == 0 && gui.igShadowDy == 0) return 0;
   float s = getGuiScale(cw);
   out->dx = (int)(gui.igShadowDx * s + (gui.igShadowDx >= 0 ? 0.5f : -0.5f));
   out->dy = (int)(gui.igShadowDy * s + (gui.igShadowDy >= 0 ? 0.5f : -0.5f));
   out->color = gui.igShadowColor;
   if (out->dx == 0 && gui.igShadowDx != 0) out->dx = gui.igShadowDx > 0 ? 1 : -1;
   if (out->dy == 0 && gui.igShadowDy != 0) out->dy = gui.igShadowDy > 0 ? 1 : -1;
   return 1;
}

int getGuiIngameSize(int cw)
{
   // Faithful: the chat character's what_size (native px), scaled only by the content/letterbox scale
   // -- no fudge factor. (An earlier 1.45x multiplier was compensating for the font wrongly falling
   // back to the dialogue font; with the correct font loaded the literal size is right.)
   int base = gui.igSize > 0 ? gui.igSize : (gui.textSize > 0 ? gui.textSize : 15);
   int s = (int)(base * getGuiScale(cw) + 0.5f);
   return s < 8 ? 8 : s;
}

void drawGuiIngameBox(int bx, int by, int bw, int bh, int cw)
{
   (void)cw;
   if (!gui.igLoaded) return;
   drawGfxTexture(bx, by, bw, bh, gui.igTex, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
}

// Draws the textbox background. With a Frame() image: a 9-slice whose corner caps are the
// image's native inset pixels drawn at the DISPLAY-SCALED size -- Ren'Py composes the Frame
// at the game's native resolution and then scales the whole surface, so the caps scale by
// the same ratio as the centre (ui/slice.h initNineSliceScaled). Without an image: the
// game's (or classic engine's) solid window colour.
void drawGuiTextbox(int bx, int by, int bw, int bh, int cw)
{
   if (!gui.frameLoaded) { fillGfxRectangle(bx, by, bw, bh, gui.textboxColor); return; }

   GfxTexture t = gui.frameTex;
   // native insets -> pixels in the (possibly pre-scaled) bundled image
   int capX = (int)(gui.frameInsetX * gui.assetScale + 0.5f);
   int capY = (int)(gui.frameInsetY * gui.assetScale + 0.5f);
   if (capX <= 0 && capY <= 0)
   {
      drawGfxTexture(bx, by, bw, bh, t, 0.0f, 0.0f, 1.0f, 1.0f, COLOR_WHITE, GFX_FILTER_LINEAR);
      return;
   }
   SpriteRegion whole = { 0, 0, t.w, t.h };
   NineSlice ns;
   initNineSliceScaled(&ns, t, bx, by, bw, bh, whole, capX, capY, getGuiAssetScale(cw));
   drawNineSlice(&ns);
}

void restartGuiCtc(void)
{
   blinkStartUs = sys_time_get_system_time();
}

// Exact anim.Blink curve (renpy/display/anim.py defaults on=set=off=rise=0.5s, high=1,
// low=0): 2.0s cycle = on(high) -> ramp down -> off(low) -> ramp up, on the real clock,
// phase-anchored to when the line appeared (the displayable's st).
void drawGuiCtcAt(int x, int y, int cw)
{
   if (!gui.ctcLoaded) return;
   uint64_t now = sys_time_get_system_time();
   float t = (float)((now - blinkStartUs) % 2000000ull) / 1000000.0f;
   float a;
   if      (t < 0.5f) a = 1.0f;
   else if (t < 1.0f) a = 1.0f - (t - 0.5f) / 0.5f;
   else if (t < 1.5f) a = 0.0f;
   else               a = (t - 1.5f) / 0.5f;
   uint32_t alpha = (uint32_t)(a * 255.0f + 0.5f);
   float s = getGuiAssetScale(cw);
   int iw = (int)(gui.ctcTex.w * s + 0.5f), ih = (int)(gui.ctcTex.h * s + 0.5f);
   drawGfxTexture(x, y, iw, ih, gui.ctcTex, 0.0f, 0.0f, 1.0f, 1.0f, (alpha << 24) | 0x00FFFFFFu, GFX_FILTER_LINEAR);
}

void drawGuiCtcFixed(int cx, int cy, int cw)
{
   if (!gui.ctcLoaded || gui.ctcXpos < 0) return;
   float s = getGuiScale(cw);
   float as = getGuiAssetScale(cw);
   int iw = (int)(gui.ctcTex.w * as + 0.5f), ih = (int)(gui.ctcTex.h * as + 0.5f);
   int x = cx + (int)(gui.ctcXpos * s + 0.5f) - gui.ctcXanchor * iw;   // integer anchors only (gap: floats)
   int y = cy + (int)(gui.ctcYpos * s + 0.5f) - gui.ctcYanchor * ih;
   drawGuiCtcAt(x, y, cw);
}

// Nestled ctc is an INLINE displayable appended after the text's last glyph
// (character.py: what_text.tokens.append([("widget", ctc)]) when ctc_position=="nestled").
// Ren'Py lays an inline widget with its TOP at the line cell top: render_pass computes
// actual_y = y + max_ascent - widget_ascent, and the widget's ascent equals the line's
// font ascent (WidgetStyle.ascent = ts.get_ascent()), so actual_y == y == the line top.
// renderFontEx reports that line-cell top (TextEnd.lineTop) and the pen x after the final
// glyph (endX); we top-align the icon there at its natural size. (The old base-on-baseline
// placement sat ~(ascent - icon_height) px too low.)
void drawGuiCtcInline(int textX, int textY, const TextEnd *end, int cw)
{
   if (!gui.ctcLoaded || !end || !end->valid) return;
   drawGuiCtcAt(textX + end->endX, textY + end->lineTop, cw);
}
