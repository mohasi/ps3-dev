// text-editor-overlay - full-screen text viewer with line numbers.
// Slice 1: read-only. Loads the whole file into memory, splits it in place on
// '\n' / '\r\n' into a pointer-per-line array, and shows a scrollable,
// line-numbered view in a fixed-position panel below a filename header, with
// its own Exit footer row. D-pad moves the caret between characters and lines,
// scrolling (vertically and, since lines aren't wrapped, horizontally) just
// enough to keep it in view. Circle closes. No editing or search yet.
#include "overlays/text-editor-overlay.h"
#include "gfx.h"
#include "pad.h"
#include "font.h"
#include "colors.h"
#include "ui/label.h"
#include "ui/image.h"
#include "ui/button.h"
#include "ui/slice.h"
#include "ui/scrollbar.h"
#include "sprite-regions.h"
#include "button-repeat.h"
#include "dynarray.h"
#include "vfs.h"
#include "format.h"
#include "dbg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sys_time.h>

#define FONT_PATH "/dev_hdd0/game/FILEMGR01/USRDIR/JetBrainsMono-Regular.ttf"

// header chrome (file icon + name), drawn above the panel at fixed screen coordinates
#define HEADER_ICON_X     39
#define HEADER_ICON_Y     37
#define HEADER_ICON_W     40
#define HEADER_ICON_H     46
#define HEADER_NAME_X     98
#define HEADER_NAME_Y     49   // mockup Y (52) - 3, to match the body/gutter centering correction
#define HEADER_NAME_SIZE  24
#define HEADER_NAME_WIDTH 1400
#define HEADER_STAR_GAP   13   // gap from the filename's measured width to the asterisk
#define HEADER_STAR_Y     (HEADER_NAME_Y - 4)

// panel (the bordered text area), also fixed at design resolution (1920x1080)
#define PANEL_X       37
#define PANEL_Y       102
#define PANEL_W       1846
#define PANEL_H       863
#define HIGHLIGHT_CAP 7   // highlight sprite (16x16) 9-slice corner cap

#define FONT_SIZE  22
#define ROW_HEIGHT 48   // also the (non-wrapped) line-number box height

// panel-relative layout: gutter (right-aligned line numbers), then a gap, then body text
#define PANEL_PAD           16   // inner padding around the gutter+text content
#define GUTTER_W            80
#define GUTTER_TEXT_PAD     22   // gap between the right-aligned number and the gutter's right edge
#define TEXT_GAP            15   // gap between the gutter's right edge and the body text
#define GUTTER_RIGHT_OFFSET (PANEL_PAD + GUTTER_W - GUTTER_TEXT_PAD)
#define TEXT_OFFSET_X       (PANEL_PAD + GUTTER_W + TEXT_GAP)
#define GUTTER_BG_X (PANEL_X + 5)            // 5px clear of the panel's border, which it was overlapping
#define GUTTER_BG_W (PANEL_PAD + GUTTER_W)

// separator above the (future) bottom status row
#define SEPARATOR_X1 42
#define SEPARATOR_X2 1878
#define SEPARATOR_Y  893
#define SEPARATOR_H  1

#define EDITOR_PAGE_SIZE (((SEPARATOR_Y - PANEL_Y) - PANEL_PAD) / ROW_HEIGHT)

// own footer row (Exit only for now - no edit handler yet), independent of the
// file list's shared footer widget
#define FOOTER_X         51
#define FOOTER_Y        998
#define FOOTER_TEXT_SIZE 20

// scrollbar: a pill-shaped thumb (SPRITE_VERTICAL_PILL, 10x31 native - a 5px-radius
// capsule) in a taller, wider track. thumb height shrinks as the document grows
// past one page; thumb width stays native, centred in the track.
#define SCROLLBAR_X           1847
#define SCROLLBAR_Y            129
#define SCROLLBAR_W             14
#define SCROLLBAR_H            742
#define SCROLLBAR_THUMB_W       10
#define SCROLLBAR_THUMB_CAP      5   // the pill's rounded-end radius (half its native width)
#define SCROLLBAR_THUMB_MIN_H   40

// body text area stops short of the scrollbar track rather than running under it
#define TEXT_GAP_BEFORE_SCROLLBAR 10
#define BODY_WIDTH                (SCROLLBAR_X - PANEL_X - TEXT_OFFSET_X - TEXT_GAP_BEFORE_SCROLLBAR)

