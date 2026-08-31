// Rewrite the AMG lookup port inside the firmware module that performs it.
//
// x3_amgsdk builds the request, then stores the server's port into the request
// object right before connecting. The port is a constant in its code, not part of
// the hostname or the URL, so the only way off port 80 is to change that constant:
//
//   1114:  li   r0,80          <- what we rewrite (twice: one site per server host)
//   1118:  addi r9,r9,-14936     the hostname pointer
//   111c:  stw  r0,32(r27)       port field of the request object
//   1120:  stw  r9,24(r27)       hostname field
//
// (offsets from the disassembly of x3_amgsdk, see docs/ps3-firmware-re.md)
//
// We match on the instruction pair rather than those offsets, so a firmware whose
// module differs cannot leave us silently patching the wrong word.

#include "port-patch.h"

#include "syscall.h"           // prxList, prxInfo, PrxSegment, scCall1
#include "proc-mem.h"          // writeProcMem - the kernel poke, writes read-only code pages
#include "string-utilities.h"  // strEq
#include "dbg.h"

#define TAG "[cdi] "

#define SYS_PROCESS_GETPID   1
#define AMG_MODULE_NAME      "x3_amgsdk_module"   // as reported by sys_prx_get_module_info
#define MODULE_IDS_MAX       192
#define AMG_DEFAULT_PORT     80

#define INSTRUCTION_LI_R0(value)  (0x38000000u | (uint32_t)(value))   // li r0,value

// stw r0,32(rA) - the store of the port into the request object, whatever base register it uses
static int isPortFieldStore(uint32_t instruction)
{
   return (instruction >> 26) == 36 && ((instruction >> 21) & 31) == 0 && (instruction & 0xffff) == 32;
}

// segment 0 (code + rodata) of the loaded module. returns 0 if it is not loaded.
static uint32_t findAmgCodeSegment(uint32_t *outSize)
{
   uint32_t    moduleIds[MODULE_IDS_MAX];
   uint32_t    moduleCount = 0;
   char        name[PRX_NAME_MAX];
   static char filePath[PRX_FILENAME_MAX];   // 512 bytes, kept off the caller's stack
   PrxSegment  segments[4];
   uint32_t    segmentCount = 0;

   if (prxList(moduleIds, MODULE_IDS_MAX, &moduleCount) < 0) return 0;

   for (uint32_t i = 0; i < moduleCount; i++) {
      if (prxInfo((int32_t)moduleIds[i], name, filePath, segments, 4, &segmentCount) < 0) continue;
      if (!strEq(name, AMG_MODULE_NAME) || segmentCount == 0) continue;
      *outSize = (uint32_t)segments[0].filesz;
      return (uint32_t)segments[0].base;
   }
   return 0;
}

int patchAmgLookupPort(uint16_t port)
{
   uint32_t segmentSize = 0;
   uint32_t segmentBase = findAmgCodeSegment(&segmentSize);
   if (!segmentBase || segmentSize < 12) return AMG_MODULE_NOT_LOADED;

   const volatile uint32_t *code = (const volatile uint32_t *)(uintptr_t)segmentBase;
   uint32_t words             = segmentSize / 4;
   uint32_t pid               = (uint32_t)scCall1(SYS_PROCESS_GETPID, 0);
   uint32_t wantedInstruction = INSTRUCTION_LI_R0(port);
   int      sitesFound        = 0;
   int      patched           = 0;

   for (uint32_t i = 0; i + 2 < words; i++) {
      if (!isPortFieldStore(code[i + 2])) continue;
      if (code[i] != INSTRUCTION_LI_R0(AMG_DEFAULT_PORT) && code[i] != wantedInstruction) continue;
      sitesFound++;
      if (code[i] == wantedInstruction) continue;

      uint32_t address = segmentBase + i * 4;
      int rc = writeProcMem(pid, address, &wantedInstruction, sizeof wantedInstruction);
      if (rc != 0) { logError(TAG "port patch at 0x%x failed rc=0x%x\n", (unsigned)address, (unsigned)rc); continue; }
      logInfo(TAG "lookup port 80 -> %d at 0x%x\n", (int)port, (unsigned)address);
      patched++;
   }

   if (sitesFound == 0) { logError(TAG "amg module loaded but no port constant found\n"); return AMG_PORT_NOT_FOUND; }
   return patched;
}

#define AMG_HOST_MARK   "allmusic"     // substring identifying the dead AMG host
#define REDIRECT_IP     "127.0.0.1"    // loopback: the console talks to its own listener

// Overwrite the dead AMG hostname string in x3_amgsdk with a loopback address so
// the firmware's own lookup resolves to our listener. This replaces the old
// gethostbyname code detour, which corrupts vsh on HEN: patching vsh CODE is
// rejected there, a data write to the string is not. Only a bare hostname (no
// '/') is touched, so a URL that merely contains the marker is never truncated.
int patchAmgHost(void)
{
   uint32_t segmentSize = 0;
   uint32_t segmentBase = findAmgCodeSegment(&segmentSize);
   if (!segmentBase || segmentSize < sizeof REDIRECT_IP) return AMG_MODULE_NOT_LOADED;

   const char *segment = (const char *)(uintptr_t)segmentBase;
   uint32_t    pid     = (uint32_t)scCall1(SYS_PROCESS_GETPID, 0);
   int         found   = 0;
   int         patched = 0;

   for (uint32_t i = 0; i + sizeof AMG_HOST_MARK - 1 < segmentSize; i++) {
      if (!startsWith(segment + i, AMG_HOST_MARK)) continue;
      found++;

      // walk back to the string start (the byte after the previous terminator)
      uint32_t start = i;
      while (start > 0 && segment[start - 1] != '\0') start--;
      const char *host    = segment + start;
      int         hostLen = getStrLen(host);

      if (hostLen < (int)sizeof REDIRECT_IP - 1) continue;        // too short to hold 127.0.0.1
      if (findBytes(host, hostLen, "/", 1) >= 0) continue;        // a URL, not a bare host
      if (startsWith(host, REDIRECT_IP)) continue;                // already redirected

      uint32_t address = segmentBase + start;
      int rc = writeProcMem(pid, address, REDIRECT_IP, sizeof REDIRECT_IP);
      if (rc != 0) { logError(TAG "host patch at 0x%x failed rc=0x%x\n", (unsigned)address, (unsigned)rc); continue; }
      logInfo(TAG "amg host -> %s at 0x%x\n", REDIRECT_IP, (unsigned)address);
      patched++;
   }

   if (found == 0) { logError(TAG "amg module loaded but no host string found\n"); return AMG_HOST_NOT_FOUND; }
   return patched;
}
