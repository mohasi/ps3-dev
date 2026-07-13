// hex-viewer-overlay - full-screen hex/ASCII viewer with a fixed offset/column
// header, an address gutter, and a byte cursor mirrored between the hex and
// ASCII columns. Streams the visible page straight from the file on every
// scroll (via vfs seek/read) instead of loading it whole, so it stays safe on
// huge files. L1/L2 and R1/R2 page up/down (L2/R2 only while the pad is
// closed). Edit (Cross) opens a dedicated hex pad (0-9/A-F, Square is
// backspace): hex digits overwrite the byte under the cursor two nibbles at a
// time, previewing after the first nibble rather than waiting for the second;
// Edit is disabled while the pad is already open, and Exit is disabled too -
// Circle closes the pad instead of the viewer while it's open. Edits land in
// a small in-memory offset->byte
// patch list, overlaid onto each page as it's read. Save (Start) flushes just
// those patched bytes straight to their file offsets - works even while the
// pad is open - and is only enabled while there are unsaved edits; saving
// clears the patch list and itself disables again. No search yet, and no byte
// insertion/deletion - only overwrite.
#include "overlays/hex-viewer-overlay.h"
#include "gfx.h"
#include "pad.h"
#include "font.h"
#include "colors.h"
#include "ui/label.h"
#include "ui/image.h"
#include "ui/button.h"
#include "ui/slice.h"
#include "ui/scrollbar.h"
#include "ui/hex-pad.h"
#include "ui/console-glyphs.h"
#include "sprite-regions.h"
#include "button-repeat.h"
#include "dynarray.h"
#include "vfs.h"
#include "dbg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONT_PATH "/dev_hdd0/game/FILEMGR01/USRDIR/JetBrainsMono-Regular.ttf"

// header chrome (file icon + name), drawn above the panel - same layout as
// text-editor-overlay's header, duplicated rather than shared (see the UI
// component audit: two headers that only coincidentally look alike aren't
// worth forcing into one component).
#define HEADER_ICON_X     39
#define HEADER_ICON_Y     37
#define HEADER_ICON_W     40
#define HEADER_ICON_H     46
#define HEADER_NAME_X     98
#define HEADER_NAME_Y     49
#define HEADER_NAME_SIZE  24
#define HEADER_NAME_WIDTH 1400
#define HEADER_STAR_GAP   13
#define HEADER_STAR_Y     (HEADER_NAME_Y - 4)

// panel (the bordered hex area), fixed at design resolution (1920x1080) - same
// bounds as text-editor-overlay's panel.
#define PANEL_X       37
#define PANEL_Y       102
#define PANEL_W       1846
#define PANEL_H       883
#define HIGHLIGHT_CAP 7

#define FONT_SIZE  22
#define ROW_HEIGHT 48
#define PANEL_PAD  16

// separator above the footer row
#define SEPARATOR_X1 42
#define SEPARATOR_X2 1878
#define SEPARATOR_Y  913
#define SEPARATOR_H  1

// byte grid: 16 bytes/row, each cell "XX " (2 hex digits + a gap), then a
// wider gap before the ASCII column. address column is 8 hex digits wide.
#define HEX_BYTES_PER_ROW 16
#define ADDRESS_CHARS      8
#define BYTE_CELL_CHARS    3
#define HEX_AREA_CHARS     (HEX_BYTES_PER_ROW * BYTE_CELL_CHARS)
#define COLUMN_GAP_CHARS   2

// row 0 of the panel is the fixed "Offset / 00 01 .. 0F" header, which never
// scrolls; byte rows start one ROW_HEIGHT below it.
#define HEX_PAGE_SIZE (((SEPARATOR_Y - PANEL_Y) - PANEL_PAD) / ROW_HEIGHT - 1)

#define FOOTER_X          51
#define FOOTER_Y         1018
#define FOOTER_TEXT_SIZE  20
#define FOOTER_BUTTON_GAP 50
#define FOOTER_ICON_H     32   // console button glyphs are scaled to this height, aspect preserved

#define SCROLLBAR_X           1847
#define SCROLLBAR_Y            129
#define SCROLLBAR_W             14
#define SCROLLBAR_H            762
#define SCROLLBAR_THUMB_W       10
#define SCROLLBAR_THUMB_CAP      5
#define SCROLLBAR_THUMB_MIN_H   40

