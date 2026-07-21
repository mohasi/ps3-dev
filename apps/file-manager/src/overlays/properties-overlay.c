// properties-overlay - details for one file plus a background SHA-1 (see properties-overlay.h).
// Same flat/metro look as confirm-overlay: dimmed screen, centred panel, a close hint along the
// bottom. Rows are a fixed key column and a value column; only the SHA-1 row changes after the
// dialog opens.
#include "overlays/properties-overlay.h"
#include "file-type.h"
#include "gfx.h"
#include "pad.h"
#include "font.h"
#include "audio.h"
#include "ui/label.h"
#include "ui/image.h"
#include "ui/console-glyphs.h"
#include "theme.h"
#include "vfs.h"
#include "path.h"
#include "format.h"
#include "sha1.h"
#include "string-utilities.h"
#include "thread.h"
#include "dbg.h"
#include <sys/sys_time.h>
#include <stdlib.h>
#include <stdio.h>

// panel geometry
#define DIALOG_W        1000
#define CONTENT_X       54
#define TOP_PAD         44
#define TITLE_SIZE      26
#define TITLE_ROWS_GAP  30
#define ROW_HEIGHT      46
#define ROW_TEXT_SIZE   20
#define VALUE_X         300   // panel-relative start of the value column
#define ROWS_BUTTON_GAP 26
#define BOTTOM_PAD      32

// button hints along the bottom (Cross appears only when a hash is waiting to be asked for)
#define BUTTON_GLYPH    28
#define BUTTON_GAP      44
#define BUTTON_SIZE     18
#define BUTTON_ROW_H    32
#define GLYPH_LABEL_GAP 10

#define HASH_CHUNK_BYTES (256 * 1024)

// hashing runs at tens of MB/s, so a big file is minutes of work - past this size the user asks
// for it with Cross rather than having it start just because they opened the panel.
#define HASH_AUTO_LIMIT  (1024ull * 1024 * 1024)

typedef enum { ROW_NAME, ROW_TYPE, ROW_SIZE, ROW_MODIFIED, ROW_PERMISSIONS, ROW_LOCATION, ROW_SHA1, ROW_COUNT } PropertyRow;

static const char *rowKeys[ROW_COUNT] = { "Name", "Type", "Size", "Modified", "Permissions", "Location", "SHA-1" };

static Font    font;
static Audio  *clickSfx;
static int     armed;         // 0 on the frame we open, so the opening press isn't read as close
static int     dialogHeight;

static Image   closeIcon, calculateIcon;
static Label   titleLabel, closeLabel, calculateLabel;
static Label   keyLabels[ROW_COUNT], valueLabels[ROW_COUNT];

// hash worker state. the worker only ever writes hashText/hashedBytes/hashRunning; the main
// thread turns those into labels, so no lock is needed (single writer per field, as folder-sizer
// does). requestedGeneration rises on every open: a worker whose generation is stale drops its
// result rather than writing over a newer file's.
static char           hashFilePath[MAX_PATH_LEN];
static char           hashText[48];
static uint64_t       hashFileSize;
static volatile uint64_t hashedBytes;
static volatile int   hashRunning;
static volatile int   hashCancel;
static volatile int   hashGeneration;
static volatile int   hashResultGeneration = -1;
static int            hashPending;          // main thread: a file is waiting for a free worker
static int            hashOnRequest;         // main thread: file too big to hash unasked - Cross starts it
static int            shownPercent;         // last percentage rendered, so we re-rasterise once per percent

static void setRow(PropertyRow row, const char *value) { setLabelText(&valueLabels[row], value); }

void initPropertiesOverlay(Audio *sfx)
{
   clickSfx = sfx;
   font     = openSystemFont(FONT_POP);

   initGlyphIcon(&closeIcon,     GLYPH_CIRCLE, BUTTON_GLYPH);
   initGlyphIcon(&calculateIcon, GLYPH_CROSS,  BUTTON_GLYPH);
   initLabel(&titleLabel,     &font, 0, 0, DIALOG_W, AUTO, TITLE_SIZE,  activeTheme->textPrimary, TEXT_NOWRAP, "Properties");
   initLabel(&closeLabel,     &font, 0, 0, DIALOG_W, AUTO, BUTTON_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "Close");
   initLabel(&calculateLabel, &font, 0, 0, DIALOG_W, AUTO, BUTTON_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, "Calculate Hash");

   int valueWidth = DIALOG_W - VALUE_X - CONTENT_X;
   for (int row = 0; row < ROW_COUNT; row++) {
      initLabel(&keyLabels[row], &font, 0, 0, VALUE_X - CONTENT_X, AUTO, ROW_TEXT_SIZE, activeTheme->textSecondary,
                TEXT_NOWRAP, rowKeys[row]);
      // values are file-supplied text: render literally so a '{' in a filename isn't read as markup
      initLabelRaw(&valueLabels[row], &font, 0, 0, valueWidth, AUTO, ROW_TEXT_SIZE, activeTheme->textPrimary,
                   TEXT_NOWRAP_ELLIPSIS, "");
   }

   dialogHeight = TOP_PAD + titleLabel.tt.tex.h + TITLE_ROWS_GAP + ROW_COUNT * ROW_HEIGHT
                + ROWS_BUTTON_GAP + BUTTON_ROW_H + BOTTOM_PAD;
}