// horizontal scroll: the whole viewport shifts together, in whole characters
// (the font is monospace, so a character-column offset maps to pixels exactly).
// EDGE_MARGIN_COLS keeps the caret that many columns clear of either visible edge.
#define EDGE_MARGIN_COLS 4

#define COLOR_SCRIM              0xFF000000u
#define COLOR_PANEL_BG           0xFF01142Bu
#define COLOR_SEPARATOR          0x40FFFFFFu
#define COLOR_GUTTER_BG_ACTIVE   0xFF031B38u
#define COLOR_LINE_NUMBER        0xFF647185u
#define COLOR_LINE_NUMBER_ACTIVE COLOR_WHITE

// caret: a blinking bar between characters, one character-column wide
#define CARET_W            2
#define CARET_BLINK_US     500000
#define CARET_SETTLE_US    300000   // holds solid for this long after a move, so scrolling doesn't look like blinking

// status bar: size | position | encoding | line ending, below the panel's bottom
// separator. right-aligned within its block, so it stays flush with the panel's
// right edge regardless of how wide the segments are.
#define STATUS_X         1330
#define STATUS_Y          917
#define STATUS_W          522
#define STATUS_H           27
#define STATUS_TEXT_SIZE   20
#define STATUS_GAP         40   // gap between adjacent segments; each "|" sits centred in it
#define STATUS_TEXT_Y      (STATUS_Y + (STATUS_H - STATUS_TEXT_SIZE) / 2 + 2)

typedef enum { LINE_ENDING_NONE, LINE_ENDING_LF, LINE_ENDING_CRLF, LINE_ENDING_CR } LineEnding;

static struct {
   char  *buffer;        // whole file content, NUL-terminated; '\n'/'\r' replaced with '\0' in place
   char **lines;         // pointers into buffer, one per line
   int    lineCount;
   int    lineCapacity;

   int cursorLine;   // highlighted line (0-based)
   int cursorCol;    // caret's character column within that line (0..line length)
   int desiredCol;   // sticky column for vertical moves - see moveCursor
   int scrollTop;    // first visible line (0-based)
   int scrollCol;    // first visible character column, shared by every visible row
   int rowsStale;    // visible row labels need rebuilding

   uint64_t   lastMoveUs;    // when the cursor last moved; the caret holds solid for a bit after
   uint64_t   fileSize;      // for the status bar's size segment
   LineEnding lineEnding;    // the first line ending style seen in the file
} state;

static Font  monoFont;     // body text + line numbers
static Font  headerFont;   // filename header
static Image headerIcon;
static Label headerNameLabel;
static Label headerStarLabel;
static NineSlice panel;
static Scrollbar scrollbar;
static Button exitFooterButton;
static int   charWidth;    // monospace glyph advance in px, so column <-> pixel is exact
static int   visibleCols;  // how many character columns BODY_WIDTH holds
static int   caretHeight;  // the font's line-box height - fixed, not per-line tex.h (0 on a blank line)

// one row of visible labels: the line number in its dim and active colour
// (pre-rendered, like file-list's checkbox/checkedBox pair - draw() just
// picks the right one instead of recolouring at draw time), plus the body
// text. numberOffsetX right-aligns the number within the gutter.
static Label numberLabels[EDITOR_PAGE_SIZE];
static Label activeNumberLabels[EDITOR_PAGE_SIZE];
static Label bodyLabels[EDITOR_PAGE_SIZE];
static int   numberOffsetX[EDITOR_PAGE_SIZE];

// status bar segments: size and line ending are set once per file open; position
// is refreshed every cursor move; encoding is a fixed label (no detection yet).
static Label sizeLabel, positionLabel, encodingLabel, lineEndingLabel, pipeLabel;

static ButtonRepeat scrollUpRepeat, scrollDownRepeat, scrollLeftRepeat, scrollRightRepeat;

static const char *lineEndingText(LineEnding le)
{
   switch (le) {
      case LINE_ENDING_CRLF: return "CRLF";
      case LINE_ENDING_CR:   return "CR";
      case LINE_ENDING_LF:   return "LF";
      default:               return "-";
   }
}