#define GUTTER_BG_X (PANEL_X + 5)   // 5px clear of the panel's border

#define COLOR_SCRIM            0xFF000000u
#define COLOR_PANEL_BG         0xFF01142Bu
#define COLOR_SEPARATOR        0x40FFFFFFu
#define COLOR_ACTIVE_BG        0xFF031B38u
#define COLOR_ADDRESS          0xFF647185u
#define COLOR_ADDRESS_ACTIVE   COLOR_WHITE
#define COLOR_HEADER_TEXT      0xFF647185u

static const char HEX_DIGITS[] = "0123456789ABCDEF";

// a single in-memory byte override, keyed by absolute file offset.
typedef struct {
   uint64_t offset;
   uint8_t  value;
} ByteEdit;

static struct {
   VfsFile  file;
   int      fileOpen;
   uint64_t fileSize;
   int      dirty;       // any byte edited since open (shows the header's unsaved-changes asterisk, enables Save)
   char     path[MAX_PATH_LEN];   // the open file's path, for Save

   int cursorRow;   // 0-based row index; row*16 is that row's first byte offset
   int cursorCol;   // 0-15, the selected byte within the row
   int scrollTop;   // first visible row
   int rowsStale;   // visible row labels need rebuilding

   int      pendingNibble;        // 1 while a byte's high nibble is buffered, waiting for the low nibble
   uint64_t pendingOffset;        // the byte offset the pending nibble belongs to
   uint8_t  pendingHighNibble;
   uint8_t  pendingOriginalByte;  // the byte's value before the pending nibble's preview edit, for Backspace to restore

   ByteEdit *edits;         // unsaved overwrites, applied on top of each page as it's read
   int       editCount;
   int       editCapacity;
} state;

static Font  monoFont;     // hex/ASCII body + gutter + column header
static Font  headerFont;   // filename header
static Image headerIcon;
static Label headerNameLabel;
static Label headerStarLabel;
static NineSlice panel;
static Scrollbar scrollbar;
static Button exitFooterButton;
static Button editFooterButton;
static Button saveFooterButton;

static Label addressHeaderLabel;   // "Offset"
static Label hexHeaderLabel;       // "00 01 02 ... 0F"

static Label addressLabels[HEX_PAGE_SIZE];
static Label activeAddressLabels[HEX_PAGE_SIZE];
static Label hexLabels[HEX_PAGE_SIZE];
static Label asciiLabels[HEX_PAGE_SIZE];

static uint8_t pageBytes[HEX_PAGE_SIZE * HEX_BYTES_PER_ROW];

static int charWidth;       // monospace glyph advance in px
static int addressAreaX;    // PANEL_X + PANEL_PAD
static int hexAreaX;        // right of the address column + a gap
static int asciiAreaX;      // right of the hex columns + a gap
static int addressBgWidth;  // width of the active-row background over the address column

static ButtonRepeat scrollUpRepeat, scrollDownRepeat, scrollLeftRepeat, scrollRightRepeat, pageUpRepeat, pageDownRepeat;

static uint64_t getTotalRowCount(void)
{
   if (state.fileSize == 0) return 0;
   return (state.fileSize + HEX_BYTES_PER_ROW - 1) / HEX_BYTES_PER_ROW;
}

// how many valid bytes row occupies - HEX_BYTES_PER_ROW for every row except
// (possibly) the last, which may be short.
static int getBytesInRow(uint64_t row)
{
   uint64_t rowOffset = row * HEX_BYTES_PER_ROW;
   uint64_t remaining = state.fileSize - rowOffset;
   return remaining > HEX_BYTES_PER_ROW ? HEX_BYTES_PER_ROW : (int)remaining;
}

// keeps the cursor within the document and off the padding of a short last row.
static void clampCursor(void)
{
   uint64_t rows = getTotalRowCount();
   if (rows == 0) { state.cursorRow = 0; state.cursorCol = 0; return; }
   if (state.cursorRow >= (int)rows) state.cursorRow = (int)rows - 1;
   int maxCol = getBytesInRow(state.cursorRow) - 1;
   if (state.cursorCol > maxCol) state.cursorCol = maxCol;
}

