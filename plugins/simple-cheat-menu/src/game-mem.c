#include "game-mem.h"
#include "syscall.h"

#include <sys/prx.h>   // sys_prx_module_info_t / sys_prx_segment_info_t for the segments query

// cobra ps3mapi (syscall 8, opcode 0x7777) is the live process-introspection path on this
// CFW. readProcMem/writeProcMem now live in the shared proc-mem.h (simple-lib-plugin); this
// file keeps only the game-specific segment enumeration.
#define SYSCALL_COBRA   8
#define PS3MAPI_OPCODE  0x7777

// ps3mapi module enumeration (TheRouletteBoi's segments opcode; Cobra 8.4+). the
// segments query hands the kernel our sys_prx_module_info_t with our own buffers set
// in it; the kernel fills the segment array and the real segment count.
#define PS3MAPI_GET_ALL_PROC_MODULE_PID   0x0041
#define PS3MAPI_GET_PROC_MODULE_SEGMENTS  0x0048
#define MODULE_LIST_MAX  128   // ps3mapi fills a 128-entry prx-id list, 0-terminated
#define SEGMENTS_MAX       8   // segments per module (a game module has ~2-4)

int getGameScanRanges(uint32_t pid, GameRange *rangesOut, int maxRanges)
{
   // list the game's module ids (proven ps3mapi call; 0 past the last)
   int32_t moduleIds[MODULE_LIST_MAX];
   for (int i = 0; i < MODULE_LIST_MAX; i++) moduleIds[i] = 0;
   scCall4(SYSCALL_COBRA, PS3MAPI_OPCODE, PS3MAPI_GET_ALL_PROC_MODULE_PID, (uint64_t)pid, (uint64_t)(uintptr_t)moduleIds);

   int count = 0;
   for (int m = 0; m < MODULE_LIST_MAX && moduleIds[m] && count < maxRanges; m++) {
      // hand the kernel our buffers via the module-info struct, then it fills them
      sys_prx_module_info_t  info;
      sys_prx_segment_info_t segments[SEGMENTS_MAX];
      char                   filename[256];
      char *clear = (char *)&info;
      for (int i = 0; i < (int)sizeof(info); i++) clear[i] = 0;
      info.filename      = (sys_prx_user_pchar_t)(uintptr_t)filename;          // cast via uintptr_t so it fits
      info.filename_size = sizeof(filename);
      info.segments      = (sys_prx_user_segment_vector_t)(uintptr_t)segments;  // either pointer-width typedef branch
      info.segments_num  = SEGMENTS_MAX;

      int rc = (int)scCall5(SYSCALL_COBRA, PS3MAPI_OPCODE, PS3MAPI_GET_PROC_MODULE_SEGMENTS,
                            (uint64_t)pid, (uint64_t)(uint32_t)moduleIds[m], (uint64_t)(uintptr_t)&info);
      if (rc != 0) continue;   // opcode absent / module gone: skip

      for (uint32_t s = 0; s < info.segments_num && s < SEGMENTS_MAX && count < maxRanges; s++) {
         if (segments[s].memsz == 0) continue;   // no runtime bytes (e.g. empty segment)
         rangesOut[count].base = (uint32_t)segments[s].base;
         rangesOut[count].size = (uint32_t)segments[s].memsz;
         count++;
      }
   }
   return count;   // 0 = caller falls back to the fixed scan window (and its no-match log tells the story)
}