// lays out the 4 status segments left to right, each followed by a "|" centred
// in a STATUS_GAP-wide space before the next segment, then shifts the whole row
// right so it ends flush with the block's right edge (STATUS_X + STATUS_W).
static void drawStatusBar(void)
{
   Label *segments[] = { &sizeLabel, &positionLabel, &encodingLabel, &lineEndingLabel };

   int totalWidth = 3 * (STATUS_GAP + pipeLabel.tt.tex.w);
   for (int i = 0; i < 4; i++) totalWidth += segments[i]->tt.tex.w;

   int x = STATUS_X + STATUS_W - totalWidth;
   for (int i = 0; i < 4; i++) {
      drawLabelAt(segments[i], x, STATUS_TEXT_Y);
      x += segments[i]->tt.tex.w;
      if (i == 3) break;
      x += STATUS_GAP / 2;
      drawLabelAt(&pipeLabel, x, STATUS_TEXT_Y);
      x += pipeLabel.tt.tex.w + STATUS_GAP / 2;
   }
}

// keeps the caret at least EDGE_MARGIN_COLS columns clear of either visible edge,
// scrolling the whole viewport (every visible row) just enough to do so.
static void ensureCursorVisibleHorizontally(void)
{
   if (state.cursorCol - state.scrollCol < EDGE_MARGIN_COLS)
      state.scrollCol = state.cursorCol - EDGE_MARGIN_COLS;
   else if (state.cursorCol - state.scrollCol > visibleCols - 1 - EDGE_MARGIN_COLS)
      state.scrollCol = state.cursorCol - (visibleCols - 1 - EDGE_MARGIN_COLS);

   if (state.scrollCol < 0) state.scrollCol = 0;
}

// centres a label's actual rendered glyph height within a ROW_HEIGHT row -
// the font's line-box height isn't FONT_SIZE, so this reads the real
// rendered texture height rather than assuming one. the +3 is a manual
// correction against the mockup (the glyph ink doesn't sit dead-centre in
// its line box).
static int centerInRow(const Label *l, int rowY)
{
   return rowY + (ROW_HEIGHT - l->tt.tex.h) / 2 + 3;
}

// solid for CARET_SETTLE_US after the cursor last moved (so scrolling doesn't read as
// a glitchy blink), then blinks on a fresh phase once it's settled on a row.
static int isCaretVisible(void)
{
   uint64_t sinceMove = sys_time_get_system_time() - state.lastMoveUs;
   if (sinceMove < CARET_SETTLE_US) return 1;
   return ((sinceMove - CARET_SETTLE_US) / CARET_BLINK_US) % 2 == 0;
}

static void freeDocument(void)
{
   free(state.buffer);
   free(state.lines);
   state.buffer       = NULL;
   state.lines        = NULL;
   state.lineCount    = 0;
   state.lineCapacity = 0;
}

static void addLine(char *start)
{
   if (!growArray(state.lines, &state.lineCapacity, state.lineCount + 1)) return;
   state.lines[state.lineCount++] = start;
}

// splits buffer in place on '\n' / '\r\n', NUL-terminating each line. a
// trailing newline yields one extra empty final line, matching how most
// editors count lines. records the first line ending style seen into
// state.lineEnding, for the status bar.
static void splitDocumentLines(char *buffer)
{
   state.lineEnding = LINE_ENDING_NONE;

   char *lineStart = buffer;
   char *p = buffer;
   while (*p) {
      if (*p == '\n' || *p == '\r') {
         char *terminator = p;
         LineEnding found = LINE_ENDING_LF;
         if (*p == '\r' && p[1] == '\n') { found = LINE_ENDING_CRLF; p++; }   // CRLF: swallow both
         else if (*p == '\r')              found = LINE_ENDING_CR;
         if (state.lineEnding == LINE_ENDING_NONE) state.lineEnding = found;
         p++;
         *terminator = '\0';
         addLine(lineStart);
         lineStart = p;
      } else {
         p++;
      }
   }
   addLine(lineStart);
}