static void ensureRowVisible(void)
{
   if (state.cursorRow < state.scrollTop)
      state.scrollTop = state.cursorRow;
   else if (state.cursorRow >= state.scrollTop + HEX_PAGE_SIZE)
      state.scrollTop = state.cursorRow - HEX_PAGE_SIZE + 1;
}

// moves the cursor by delta rows, keeping the same byte column (clamped to
// the destination row's length).
static void moveCursorRows(int delta)
{
   uint64_t rows = getTotalRowCount();
   if (rows == 0) return;

   int newRow = state.cursorRow + delta;
   if (newRow < 0) newRow = 0;
   if (newRow >= (int)rows) newRow = (int)rows - 1;
   if (newRow == state.cursorRow) return;

   state.cursorRow = newRow;
   clampCursor();
   ensureRowVisible();
   state.rowsStale = 1;
}

// moves the cursor by delta bytes, wrapping into the previous/next row at
// either edge of the current row (clamped at the very first/last byte).
static void moveCursorBytes(int delta)
{
   uint64_t rows = getTotalRowCount();
   if (rows == 0) return;

   int newRow = state.cursorRow;
   int newCol = state.cursorCol + delta;
   if (newCol < 0) {
      if (newRow > 0) { newRow--; newCol = HEX_BYTES_PER_ROW - 1; }
      else newCol = 0;
   } else if (newCol >= HEX_BYTES_PER_ROW) {
      if (newRow < (int)rows - 1) { newRow++; newCol = 0; }
      else newCol = HEX_BYTES_PER_ROW - 1;
   }
   if (newRow == state.cursorRow && newCol == state.cursorCol) return;

   state.cursorRow = newRow;
   state.cursorCol = newCol;
   clampCursor();
   ensureRowVisible();
   state.rowsStale = 1;
}

// records value as an override for offset, replacing any prior edit at that
// same offset. edits are never written to disk here - just held in memory
// until a save action exists.
static void recordEdit(uint64_t offset, uint8_t value)
{
   for (int i = 0; i < state.editCount; i++) {
      if (state.edits[i].offset == offset) { state.edits[i].value = value; return; }
   }
   if (!growArray(state.edits, &state.editCapacity, state.editCount + 1)) return;
   state.edits[state.editCount].offset = offset;
   state.edits[state.editCount].value  = value;
   state.editCount++;
}

// applies any pending edits that fall within [base, base + count) onto buffer
// (buffer holds the freshly-read bytes for that range, indexed from 0).
static void applyEdits(uint8_t *buffer, uint64_t base, int count)
{
   for (int i = 0; i < state.editCount; i++) {
      if (state.edits[i].offset < base || state.edits[i].offset >= base + (uint64_t)count) continue;
      buffer[state.edits[i].offset - base] = state.edits[i].value;
   }
}

int openHexViewer(const char *path)
{
   VfsStat fileStat;
   if (statPath(path, &fileStat) != 0) return -1;

   VfsFile file;
   if (openFs(path, VFS_O_RDONLY, &file) != 0) return -1;

   if (state.fileOpen) closeFs(&state.file);
   state.file     = file;
   state.fileOpen = 1;
   state.fileSize = fileStat.size;
   state.dirty    = 0;
   state.editCount = 0;
   strCopy(state.path, sizeof state.path, path);

   state.cursorRow     = 0;
   state.cursorCol      = 0;
   state.scrollTop      = 0;
   state.rowsStale      = 1;
   state.pendingNibble  = 0;

   const char *fileName = getBaseName(path);
   setLabelText(&headerNameLabel, fileName);
   // headerNameLabel.tt.tex.w is the label's ACTUAL rendered width - already ellipsis-truncated
   // if fileName is too long for HEADER_NAME_WIDTH - so the star lands right after what's really
   // on screen, unlike re-measuring the untruncated fileName would.
   moveLabel(&headerStarLabel, HEADER_NAME_X + headerNameLabel.tt.tex.w + HEADER_STAR_GAP, HEADER_STAR_Y);

   showOverlay(&hexViewerOverlay);
   return 0;
}

