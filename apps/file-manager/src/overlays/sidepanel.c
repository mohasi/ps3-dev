// sidepanel - slides in from the right; header + vertical action list.
#include "overlays/sidepanel.h"
#include "gfx.h"
#include "anim.h"
#include "pad.h"
#include "font.h"
#include "audio.h"
#include "colors.h"
#include "ui/label.h"
#include "ui/icon-font.h"
#include "theme.h"
#include "button-repeat.h"
#include "string-utilities.h"

#define SIDEPANEL_MAX_ACTIONS 9
#define PANEL_WIDTH      474
#define PANEL_BORDER     2
#define SLIDE_MS         300

// header layout (panel-relative)
#define HEADER_HEIGHT    197
#define TOP_NO_HEADER    18  // top inset for the first row when no header is drawn
#define ICON_X           35
#define ICON_Y           70
#define HEADER_ICON_SIZE 64  // header file-type glyph font size
#define TEXT_X           133
#define TEXT_RIGHT_PAD   8
#define TITLE_Y          75
#define SUBTITLE_Y       108
#define DETAIL_Y         136
#define TITLE_SIZE       24
#define SUBTITLE_SIZE    18
#define DETAIL_SIZE      18

// row layout (item 414x94; first row at panel-relative (18, HEADER_HEIGHT))
#define ROW_X            18
#define ROW_WIDTH        414
#define ROW_HEIGHT       94
#define ROW_ICON_OFFSET  25
#define ROW_ICON_SIZE    48
#define ROW_TEXT_X       96
#define ROW_TITLE_Y      22
#define ROW_SUBTITLE_Y   57
#define ROW_TITLE_SIZE   22
#define ROW_SUBTITLE_SIZE 16


static float x;
static Anims anims;
static Font  font;

static SelectionSummary       summary;
static SelectionAction        actions[SIDEPANEL_MAX_ACTIONS];
static int                    actionCount;
static int                    selectedIndex;
static int                    hasHeader;
static SelectionActionHandler actionHandler;

static Label headerTitle, headerSubtitle, headerDetail;
static Label rowTitles[SIDEPANEL_MAX_ACTIONS];
static Label rowSubtitles[SIDEPANEL_MAX_ACTIONS];
static Icon  headerIcon;
static Icon  rowIcons[SIDEPANEL_MAX_ACTIONS];
static Audio *clickSfx;

void initSidepanel(Audio *sfx, SelectionActionHandler handler)
{
   clickSfx      = sfx;
   actionHandler = handler;

   font = openSystemFont(FONT_POP);
   int headerLabelW = PANEL_WIDTH - TEXT_X    - TEXT_RIGHT_PAD;
   int rowLabelW    = ROW_WIDTH   - ROW_TEXT_X - TEXT_RIGHT_PAD;
   initLabel(&headerTitle,    &font, 0, 0, headerLabelW, AUTO, TITLE_SIZE,    activeTheme->textPrimary,   TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&headerSubtitle, &font, 0, 0, headerLabelW, AUTO, SUBTITLE_SIZE, activeTheme->textSecondary, TEXT_NOWRAP,          "");
   initLabel(&headerDetail,   &font, 0, 0, headerLabelW, AUTO, DETAIL_SIZE,   activeTheme->textSecondary, TEXT_NOWRAP,          "");
   for (int i = 0; i < SIDEPANEL_MAX_ACTIONS; i++) {
      initLabel(&rowTitles[i],    &font, 0, 0, rowLabelW, AUTO, ROW_TITLE_SIZE,    activeTheme->textPrimary,     TEXT_NOWRAP_ELLIPSIS, "");
      initLabel(&rowSubtitles[i], &font, 0, 0, rowLabelW, AUTO, ROW_SUBTITLE_SIZE, activeTheme->textOnHighlight, TEXT_NOWRAP_ELLIPSIS, "");
   }
}

void setSidepanelContent(const SelectionSummary *s, const SelectionAction *a, int count)
{
   if (count > SIDEPANEL_MAX_ACTIONS) count = SIDEPANEL_MAX_ACTIONS;
   actionCount = count;
   for (int i = 0; i < count; i++) actions[i] = a[i];

   // default to Paste when it's offered - a cut/copy is usually followed by a paste.
   selectedIndex = 0;
   for (int i = 0; i < actionCount; i++)
      if (actions[i] == ACTION_PASTE) { selectedIndex = i; break; }

   for (int i = 0; i < actionCount; i++) {
      setLabelText(&rowTitles[i],    getActionTitle(actions[i]));
      setLabelText(&rowSubtitles[i], getActionSubtitle(actions[i]));
      initIcon(&rowIcons[i], getActionIcon(actions[i]), ROW_ICON_SIZE);
   }

   // snapshot the header from the summary as it is right now; the panel
   // does not refresh while open even if file-list keeps sizing folders.
   summary = *s;
   hasHeader = summary.title && summary.title[0];

   char upper[LABEL_MAX_TEXT];
   toUpper(upper, (int)sizeof(upper), strOrEmpty(summary.title));
   setLabelText(&headerTitle,    upper);
   setLabelText(&headerSubtitle, strOrEmpty(summary.subtitle));
   setLabelText(&headerDetail,   strOrEmpty(summary.detail));

   initIcon(&headerIcon, summary.icon, HEADER_ICON_SIZE);
}