int openTextEditor(const char *path)
{
   VfsStat fileStat;
   if (statPath(path, &fileStat) != 0) return -1;

   char *buffer = (char *)malloc((size_t)fileStat.size + 1);
   if (!buffer) return -1;

   if (readFile(path, buffer, (int)fileStat.size + 1) < 0) {
      free(buffer);
      return -1;
   }

   freeDocument();
   state.buffer   = buffer;
   state.fileSize = fileStat.size;
   splitDocumentLines(state.buffer);

   state.cursorLine  = 0;
   state.cursorCol   = 0;
   state.desiredCol  = 0;
   state.scrollTop   = 0;
   state.scrollCol   = 0;
   state.rowsStale   = 1;
   state.lastMoveUs  = sys_time_get_system_time();

   const char *fileName = getBaseName(path);
   setLabelText(&headerNameLabel, fileName);
   float nameWidth = measureFontText(&headerFont, HEADER_NAME_SIZE, fileName);
   moveLabel(&headerStarLabel, HEADER_NAME_X + (int)nameWidth + HEADER_STAR_GAP, HEADER_STAR_Y);

   char sizeText[24];
   formatSize(state.fileSize, sizeText);
   setLabelText(&sizeLabel, sizeText);
   setLabelText(&lineEndingLabel, lineEndingText(state.lineEnding));
   setLabelText(&positionLabel, "Ln 1, Col 1");

   showOverlay(&textEditorOverlay);
   return 0;
}

static void show(void) { textEditorOverlay.status = OVERLAY_VISIBLE; }

static void hide(void)
{
   freeDocument();
   textEditorOverlay.status = OVERLAY_HIDDEN;
}

static void rebuildRows(void)
{
   char numberText[12];

   for (int i = 0; i < EDITOR_PAGE_SIZE; i++) {
      int idx = state.scrollTop + i;
      if (idx >= state.lineCount) {
         setLabelText(&numberLabels[i], "");
         setLabelText(&activeNumberLabels[i], "");
         setLabelText(&bodyLabels[i], "");
         continue;
      }

      snprintf(numberText, sizeof numberText, "%d", idx + 1);
      setLabelText(&numberLabels[i], numberText);
      setLabelText(&activeNumberLabels[i], numberText);
      numberOffsetX[i] = GUTTER_RIGHT_OFFSET - (int)measureFontText(&monoFont, FONT_SIZE, numberText);

      // the whole viewport scrolls horizontally together: every row starts at the
      // same character column, clamped to its own (possibly shorter) length.
      int lineLen  = (int)strlen(state.lines[idx]);
      int startCol = state.scrollCol < lineLen ? state.scrollCol : lineLen;
      setLabelText(&bodyLabels[i], state.lines[idx] + startCol);
   }

   char positionText[32];
   snprintf(positionText, sizeof positionText, "Ln %d, Col %d", state.cursorLine + 1, state.cursorCol + 1);
   setLabelText(&positionLabel, positionText);

   state.rowsStale = 0;
}

// moves the highlighted line by delta, scrolling vertically just enough to keep it
// visible. the caret's column snaps to the end of a shorter line, but desiredCol
// itself is left untouched - so it snaps back once a line long enough to hold it
// is reached again, instead of forgetting where the caret "should" be.
static void moveCursor(int delta)
{
   if (state.lineCount == 0) return;

   int newLine = state.cursorLine + delta;
   if (newLine < 0) newLine = 0;
   if (newLine >= state.lineCount) newLine = state.lineCount - 1;
   if (newLine == state.cursorLine) return;
   state.cursorLine = newLine;
   state.lastMoveUs = sys_time_get_system_time();

   int lineLen = (int)strlen(state.lines[state.cursorLine]);
   state.cursorCol = state.desiredCol < lineLen ? state.desiredCol : lineLen;

   if (state.cursorLine < state.scrollTop)
      state.scrollTop = state.cursorLine;
   else if (state.cursorLine >= state.scrollTop + EDITOR_PAGE_SIZE)
      state.scrollTop = state.cursorLine - EDITOR_PAGE_SIZE + 1;

   ensureCursorVisibleHorizontally();
   state.rowsStale = 1;
}

// moves the caret within the current line by delta characters; unlike moveCursor,
// this also resets desiredCol - an explicit horizontal move sets a new "home" column.
static void moveCursorHorizontal(int delta)
{
   if (state.lineCount == 0) return;

   int lineLen = (int)strlen(state.lines[state.cursorLine]);
   int newCol  = state.cursorCol + delta;
   if (newCol < 0) newCol = 0;
   if (newCol > lineLen) newCol = lineLen;
   if (newCol == state.cursorCol) return;

   state.cursorCol  = newCol;
   state.desiredCol = newCol;
   state.lastMoveUs = sys_time_get_system_time();

   ensureCursorVisibleHorizontally();
   state.rowsStale = 1;
}