static void show(void) { hexViewerOverlay.status = OVERLAY_VISIBLE; }

static void hide(void)
{
   if (state.fileOpen) { closeFs(&state.file); state.fileOpen = 0; }
   hexViewerOverlay.status = OVERLAY_HIDDEN;
}

// re-reads the page of bytes covering the visible rows, then reformats each
// row's address/hex/ASCII labels from it.
static void rebuildRows(void)
{
   uint64_t base = (uint64_t)state.scrollTop * HEX_BYTES_PER_ROW;
   int pageByteCount = 0;
   if (state.fileSize > base) {
      uint64_t want = (uint64_t)HEX_PAGE_SIZE * HEX_BYTES_PER_ROW;
      uint64_t remaining = state.fileSize - base;
      if (want > remaining) want = remaining;
      seekFs(&state.file, (int64_t)base, VFS_SEEK_SET);
      int64_t got = readFs(&state.file, pageBytes, want);
      pageByteCount = got > 0 ? (int)got : 0;
      applyEdits(pageBytes, base, pageByteCount);
   }

   char addressText[12];
   char hexText[HEX_AREA_CHARS + 1];
   char asciiText[HEX_BYTES_PER_ROW + 1];

   for (int i = 0; i < HEX_PAGE_SIZE; i++) {
      uint64_t rowOffset = base + (uint64_t)i * HEX_BYTES_PER_ROW;
      if (rowOffset >= state.fileSize) {
         setLabelText(&addressLabels[i], "");
         setLabelText(&activeAddressLabels[i], "");
         setLabelText(&hexLabels[i], "");
         setLabelText(&asciiLabels[i], "");
         continue;
      }

      int rowBytes = i * HEX_BYTES_PER_ROW < pageByteCount ? pageByteCount - i * HEX_BYTES_PER_ROW : 0;
      if (rowBytes > HEX_BYTES_PER_ROW) rowBytes = HEX_BYTES_PER_ROW;
      const uint8_t *row = pageBytes + i * HEX_BYTES_PER_ROW;

      snprintf(addressText, sizeof addressText, "%08X", (unsigned int)rowOffset);
      setLabelText(&addressLabels[i], addressText);
      setLabelText(&activeAddressLabels[i], addressText);

      int hexPos = 0, asciiPos = 0;
      for (int col = 0; col < HEX_BYTES_PER_ROW; col++) {
         if (col < rowBytes) {
            uint8_t byte = row[col];
            hexText[hexPos++]     = HEX_DIGITS[byte >> 4];
            hexText[hexPos++]     = HEX_DIGITS[byte & 0xF];
            asciiText[asciiPos++] = (byte >= 32 && byte < 127) ? (char)byte : '.';
         } else {
            hexText[hexPos++]     = ' ';
            hexText[hexPos++]     = ' ';
            asciiText[asciiPos++] = ' ';
         }
         hexText[hexPos++] = ' ';
      }
      hexText[hexPos]     = '\0';
      asciiText[asciiPos] = '\0';
      setLabelText(&hexLabels[i], hexText);
      setLabelText(&asciiLabels[i], asciiText);
   }

   state.rowsStale = 0;
}

static int hexDigitValue(char c)
{
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   if (c >= 'A' && c <= 'F') return c - 'A' + 10;
   return -1;
}

// the cursor byte's current displayed value (post-edits, from the cached page) -
// used to preview a half-entered nibble against the byte's real other nibble.
static uint8_t getCursorByte(void)
{
   int rowIndex = state.cursorRow - state.scrollTop;
   if (rowIndex < 0 || rowIndex >= HEX_PAGE_SIZE) return 0;
   return pageBytes[rowIndex * HEX_BYTES_PER_ROW + state.cursorCol];
}

// overwrites the byte at the cursor in the in-memory edit list (not on disk),
// then re-reads the page so the change shows immediately.
static void setCursorByte(uint8_t value)
{
   uint64_t offset = (uint64_t)state.cursorRow * HEX_BYTES_PER_ROW + state.cursorCol;
   recordEdit(offset, value);
   state.dirty     = 1;
   state.rowsStale = 1;
}

