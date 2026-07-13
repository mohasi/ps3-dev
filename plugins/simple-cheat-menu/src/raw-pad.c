// raw kernel pad reads. see raw-pad.h for the safety contract.
//
// resolution recipe (proven on this firmware 2026-07-08, recorded in memory):
// the vsh export sys_io_3733EA3C is cellPadGetData; its first instruction is
// `lwz rX, slot(r2)` where slot is a SIGNED 16-bit displacement off vsh's TOC.
// io_pad_object = *(vshToc + slot). vshToc is found by scanning vsh's image
// for the .init_proc signature word 0x00010200 whose +4 and +0xC words match.
// per-port pad handle: *(*(io_pad_object + 4) + 0x104 + port*0xE8), fetched
// fresh on every read so a rebuilt pad object never leaves us a stale handle.

#include <stdint.h>

#include "dbg.h"
#include "syscall.h"
#include "vsh-ext.h"
#include "raw-pad.h"

#define TAG "[cht] "

#define SYSCALL_HID_MANAGER_READ  0x1F6
#define VSH_TOC_SCAN_START        0x10000u
#define VSH_TOC_SCAN_END          0x700000u

static uint32_t ioPadObject = 0;

// section: resolve io_pad_object

static uint32_t findVshToc(void)
{
   for (uint32_t addr = VSH_TOC_SCAN_START; addr < VSH_TOC_SCAN_END; addr += 4) {
      const uint32_t *words = (const uint32_t *)(uintptr_t)addr;
      if (words[0] == 0x00010200u && words[1] == words[3]) return words[1];
   }
   return 0;
}

int initRawPad(void)
{
   uint32_t vshToc = findVshToc();
   if (!vshToc) {
      logError(TAG "raw-pad: vsh toc scan failed\n");
      return -1;
   }

   // function pointer -> opd -> code entry of vsh's cellPadGetData export
   const uint32_t *opd = (const uint32_t *)(uintptr_t)sys_io_3733EA3C;
   uint32_t entry = opd[0];
   uint32_t firstInstruction = *(const uint32_t *)(uintptr_t)entry;

   // expect `lwz rX, slot(r2)`: primary opcode 32, base register r2
   if ((firstInstruction >> 26) != 32 || ((firstInstruction >> 16) & 0x1F) != 2) {
      logError(TAG "raw-pad: unexpected prologue 0x%x at 0x%x\n", firstInstruction, entry);
      return -1;
   }

   int16_t tocSlot = (int16_t)(firstInstruction & 0xFFFF);
   ioPadObject = *(const uint32_t *)(uintptr_t)(vshToc + tocSlot);
   logInfo(TAG "raw-pad: toc=0x%x slot=%d io_pad_object=0x%x\n", vshToc, tocSlot, ioPadObject);
   return ioPadObject ? 0 : -1;
}

// section: input capture

void setVshPadEnabled(int enabled)
{
   if (ioPadObject) *(volatile uint8_t *)(uintptr_t)ioPadObject = (uint8_t)(enabled != 0);
}

// section: read

int readRawPad(uint32_t *buttonsOut)
{
   *buttonsOut = 0;
   if (!ioPadObject) return -1;

   // fetch the port-0 handle fresh each read (never cache across transitions)
   uint32_t portTable = *(const uint32_t *)(uintptr_t)(ioPadObject + 4);
   if (!portTable) return -2;
   uint32_t handle = *(const uint32_t *)(uintptr_t)(portTable + 0x104);
   if (!handle) return -3;

   // same wire layout as CellPadData.button[]: 0x80 bytes of 16-bit words,
   // digital1 at word 2, digital2 at word 3
   uint16_t report[64];
   int64_t length = scCall4(SYSCALL_HID_MANAGER_READ, handle, 0xFF, (uint32_t)(uintptr_t)report, sizeof(report));
   if (length >= 8) *buttonsOut = (uint32_t)report[2] | ((uint32_t)report[3] << 8);   // >=8 bytes covers words 2-3
   return (int)length;
}