static void update(void)
{
   if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
      hideOverlay(&textEditorOverlay);
      return;
   }

   if (isRepeatDue(&scrollDownRepeat, getPadButtonState(PAD_BTN_DOWN))) moveCursor(1);
   else if (isRepeatDue(&scrollUpRepeat, getPadButtonState(PAD_BTN_UP))) moveCursor(-1);
   else if (isRepeatDue(&scrollRightRepeat, getPadButtonState(PAD_BTN_RIGHT))) moveCursorHorizontal(1);
   else if (isRepeatDue(&scrollLeftRepeat, getPadButtonState(PAD_BTN_LEFT))) moveCursorHorizontal(-1);

   if (state.rowsStale) rebuildRows();
}

static void draw(void)
{
   fillGfxRectangle(0, 0, getGfxScreenWidth(), getGfxScreenHeight(), COLOR_SCRIM);

   drawImage(&headerIcon);
   drawLabel(&headerNameLabel);
   drawLabel(&headerStarLabel);

   fillGfxRectangle(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, COLOR_PANEL_BG);
   drawNineSlice(&panel);
   fillGfxRectangle(SEPARATOR_X1, SEPARATOR_Y, SEPARATOR_X2 - SEPARATOR_X1, SEPARATOR_H, COLOR_SEPARATOR);

   for (int i = 0; i < EDITOR_PAGE_SIZE; i++) {
      int idx = state.scrollTop + i;
      if (idx >= state.lineCount) break;

      int rowY      = PANEL_Y + PANEL_PAD + i * ROW_HEIGHT;
      int isCurrent = (idx == state.cursorLine);

      if (isCurrent) fillGfxRectangle(GUTTER_BG_X, rowY, GUTTER_BG_W, ROW_HEIGHT, COLOR_GUTTER_BG_ACTIVE);

      Label *numberLabel = isCurrent ? &activeNumberLabels[i] : &numberLabels[i];
      drawLabelAt(numberLabel, PANEL_X + numberOffsetX[i], centerInRow(numberLabel, rowY));
      drawLabelAt(&bodyLabels[i], PANEL_X + TEXT_OFFSET_X, centerInRow(&bodyLabels[i], rowY));

      if (isCurrent && isCaretVisible()) {
         int caretY = rowY + (ROW_HEIGHT - caretHeight) / 2 + 3;
         int caretX = PANEL_X + TEXT_OFFSET_X + (state.cursorCol - state.scrollCol) * charWidth;
         fillGfxRectangle(caretX, caretY, CARET_W, caretHeight, COLOR_WHITE);
      }
   }

   drawScrollbar(&scrollbar, state.lineCount, EDITOR_PAGE_SIZE, state.scrollTop);
   drawStatusBar();

   drawButton(&exitFooterButton);
}

static void term(void)
{
   freeDocument();
   for (int i = 0; i < EDITOR_PAGE_SIZE; i++) {
      freeLabel(&numberLabels[i]);
      freeLabel(&activeNumberLabels[i]);
      freeLabel(&bodyLabels[i]);
   }
   freeLabel(&headerNameLabel);
   freeLabel(&headerStarLabel);
   freeLabel(&sizeLabel);
   freeLabel(&positionLabel);
   freeLabel(&encodingLabel);
   freeLabel(&lineEndingLabel);
   freeLabel(&pipeLabel);
   freeButton(&exitFooterButton);
   closeFont(&monoFont);
   closeFont(&headerFont);
   textEditorOverlay.status = OVERLAY_TERMINATED;
}

Overlay textEditorOverlay = { show, hide, update, draw, term, OVERLAY_TERMINATED };