// flushes each patched byte straight to its own file offset - no whole-file
// rewrite, so this stays cheap regardless of file size. no-op if there's
// nothing to save (also guards against Save firing while visually disabled,
// or while a nibble is still half-entered - see cancelPendingNibble).
//
// stops at the first offset that fails to seek/write and drops only the edits that made it to
// disk, so a mid-save failure (full volume, media pulled) leaves the rest pending and dirty
// instead of reporting a partial write as a full success.
static void saveDocument(void)
{
   if (!state.dirty || state.editCount == 0 || state.pendingNibble) return;

   VfsFile writeHandle;
   if (openFs(state.path, VFS_O_WRONLY, &writeHandle) != 0) {
      logError("[hex-viewer] failed to open %s for save\n", state.path);
      return;
   }

   int savedCount = 0;
   while (savedCount < state.editCount) {
      const ByteEdit *edit = &state.edits[savedCount];
      if (seekFs(&writeHandle, (int64_t)edit->offset, VFS_SEEK_SET) < 0 || writeFs(&writeHandle, &edit->value, 1) != 1) {
         logError("[hex-viewer] failed to write offset %u of %s\n", (unsigned int)edit->offset, state.path);
         break;
      }
      savedCount++;
   }
   if (closeFs(&writeHandle) != 0) {
      logError("[hex-viewer] failed to commit %s\n", state.path);
      savedCount = 0;   // can't confirm anything actually landed on disk - keep every edit pending
   }

   if (savedCount > 0) {
      memmove(state.edits, state.edits + savedCount, (size_t)(state.editCount - savedCount) * sizeof(ByteEdit));
      state.editCount -= savedCount;
   }
   state.dirty = state.editCount > 0;
}

// restores the byte a pending nibble was previewing, at the offset it actually belongs to
// (not wherever the cursor happens to be now) - shared by Backspace-cancel and by a cursor
// move invalidating a still-pending nibble.
static void cancelPendingNibble(void)
{
   recordEdit(state.pendingOffset, state.pendingOriginalByte);
   state.dirty         = 1;
   state.rowsStale     = 1;
   state.pendingNibble = 0;
}

// hex digits (0-9/A-F) fill the byte under the cursor two nibbles at a time -
// high nibble first - then auto-advance, mirroring how a real hex editor's
// keypad entry works. The first nibble previews immediately (paired with the
// byte's existing low nibble) rather than waiting for the second, so every
// keypress visibly changes something. Backspace cancels a half-entered nibble
// by restoring the byte to what it was before the preview. anything else is
// not a valid hex digit and is ignored.
//
// the pending nibble is tied to the offset it started on (pendingOffset), not to wherever the
// cursor currently sits - holding L2 lets the document take the d-pad back while the pad stays
// open, so the cursor can move away mid-entry. if that happens, the stale pending nibble is
// cancelled (restoring its byte) and this keypress starts a fresh entry at the new position,
// instead of writing the second nibble into an unrelated byte.
static void onHexPadKey(char key)
{
   if (key == '\b') {
      if (state.pendingNibble) cancelPendingNibble();
      return;
   }

   int nibble = hexDigitValue(key);
   if (nibble < 0) return;

   uint64_t cursorOffset = (uint64_t)state.cursorRow * HEX_BYTES_PER_ROW + state.cursorCol;
   if (state.pendingNibble && cursorOffset != state.pendingOffset) cancelPendingNibble();

   if (!state.pendingNibble) {
      state.pendingOffset       = cursorOffset;
      state.pendingOriginalByte = getCursorByte();
      state.pendingHighNibble   = (uint8_t)nibble;
      state.pendingNibble       = 1;
      setCursorByte((uint8_t)((nibble << 4) | (state.pendingOriginalByte & 0x0F)));
      return;
   }

   setCursorByte((uint8_t)((state.pendingHighNibble << 4) | nibble));
   state.pendingNibble = 0;
   moveCursorBytes(1);
}