// labels capture their colour at init, so a live theme switch needs this (the slab/highlight read the
// theme live and follow for free). see applyThemeToHome.
void rethemeSidepanel(void)
{
   setLabelColor(&headerTitle,    activeTheme->textPrimary);
   setLabelColor(&headerSubtitle, activeTheme->textSecondary);
   setLabelColor(&headerDetail,   activeTheme->textSecondary);
   for (int i = 0; i < SIDEPANEL_MAX_ACTIONS; i++) {
      setLabelColor(&rowTitles[i],    activeTheme->textPrimary);
      setLabelColor(&rowSubtitles[i], activeTheme->textOnHighlight);
   }
}

static void show(void)
{
   int sw = getGfxScreenWidth();
   x = (float)sw;
   setAnim(&anims, &x, (float)sw, (float)(sw - PANEL_WIDTH), SLIDE_MS, EASE_OUT_CUBIC, ANIM_ONCE, NULL);
   sidepanel.status = OVERLAY_VISIBLE;
}

static void onHidden(AnimHandle self) { (void)self; sidepanel.status = OVERLAY_HIDDEN; }

static void hide(void)
{
   setAnim(&anims, &x, x, (float)getGfxScreenWidth(), SLIDE_MS, EASE_IN_CUBIC, ANIM_ONCE, onHidden);
}

static void handleInput(void)
{
   if (isPadButtonPressed(PAD_BTN_TRIANGLE) || isPadButtonPressed(PAD_BTN_CIRCLE)) {
      hideOverlay(&sidepanel);
      return;
   }
   if (actionCount == 0) return;
   static ButtonRepeat repeat;
   if (isRepeatDue(&repeat, getPadButtonState(PAD_BTN_UP))) {
      selectedIndex = (selectedIndex - 1 + actionCount) % actionCount;
      playAudioOnce(clickSfx);
   }
   else if (isRepeatDue(&repeat, getPadButtonState(PAD_BTN_DOWN))) {
      selectedIndex = (selectedIndex + 1) % actionCount;
      playAudioOnce(clickSfx);
   }
   if (isPadButtonPressed(PAD_BTN_CROSS)) {
      SelectionAction picked = actions[selectedIndex];
      hideOverlay(&sidepanel);
      if (actionHandler) actionHandler(picked);
   }
}

static void update(void)
{
   handleInput();
   updateAnim(&anims);
}

static void draw(void)
{
   int px = (int)x;
   int sw = getGfxScreenWidth();
   int sh = getGfxScreenHeight();

   fillGfxRectangle(px, 0, sw - px, sh, activeTheme->menuFill);
   fillGfxRectangle(px, 0, PANEL_BORDER, sh, activeTheme->menuBorder);

   if (hasHeader) {
      drawIcon(&headerIcon, px + ICON_X, ICON_Y, activeTheme->textPrimary);
      drawLabelAt(&headerTitle,    px + TEXT_X, TITLE_Y);
      drawLabelAt(&headerSubtitle, px + TEXT_X, SUBTITLE_Y);
      drawLabelAt(&headerDetail,   px + TEXT_X, DETAIL_Y);
   }

   int rowX = px + ROW_X;
   int rowOriginY = hasHeader ? HEADER_HEIGHT : TOP_NO_HEADER;
   for (int i = 0; i < actionCount; i++) {
      int rowY = rowOriginY + i * ROW_HEIGHT;
      if (i == selectedIndex)
         drawGfxBox(rowX, rowY, ROW_WIDTH, ROW_HEIGHT, activeTheme->borderThickness, activeTheme->highlightFill, activeTheme->highlightBorder);
      drawIcon(&rowIcons[i],       rowX + ROW_ICON_OFFSET, rowY + ROW_ICON_OFFSET, activeTheme->textPrimary);
      drawLabelAt(&rowTitles[i],    rowX + ROW_TEXT_X, rowY + ROW_TITLE_Y);
      drawLabelAt(&rowSubtitles[i], rowX + ROW_TEXT_X, rowY + ROW_SUBTITLE_Y);
   }
}

static void term(void)
{
   cancelAllAnims(&anims);
   freeLabel(&headerTitle);
   freeLabel(&headerSubtitle);
   freeLabel(&headerDetail);
   for (int i = 0; i < SIDEPANEL_MAX_ACTIONS; i++) {
      freeLabel(&rowTitles[i]);
      freeLabel(&rowSubtitles[i]);
   }
   closeFont(&font);
   sidepanel.status = OVERLAY_TERMINATED;
}

Overlay sidepanel = { show, hide, update, draw, term, OVERLAY_TERMINATED };