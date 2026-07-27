#include "proc-mem.h"
#include "syscall.h"

// dbg process-memory syscalls, arg order (pid, address, size, buffer). The debug syscall is
// refused on some CFW (rc 0x80010003), so both paths fall back to cobra ps3mapi (syscall 8,
// opcode 0x7777, get=0x0031 / set=0x0032). Once a path succeeds we stick to it and stop probing
// the other: the working path is a fixed per-boot CFW property, so re-probing the dead debug
// syscall per call just wastes half the syscalls on a scan/poke sweep. Cache ONLY on success,
// so a failed read (an unmapped page in a sweep) is never mistaken for a dead ABI. Worker and
// menu threads both call these; memPath is an aligned int and every thread only ever caches the
// same terminal value, so the race is benign.
#define SYS_DBG_READ_PROCESS_MEMORY   904
#define SYS_DBG_WRITE_PROCESS_MEMORY  905
#define SYSCALL_COBRA                 8
#define PS3MAPI_OPCODE                0x7777
#define PS3MAPI_GET_PROC_MEM          0x0031
#define PS3MAPI_SET_PROC_MEM          0x0032

enum { MEM_PATH_UNKNOWN, MEM_PATH_DEBUG, MEM_PATH_COBRA };
static int memPath = MEM_PATH_UNKNOWN;

int readProcMem(uint32_t pid, uint32_t address, void *out, uint32_t size)
{
   if (memPath != MEM_PATH_COBRA &&
       scCall4(SYS_DBG_READ_PROCESS_MEMORY, (uint64_t)pid, (uint64_t)address, (uint64_t)size, (uint64_t)(uintptr_t)out) == 0) {
      memPath = MEM_PATH_DEBUG;
      return 0;
   }
   int mapiRc = (int)scCall6(SYSCALL_COBRA, PS3MAPI_OPCODE, PS3MAPI_GET_PROC_MEM, (uint64_t)pid, (uint64_t)address, (uint64_t)(uintptr_t)out, (uint64_t)size);
   if (mapiRc == 0) memPath = MEM_PATH_COBRA;
   return mapiRc;
}

int writeProcMem(uint32_t pid, uint32_t address, const void *src, uint32_t size)
{
   if (memPath != MEM_PATH_COBRA &&
       scCall4(SYS_DBG_WRITE_PROCESS_MEMORY, (uint64_t)pid, (uint64_t)address, (uint64_t)size, (uint64_t)(uintptr_t)src) == 0) {
      memPath = MEM_PATH_DEBUG;
      return 0;
   }
   int mapiRc = (int)scCall6(SYSCALL_COBRA, PS3MAPI_OPCODE, PS3MAPI_SET_PROC_MEM, (uint64_t)pid, (uint64_t)address, (uint64_t)(uintptr_t)src, (uint64_t)size);
   if (mapiRc == 0) memPath = MEM_PATH_COBRA;
   return mapiRc;
}
