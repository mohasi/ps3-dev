#include <stdio.h>

#include "console-model.h"
#include "syscall.h"
#include "string-utilities.h"
#include "dbg.h"

// the 8th byte of the console id is the product sub code, which names the
// motherboard. table from psdevwiki's "Product Sub Code" page, chip process per
// board from its "RSX" page: the 90 nm part runs to DIA-001 (CECHH, plus the
// CECHM / CECHQ that share that board), and DIA-002 onwards is the reworked
// 65 nm part.
#define LAST_FRAGILE_SUB_CODE 0x06

typedef struct SubCodeEntry { int subCode; const char *name; const char *rsxProcess; } SubCodeEntry;

static const SubCodeEntry SUB_CODES[] = {
   { 0x01, "CECHA (COK-001)",         "90 nm" },
   { 0x02, "CECHB (COK-001)",         "90 nm" },
   { 0x03, "CECHC (COK-002)",         "90 nm" },
   { 0x04, "CECHE (COK-002)",         "90 nm" },
   { 0x05, "CECHG (SEM-001)",         "90 nm" },
   { 0x06, "CECHH / M / Q (DIA-001)", "90 nm" },
   { 0x07, "CECHJ / K (DIA-002)",     "65 nm" },
   { 0x08, "CECHL / P (VER-001)",     "65 nm" },
   { 0x09, "CECH-20xx (DYN-001)",     "65 nm" },
   { 0x0A, "CECH-21xx (SUR-001)",     "40 nm" },
   { 0x0B, "CECH-25xx (JTP-001)",     "40 nm" },
   { 0x0C, "CECH-30xx (KTE-001)",     "40 nm" },
   { 0x0D, "CECH-40xx",               "40 or 28 nm" },
   { 0x0E, "CECH-40xx",               "40 or 28 nm" },
   { 0x0F, "CECH-40xx",               "40 or 28 nm" },
   { 0x10, "CECH-40xx",               "40 or 28 nm" },
   { 0x11, "CECH-42xx",               "28 nm" },
   { 0x12, "CECH-42xx",               "28 nm" },
   { 0x13, "CECH-43xx",               "28 nm" },
   { 0x14, "CECH-43xx",               "28 nm" },
};
#define SUB_CODE_COUNT (int)(sizeof SUB_CODES / sizeof SUB_CODES[0])

// the 90 nm chip's underfill starts weakening around 70 C, so a console we cannot
// identify is assumed to be one of those: guessing low costs a shorter run,
// guessing high costs the console.
static char modelSummary[96] = "unknown console model";
static int cutoffCelsius = FRAGILE_CUTOFF_CELSIUS;
static int modelDetected;

// the id always starts with the four magic bytes 00 00 00 01; anything else means
// we did not get an id back and the model byte would be nonsense.
static int isConsoleIdValid(const uint8_t *consoleId)
{
   return consoleId[0] == 0 && consoleId[1] == 0 && consoleId[2] == 0 && consoleId[3] == 1;
}

static void logConsoleId(const uint8_t *consoleId)
{
   char hexText[CONSOLE_ID_LENGTH * 2 + 1];
   toHexText(hexText, sizeof hexText, consoleId, CONSOLE_ID_LENGTH);
   logInfo("[bench] console id %s\n", hexText);
}

static void detectConsoleModel(void)
{
   modelDetected = 1;

   // read the console id
   uint8_t consoleId[CONSOLE_ID_LENGTH] = { 0 };
   int32_t rc = getConsoleId(consoleId);
   if (rc == 0) logConsoleId(consoleId);
   if (rc != 0 || !isConsoleIdValid(consoleId))
   {
      logWarn("[bench] console model unreadable (rc=0x%x) - safety cutoff stays at %d C\n", rc, cutoffCelsius);
      return;
   }

   // look its model byte up
   int subCode = consoleId[7];
   const SubCodeEntry *entry = NULL;
   for (int index = 0; index < SUB_CODE_COUNT && !entry; index++)
      if (SUB_CODES[index].subCode == subCode) entry = &SUB_CODES[index];

   if (!entry)
   {
      logWarn("[bench] model byte 0x%02x is not in the table - cutoff stays at %d C\n", subCode, cutoffCelsius);
      return;
   }

   snprintf(modelSummary, sizeof modelSummary, "%s, %s graphics chip", entry->name, entry->rsxProcess);
   cutoffCelsius = subCode <= LAST_FRAGILE_SUB_CODE ? FRAGILE_CUTOFF_CELSIUS : MODERN_CUTOFF_CELSIUS;
   logInfo("[bench] console is a %s - default safety cutoff %d C\n", modelSummary, cutoffCelsius);
}

const char *getConsoleModelSummary(void)
{
   if (!modelDetected) detectConsoleModel();
   return modelSummary;
}

int getModelSafetyCutoffCelsius(void)
{
   if (!modelDetected) detectConsoleModel();
   return cutoffCelsius;
}