void initTextEditorOverlay(GfxTexture sprites)
{
   monoFont   = openFontFile(FONT_PATH);
   headerFont = openSystemFont(FONT_POP);
   if (!monoFont.open) logError("[text-editor] failed to open mono font: %s\n", FONT_PATH);

   // snap every glyph advance to a whole pixel: without this, the font's true
   // (fractional) advance drifts from our integer charWidth*column math a little
   // more each character, until the caret visibly lags behind the real glyphs.
   setFontMetrics(&monoFont, FONT_METRICS_GRID_CEIL, 1.0f);
   charWidth = (int)measureFontText(&monoFont, FONT_SIZE, "0");   // monospace: any glyph is representative
   if (charWidth <= 0) charWidth = 1;                             // font failed to open: avoid a divide-by-zero below
   visibleCols = BODY_WIDTH / charWidth;

   // the caret's height is the font's line-box height, not any one line's actual
   // ink extent (which is 0 for a blank line, and varies with ascenders/descenders
   // otherwise) - so it stays a constant size regardless of line content.
   TextTexture scratch;
   TextEnd     end = {0};
   memset(&scratch, 0, sizeof scratch);
   renderFontEx(&scratch, &monoFont, FONT_SIZE, "0", COLOR_WHITE, AUTO, TEXT_NOWRAP, NULL, &end);
   caretHeight = end.lineHeight;
   freeTextTexture(&scratch);

   initImage(&headerIcon, sprites, HEADER_ICON_X, HEADER_ICON_Y, HEADER_ICON_W, HEADER_ICON_H,
             spriteRegions[SPRITE_TEXT], GFX_FILTER_LINEAR);
   initLabel(&headerNameLabel, &headerFont, HEADER_NAME_X, HEADER_NAME_Y, HEADER_NAME_WIDTH, AUTO,
             HEADER_NAME_SIZE, COLOR_WHITE, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&headerStarLabel, &headerFont, 0, 0, 20, AUTO, HEADER_NAME_SIZE, COLOR_RED, TEXT_NOWRAP, "*");

   initNineSlice(&panel, sprites, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, spriteRegions[SPRITE_HIGHLIGHT], HIGHLIGHT_CAP, HIGHLIGHT_CAP);
   initScrollbar(&scrollbar, sprites, SCROLLBAR_X, SCROLLBAR_Y, SCROLLBAR_W, SCROLLBAR_H,
                 spriteRegions[SPRITE_VERTICAL_PILL], SCROLLBAR_THUMB_W, SCROLLBAR_THUMB_CAP, SCROLLBAR_THUMB_MIN_H);

   Image exitIcon;
   Label exitLabel;
   initImage(&exitIcon, sprites, 0, 0, AUTO, AUTO, spriteRegions[SPRITE_CIRCLE], GFX_FILTER_LINEAR);
   initLabel(&exitLabel, &headerFont, 0, 0, AUTO, AUTO, FOOTER_TEXT_SIZE, COLOR_WHITE, TEXT_NOWRAP, "Exit");
   initButton(&exitFooterButton, exitIcon, exitLabel, BUTTON_ENABLED);   // closing is handled directly in update()
   moveButton(&exitFooterButton, FOOTER_X, FOOTER_Y);

   for (int i = 0; i < EDITOR_PAGE_SIZE; i++) {
      initLabel(&numberLabels[i],       &monoFont, 0, 0, GUTTER_W,   AUTO, FONT_SIZE, COLOR_LINE_NUMBER,        TEXT_NOWRAP, "");
      initLabel(&activeNumberLabels[i], &monoFont, 0, 0, GUTTER_W,   AUTO, FONT_SIZE, COLOR_LINE_NUMBER_ACTIVE, TEXT_NOWRAP, "");
      initLabel(&bodyLabels[i],         &monoFont, 0, 0, BODY_WIDTH, AUTO, FONT_SIZE, COLOR_WHITE,              TEXT_NOWRAP_ELLIPSIS, "");
   }

   initLabel(&sizeLabel,        &headerFont, 0, 0, AUTO, AUTO, STATUS_TEXT_SIZE, COLOR_WHITE,       TEXT_NOWRAP, "");
   initLabel(&positionLabel,    &headerFont, 0, 0, AUTO, AUTO, STATUS_TEXT_SIZE, COLOR_WHITE,       TEXT_NOWRAP, "");
   initLabel(&encodingLabel,    &headerFont, 0, 0, AUTO, AUTO, STATUS_TEXT_SIZE, COLOR_WHITE,       TEXT_NOWRAP, "UTF-8");
   initLabel(&lineEndingLabel,  &headerFont, 0, 0, AUTO, AUTO, STATUS_TEXT_SIZE, COLOR_WHITE,       TEXT_NOWRAP, "");
   initLabel(&pipeLabel,        &headerFont, 0, 0, AUTO, AUTO, STATUS_TEXT_SIZE, COLOR_LINE_NUMBER, TEXT_NOWRAP, "|");
}