// section: SHA-1 worker

static void toHexText(const uint8_t digest[20], char *out)
{
   static const char digits[] = "0123456789abcdef";
   for (int i = 0; i < 20; i++) {
      out[i * 2]     = digits[digest[i] >> 4];
      out[i * 2 + 1] = digits[digest[i] & 0xf];
   }
   out[40] = '\0';
}

static void hashWorker(uint64_t arg)
{
   int generation = (int)arg;

   uint8_t *buffer = (uint8_t *)malloc(HASH_CHUNK_BYTES);
   VfsFile  file;
   int      opened = buffer && openFs(hashFilePath, VFS_O_RDONLY, &file) == 0;
   char     result[48];
   strCopy(result, sizeof result, "Could not be read");

   if (opened) {
      uint64_t startUs = sys_time_get_system_time();
      Sha1State state;
      initSha1(&state);
      int64_t got;
      while ((got = readFs(&file, buffer, HASH_CHUNK_BYTES)) > 0 && !hashCancel) {
         updateSha1(&state, buffer, (int)got);
         hashedBytes += (uint64_t)got;
      }
      int failed = got < 0;
      closeFs(&file);

      if (!failed && !hashCancel) {
         uint8_t digest[20];
         finalizeSha1(&state, digest);
         toHexText(digest, result);

         uint64_t elapsedUs = sys_time_get_system_time() - startUs;
         if (elapsedUs > 0)
            logInfo("[properties] sha-1 of %llu bytes in %llu ms (%llu MB/s)\n", (unsigned long long)hashedBytes,
                    (unsigned long long)(elapsedUs / 1000), (unsigned long long)(hashedBytes / elapsedUs));
      }
   }

   if (!hashCancel && generation == hashGeneration) {
      strCopy(hashText, sizeof hashText, result);
      hashResultGeneration = generation;
   }
   free(buffer);
   hashRunning = 0;
   exitThread();
}

// starts the worker once any previous one has stopped touching the shared state.
static void startPendingHash(void)
{
   if (!hashPending || hashRunning) return;
   hashCancel   = 0;
   hashedBytes  = 0;
   hashRunning  = 1;
   hashPending  = 0;
   sys_ppu_thread_t worker;
   if (spawnThread(&worker, hashWorker, (uint64_t)hashGeneration, THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_16KB, "sha1") != 0) {
      hashRunning = 0;
      strCopy(hashText, sizeof hashText, "Could not be calculated");
      hashResultGeneration = hashGeneration;
   }
}

// hands the current file to the worker: any hash still running for an earlier file is told to
// stop, and update() starts this one as soon as that worker has exited.
static void requestHash(void)
{
   setRow(ROW_SHA1, "Calculating...");
   shownPercent  = -1;
   hashOnRequest = 0;
   hashCancel    = 1;
   hashGeneration++;
   hashPending   = 1;
}

// section: open / close

void showProperties(const char *path)
{
   VfsStat stat;
   if (statPath(path, &stat) != 0 || stat.isDir) return;

   const char *name = getBaseName(path);
   char parent[MAX_PATH_LEN];
   getParentPath(path, parent, sizeof parent);

   char sizeText[64], modifiedText[32], permissionText[12];
   char humanSize[24];
   formatSize(stat.size, humanSize);
   snprintf(sizeText, sizeof sizeText, "%s (%llu bytes)", humanSize, (unsigned long long)stat.size);
   formatDateTimeLocal(modifiedText, sizeof modifiedText, stat.mtime);
   formatPermissions(permissionText, stat.mode, 0);

   setRow(ROW_NAME,        name);
   setRow(ROW_TYPE,        getFileTypeName(classifyFileType(name, 0)));
   setRow(ROW_SIZE,        sizeText);
   setRow(ROW_MODIFIED,    modifiedText);
   setRow(ROW_PERMISSIONS, permissionText);
   setRow(ROW_LOCATION,    parent);

   strCopy(hashFilePath, sizeof hashFilePath, path);
   hashFileSize = stat.size;

   // small files hash on the spot; a big one waits for Cross, so opening the panel never commits
   // the user to minutes of work they didn't ask for.
   if (stat.size > HASH_AUTO_LIMIT) {
      hashCancel    = 1;   // stop a hash still running for a previous file
      hashGeneration++;
      hashPending   = 0;
      hashOnRequest = 1;
      setRow(ROW_SHA1, "Not calculated");
   } else {
      requestHash();
   }

   showOverlay(&propertiesOverlay);
}