// d-pad/shoulder cursor movement - shared by normal input and by the
// keyboard-open/L2-held path, where the document temporarily takes the d-pad
// back from the keyboard.
static void handleDpadNavigation(void)
{
   if (isRepeatDue(&scrollDownRepeat, getPadButtonState(PAD_BTN_DOWN)))        moveCursorRows(1);
   else if (isRepeatDue(&scrollUpRepeat, getPadButtonState(PAD_BTN_UP)))      moveCursorRows(-1);
   else if (isRepeatDue(&scrollRightRepeat, getPadButtonState(PAD_BTN_RIGHT))) moveCursorBytes(1);
   else if (isRepeatDue(&scrollLeftRepeat, getPadButtonState(PAD_BTN_LEFT)))   moveCursorBytes(-1);
   else if (isPadButtonPressed(PAD_BTN_R1)) moveCursorRows(HEX_PAGE_SIZE);
   else if (isPadButtonPressed(PAD_BTN_L1)) moveCursorRows(-HEX_PAGE_SIZE);
}

static void update(void)
{
   // Save works regardless of the hex pad's state, so it doesn't need to close
   // (or even be unfocused from) the pad first.
   if (isPadButtonPressed(PAD_BTN_START)) { saveDocument(); return; }

   if (isHexPadOpen()) {
      // the pad has exclusive input while open (it closes itself on Circle), but
      // edits committed through it still need their rows re-rendered live, or the
      // page appears unchanged until the pad closes. holding L2 hands the d-pad
      // back to the document so the byte cursor can be repositioned without
      // closing the pad.
      if (isHexPadBackgroundFocused()) handleDpadNavigation();
      if (state.rowsStale) rebuildRows();
      return;
   }

   if (state.fileSize > 0 && isPadButtonPressed(PAD_BTN_CROSS)) { openHexPad(onHexPadKey); return; }

   if (isPadButtonPressed(PAD_BTN_CIRCLE)) {
      hideOverlay(&hexViewerOverlay);
      return;
   }

   handleDpadNavigation();

   // page up/down, repeating while held. only reachable while the hex pad is fully closed (the
   // early return above), so this never fights L2's shift-focus role while the pad is up.
   if (isRepeatDue(&pageDownRepeat, getPadButtonState(PAD_BTN_R2)))    moveCursorRows(HEX_PAGE_SIZE);
   else if (isRepeatDue(&pageUpRepeat, getPadButtonState(PAD_BTN_L2))) moveCursorRows(-HEX_PAGE_SIZE);

   if (state.rowsStale) rebuildRows();
}

static void draw(void)
{
   fillGfxRectangle(0, 0, getGfxScreenWidth(), getGfxScreenHeight(), COLOR_SCRIM);

   drawImage(&headerIcon);
   drawLabel(&headerNameLabel);
   if (state.dirty) drawLabel(&headerStarLabel);

   fillGfxRectangle(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, COLOR_PANEL_BG);
   drawNineSlice(&panel);
   fillGfxRectangle(SEPARATOR_X1, SEPARATOR_Y, SEPARATOR_X2 - SEPARATOR_X1, SEPARATOR_H, COLOR_SEPARATOR);

   int headerRowY = PANEL_Y + PANEL_PAD;
   drawLabelAt(&addressHeaderLabel, addressAreaX, getCenteredLabelY(&addressHeaderLabel, headerRowY, ROW_HEIGHT));
   drawLabelAt(&hexHeaderLabel, hexAreaX, getCenteredLabelY(&hexHeaderLabel, headerRowY, ROW_HEIGHT));

   for (int i = 0; i < HEX_PAGE_SIZE; i++) {
      uint64_t rowOffset = (uint64_t)(state.scrollTop + i) * HEX_BYTES_PER_ROW;
      if (rowOffset >= state.fileSize) break;

      int rowY      = PANEL_Y + PANEL_PAD + (i + 1) * ROW_HEIGHT;   // +1 row for the fixed header
      int isCurrent = (state.scrollTop + i == state.cursorRow);

      if (isCurrent) {
         fillGfxRectangle(GUTTER_BG_X, rowY, addressBgWidth, ROW_HEIGHT, COLOR_ACTIVE_BG);
         int hexCellX = hexAreaX + state.cursorCol * BYTE_CELL_CHARS * charWidth;
         fillGfxRectangle(hexCellX, rowY, 2 * charWidth, ROW_HEIGHT, COLOR_ACTIVE_BG);
         int asciiCellX = asciiAreaX + state.cursorCol * charWidth;
         fillGfxRectangle(asciiCellX, rowY, charWidth, ROW_HEIGHT, COLOR_ACTIVE_BG);
      }

      Label *addressLabel = isCurrent ? &activeAddressLabels[i] : &addressLabels[i];
      drawLabelAt(addressLabel, addressAreaX, getCenteredLabelY(addressLabel, rowY, ROW_HEIGHT));
      drawLabelAt(&hexLabels[i], hexAreaX, getCenteredLabelY(&hexLabels[i], rowY, ROW_HEIGHT));
      drawLabelAt(&asciiLabels[i], asciiAreaX, getCenteredLabelY(&asciiLabels[i], rowY, ROW_HEIGHT));
   }

   drawScrollbar(&scrollbar, (int)getTotalRowCount(), HEX_PAGE_SIZE, state.scrollTop);
   setButtonState(&editFooterButton, (isHexPadOpen() || state.fileSize == 0) ? BUTTON_DISABLED : BUTTON_ENABLED);
   setButtonState(&saveFooterButton, (state.dirty && !state.pendingNibble) ? BUTTON_ENABLED : BUTTON_DISABLED);
   setButtonState(&exitFooterButton, isHexPadOpen() ? BUTTON_DISABLED : BUTTON_ENABLED);   // Circle closes the pad, not the viewer, while it's open
   drawButton(&editFooterButton);
   drawButton(&saveFooterButton);
   drawButton(&exitFooterButton);
}

