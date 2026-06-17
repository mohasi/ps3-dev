#include "osk-input.h"

#include <stdint.h>
#include <sys/memory.h>
#include <sysutil/sysutil_common.h>
#include <sysutil/sysutil_oskdialog.h>

#include "string-utilities.h"  // utf8ToUtf16 / utf16ToUtf8
#include "dbg.h"

// slot 0 is held by the app exit callback (see simple-lib-app/app.h); the OSK
// takes its own slot so the two callbacks coexist.
#define OSK_CALLBACK_SLOT  1

// the system caps a field at CELL_OSKDIALOG_STRING_SIZE (512) UTF-16 units.
#define OSK_MAX_CHARS      CELL_OSKDIALOG_STRING_SIZE

// a UTF-16 code unit can take up to 3 UTF-8 bytes (BMP); surrogate pairs collapse
// to one 4-byte sequence, so 3 bytes per unit is a safe upper bound, plus NUL.
#define OSK_UTF8_BYTES     (OSK_MAX_CHARS * 3 + 1)

static bool            active;
static OskDoneCallback doneCallback;
static uint16_t        messageW[OSK_MAX_CHARS + 1];   // guide caption (UTF-16)
static uint16_t        initTextW[OSK_MAX_CHARS + 1];  // pre-filled field text (UTF-16)
static uint16_t        resultW[OSK_MAX_CHARS + 1];    // confirmed text from unload
static char            resultUtf8[OSK_UTF8_BYTES];    // result converted for caller

// the dialog signalled the user is done. unloading is what tears the keyboard off
// screen and fills resultW with the confirmed text; we convert it and hand it to
// the caller. result codes other than OK (cancel / no input) yield NULL.
static void oskFinish(void)
{
   CellOskDialogCallbackReturnParam ret;
   ret.result               = CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK;
   ret.numCharsResultString = OSK_MAX_CHARS;
   ret.pResultString        = resultW;
   resultW[0] = 0;
   cellOskDialogUnloadAsync(&ret);

   if (!doneCallback) return;

   if (ret.result == CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK && resultW[0] != 0) {
      utf16ToUtf8(resultW, resultUtf8, sizeof resultUtf8);
      doneCallback(resultUtf8);
   } else {
      doneCallback(NULL);  // cancelled or empty
   }
}

// the dialog is fully gone; release our callback slot and go idle.
static void oskCleanup(void)
{
   cellSysutilUnregisterCallback(OSK_CALLBACK_SLOT);
   active = false;
}

static void oskCallback(uint64_t status, uint64_t param, void *userdata)
{
   (void)param;
   (void)userdata;
   switch (status) {
      case CELL_SYSUTIL_OSKDIALOG_FINISHED:  oskFinish();  break;
      case CELL_SYSUTIL_OSKDIALOG_UNLOADED:  oskCleanup(); break;
      default: break;
   }
}

bool oskInputActive(void)
{
   return active;
}

bool oskInputBegin(const char *caption, const char *initialText, OskDoneCallback onDone)
{
   if (active) return true;

   doneCallback = onDone;
   utf8ToUtf16(caption,     messageW,  OSK_MAX_CHARS);
   utf8ToUtf16(initialText, initTextW, OSK_MAX_CHARS);
   resultW[0] = 0;

   CellOskDialogInputFieldInfo info;
   info.message      = messageW;
   info.init_text    = initTextW;
   info.limit_length = OSK_MAX_CHARS;

   CellOskDialogParam param;
   param.allowOskPanelFlg = CELL_OSKDIALOG_PANELMODE_ALPHABET |
                      CELL_OSKDIALOG_PANELMODE_NUMERAL  |
                      CELL_OSKDIALOG_PANELMODE_ENGLISH;
   param.firstViewPanel   = CELL_OSKDIALOG_PANELMODE_ALPHABET;
   param.controlPoint.x   = 0.0f;
   param.controlPoint.y   = 0.0f;
   param.prohibitFlgs     = CELL_OSKDIALOG_NO_RETURN;  // single-line file name

   if (cellSysutilRegisterCallback(OSK_CALLBACK_SLOT, oskCallback, NULL) != CELL_OK) {
      logInfo("[osk-input] register callback failed\n");
      return false;
   }

   cellOskDialogAddSupportLanguage(param.allowOskPanelFlg);
   cellOskDialogSetKeyLayoutOption(CELL_OSKDIALOG_10KEY_PANEL | CELL_OSKDIALOG_FULLKEY_PANEL);

   int ret = cellOskDialogLoadAsync(SYS_MEMORY_CONTAINER_ID_INVALID, &param, &info);
   if (ret < 0) {
      logInfo("[osk-input] cellOskDialogLoadAsync failed: 0x%x\n", ret);
      cellSysutilUnregisterCallback(OSK_CALLBACK_SLOT);
      return false;
   }

   active = true;
   return true;
}