void rethemePropertiesOverlay(void)
{
   setLabelColor(&titleLabel,     activeTheme->textPrimary);
   setLabelColor(&closeLabel,     activeTheme->textPrimary);
   setLabelColor(&calculateLabel, activeTheme->textPrimary);
   for (int row = 0; row < ROW_COUNT; row++) {
      setLabelColor(&keyLabels[row],   activeTheme->textSecondary);
      setLabelColor(&valueLabels[row], activeTheme->textPrimary);
   }
}

static void show(void) { armed = 0; propertiesOverlay.status = OVERLAY_VISIBLE; }

static void hide(void)
{
   hashCancel  = 1;   // the worker is detached: tell it to stop, it exits on its own
   hashPending = 0;
   propertiesOverlay.status = OVERLAY_HIDDEN;
}

// section: frame

// "Calculating... 42%" while the worker runs. re-rendered only when the whole percent changes,
// since every setLabelText re-rasterises the text into VRAM.
static void updateHashProgress(void)
{
   if (hashFileSize == 0) return;
   int percent = (int)(hashedBytes * 100 / hashFileSize);
   if (percent == shownPercent) return;
   shownPercent = percent;

   char text[32];
   snprintf(text, sizeof text, "Calculating... %d%%", percent);
   setRow(ROW_SHA1, text);
}

static void update(void)
{
   startPendingHash();

   if (hashResultGeneration == hashGeneration) {
      setRow(ROW_SHA1, hashText);
      hashResultGeneration = -1;
   } else if (hashRunning) {
      updateHashProgress();
   }

   if (!armed) { armed = 1; return; }   // swallow the press that opened the dialog

   if (hashOnRequest && isPadButtonPressed(PAD_BTN_CROSS)) {
      playAudioOnce(clickSfx);
      requestHash();
      return;
   }
   if (isPadButtonPressed(PAD_BTN_CIRCLE) || isPadButtonPressed(PAD_BTN_CROSS)) {
      playAudioOnce(clickSfx);
      hideOverlay(&propertiesOverlay);
   }
}

static void draw(void)
{
   int dialogX = (getGfxScreenWidth()  - DIALOG_W)     / 2;
   int dialogY = (getGfxScreenHeight() - dialogHeight) / 2;

   fillGfxRectangle(0, 0, getGfxScreenWidth(), getGfxScreenHeight(), activeTheme->scrim);
   drawGfxBox(dialogX, dialogY, DIALOG_W, dialogHeight, activeTheme->borderThickness, activeTheme->dialogFill, activeTheme->dialogBorder);

   drawLabelAt(&titleLabel, dialogX + CONTENT_X, dialogY + TOP_PAD);

   int rowY = dialogY + TOP_PAD + titleLabel.tt.tex.h + TITLE_ROWS_GAP;
   for (int row = 0; row < ROW_COUNT; row++) {
      drawLabelAt(&keyLabels[row],   dialogX + CONTENT_X, rowY);
      drawLabelAt(&valueLabels[row], dialogX + VALUE_X,   rowY);
      rowY += ROW_HEIGHT;
   }

   // button hints, centred as a group: Calculate Hash (only while one is on offer), then Close
   Image *icons[2]  = { &calculateIcon, &closeIcon };
   Label *labels[2] = { &calculateLabel, &closeLabel };
   int first = hashOnRequest ? 0 : 1;

   int rowWidth = 0;
   for (int i = first; i < 2; i++) rowWidth += icons[i]->w + GLYPH_LABEL_GAP + labels[i]->tt.tex.w;
   if (first == 0) rowWidth += BUTTON_GAP;

   int buttonY = rowY + ROWS_BUTTON_GAP;
   int buttonX = dialogX + (DIALOG_W - rowWidth) / 2;
   for (int i = first; i < 2; i++) {
      drawImageAt(icons[i], buttonX, buttonY + (BUTTON_ROW_H - icons[i]->h) / 2);
      drawLabelAt(labels[i], buttonX + icons[i]->w + GLYPH_LABEL_GAP, buttonY + (BUTTON_ROW_H - BUTTON_SIZE) / 2 - 2);
      buttonX += icons[i]->w + GLYPH_LABEL_GAP + labels[i]->tt.tex.w + BUTTON_GAP;
   }
}

static void term(void)
{
   hashCancel = 1;
   freeLabel(&titleLabel);
   freeLabel(&closeLabel);
   freeLabel(&calculateLabel);
   for (int row = 0; row < ROW_COUNT; row++) {
      freeLabel(&keyLabels[row]);
      freeLabel(&valueLabels[row]);
   }
   closeFont(&font);
   propertiesOverlay.status = OVERLAY_TERMINATED;
}

Overlay propertiesOverlay = { show, hide, update, draw, term, OVERLAY_TERMINATED };