static void term(void)
{
   if (state.fileOpen) { closeFs(&state.file); state.fileOpen = 0; }
   free(state.edits);
   for (int i = 0; i < HEX_PAGE_SIZE; i++) {
      freeLabel(&addressLabels[i]);
      freeLabel(&activeAddressLabels[i]);
      freeLabel(&hexLabels[i]);
      freeLabel(&asciiLabels[i]);
   }
   freeLabel(&headerNameLabel);
   freeLabel(&headerStarLabel);
   freeLabel(&addressHeaderLabel);
   freeLabel(&hexHeaderLabel);
   freeButton(&exitFooterButton);
   freeButton(&editFooterButton);
   freeButton(&saveFooterButton);
   closeFont(&monoFont);
   closeFont(&headerFont);
   hexViewerOverlay.status = OVERLAY_TERMINATED;
}

Overlay hexViewerOverlay = { show, hide, update, draw, term, OVERLAY_TERMINATED };

void initHexViewerOverlay(GfxTexture sprites)
{
   monoFont   = openFontFile(FONT_PATH);
   headerFont = openSystemFont(FONT_POP);
   if (!monoFont.open) logError("[hex-viewer] failed to open mono font: %s\n", FONT_PATH);

   setFontMetrics(&monoFont, FONT_METRICS_GRID_CEIL, 1.0f);
   charWidth = (int)measureFontText(&monoFont, FONT_SIZE, "0");
   if (charWidth <= 0) charWidth = 1;

   addressAreaX   = PANEL_X + PANEL_PAD;
   hexAreaX       = addressAreaX + (ADDRESS_CHARS + COLUMN_GAP_CHARS) * charWidth;
   asciiAreaX     = hexAreaX + (HEX_AREA_CHARS + COLUMN_GAP_CHARS) * charWidth;

   // covers the address text plus half the column gap, so it stops short of the
   // hex area instead of stretching all the way up to (and touching) it.
   addressBgWidth = (addressAreaX - GUTTER_BG_X) + ADDRESS_CHARS * charWidth + (COLUMN_GAP_CHARS * charWidth) / 2;

   initImage(&headerIcon, sprites, HEADER_ICON_X, HEADER_ICON_Y, HEADER_ICON_W, HEADER_ICON_H,
             spriteRegions[SPRITE_GENERIC], GFX_FILTER_LINEAR);
   initLabelRaw(&headerNameLabel, &headerFont, HEADER_NAME_X, HEADER_NAME_Y, HEADER_NAME_WIDTH, AUTO,
                HEADER_NAME_SIZE, COLOR_WHITE, TEXT_NOWRAP_ELLIPSIS, "");
   initLabel(&headerStarLabel, &headerFont, 0, 0, 20, AUTO, HEADER_NAME_SIZE, COLOR_RED, TEXT_NOWRAP, "*");

   initNineSlice(&panel, sprites, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, spriteRegions[SPRITE_HIGHLIGHT], HIGHLIGHT_CAP, HIGHLIGHT_CAP);
   initScrollbar(&scrollbar, sprites, SCROLLBAR_X, SCROLLBAR_Y, SCROLLBAR_W, SCROLLBAR_H,
                 spriteRegions[SPRITE_VERTICAL_PILL], SCROLLBAR_THUMB_W, SCROLLBAR_THUMB_CAP, SCROLLBAR_THUMB_MIN_H);

   Image editIcon;
   Label editLabel;
   initGlyphIcon(&editIcon, GLYPH_CROSS, FOOTER_ICON_H);
   initLabel(&editLabel, &headerFont, 0, 0, AUTO, AUTO, FOOTER_TEXT_SIZE, COLOR_WHITE, TEXT_NOWRAP, "Edit");
   initButton(&editFooterButton, editIcon, editLabel, BUTTON_ENABLED);   // opening the keyboard is handled directly in update()
   moveButton(&editFooterButton, FOOTER_X, FOOTER_Y);

   Image saveIcon;
   Label saveLabel;
   initGlyphIcon(&saveIcon, GLYPH_START, FOOTER_ICON_H);
   initLabel(&saveLabel, &headerFont, 0, 0, AUTO, AUTO, FOOTER_TEXT_SIZE, COLOR_WHITE, TEXT_NOWRAP, "Save");
   initButton(&saveFooterButton, saveIcon, saveLabel, BUTTON_DISABLED);   // saving is handled directly in update()
   moveButton(&saveFooterButton, FOOTER_X + getButtonWidth(&editFooterButton) + FOOTER_BUTTON_GAP, FOOTER_Y);

   Image exitIcon;
   Label exitLabel;
   initGlyphIcon(&exitIcon, GLYPH_CIRCLE, FOOTER_ICON_H);
   initLabel(&exitLabel, &headerFont, 0, 0, AUTO, AUTO, FOOTER_TEXT_SIZE, COLOR_WHITE, TEXT_NOWRAP, "Exit");
   initButton(&exitFooterButton, exitIcon, exitLabel, BUTTON_ENABLED);
   moveButton(&exitFooterButton, FOOTER_X + getButtonWidth(&editFooterButton) + FOOTER_BUTTON_GAP
                                          + getButtonWidth(&saveFooterButton) + FOOTER_BUTTON_GAP, FOOTER_Y);

   initLabel(&addressHeaderLabel, &monoFont, 0, 0, AUTO, AUTO, FONT_SIZE, COLOR_HEADER_TEXT, TEXT_NOWRAP, "Offset");

   char columnHeaderText[HEX_AREA_CHARS + 1];
   int pos = 0;
   for (int col = 0; col < HEX_BYTES_PER_ROW; col++) {
      columnHeaderText[pos++] = HEX_DIGITS[col >> 4];
      columnHeaderText[pos++] = HEX_DIGITS[col & 0xF];
      columnHeaderText[pos++] = ' ';
   }
   columnHeaderText[pos] = '\0';
   initLabel(&hexHeaderLabel, &monoFont, 0, 0, AUTO, AUTO, FONT_SIZE, COLOR_HEADER_TEXT, TEXT_NOWRAP, columnHeaderText);

   for (int i = 0; i < HEX_PAGE_SIZE; i++) {
      initLabel(&addressLabels[i],       &monoFont, 0, 0, AUTO, AUTO, FONT_SIZE, COLOR_ADDRESS,        TEXT_NOWRAP, "");
      initLabel(&activeAddressLabels[i], &monoFont, 0, 0, AUTO, AUTO, FONT_SIZE, COLOR_ADDRESS_ACTIVE, TEXT_NOWRAP, "");
      initLabel(&hexLabels[i],           &monoFont, 0, 0, AUTO, AUTO, FONT_SIZE, COLOR_WHITE,          TEXT_NOWRAP, "");
      initLabelRaw(&asciiLabels[i],      &monoFont, 0, 0, AUTO, AUTO, FONT_SIZE, COLOR_WHITE,          TEXT_NOWRAP, "");
   }
}
