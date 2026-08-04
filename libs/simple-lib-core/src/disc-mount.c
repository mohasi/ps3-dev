// disc-mount - see disc-mount.h
#include "disc-mount.h"
#include "syscall.h"
#include "string-utilities.h"
#include "vfs.h"
#include "dbg.h"
#include <stdint.h>
#include <sys/timer.h>

#define SC_COBRA                8
#define OP_GET_DISC_TYPE        0x7020
#define OP_FAKE_STORAGE_EVENT   0x7022
#define OP_GET_EMU_STATE        0x7023
#define OP_UMOUNT_DISCFILE      0x702C
#define DEV_BDVD                0x101000000000006ULL

#define STORAGE_EVENT_PRE_EJECT     4
#define STORAGE_EVENT_EJECTED       8
#define STORAGE_EVENT_PRE_INSERT    7
#define STORAGE_EVENT_INSERTED      3

static const char *pathLastMount = "/dev_hdd0/tmp/sdm_last.txt";

// Set while Cobra is serving one of our images. Only a fallback for
// isDiscImageMounted(), which asks Cobra directly.
static int imageMounted = 0;

// What the drive physically held when we mounted. Kept because our own fake
// eject wipes Cobra's copy, so at unmount time nothing else can tell us.
static unsigned int discTypeBeforeMount = 0;

// Cobra caches the disc type from the last drive event rather than probing, and
// the fake eject we send while mounting resets it to 0 — so it is only truthful
// until we mount something. mountDiscImage captures it beforehand for that reason.
unsigned int getRealDiscType(void)
{
   unsigned int realType = 0, effectiveType = 0, fakeType = 0;
   int rc = (int)scCall4(SC_COBRA, OP_GET_DISC_TYPE, (uint64_t)(uintptr_t)&realType,
                         (uint64_t)(uintptr_t)&effectiveType, (uint64_t)(uintptr_t)&fakeType);
   if (rc != 0) {
      static int warned = 0;   // polled every couple of seconds, so warn once
      if (!warned) {
         warned = 1;
         logError("[disc] get_disc_type rc=0x%x\n", rc);
      }
      return 0;
   }
   return realType;
}

// Issue syscall8 opcode 0x7024 to switch Cobra's active emulated PS3 disc.
// `files` points to a syscall-visible argv array of image part paths.
static int cobraMountPs3(char *files[], unsigned int count)
{
   int ret;
   __asm__ volatile (
       "li 3, 0x7024\n\t"
       "mr 4, %[count]\n\t"
       "mr 5, %[files]\n\t"
       "li 11, 8\n\t"
       "sc\n\t"
       "mr %[ret], 3\n\t"
       : [ret]"=r"(ret)
       : [count]"r"((uint64_t)count),
         [files]"r"((uint64_t)(uintptr_t)files)
       : "r0","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12",
         "cr0","ctr","xer","memory"
   );
   if (ret != 0) logError("[disc] sc8 mount_ps3 rc=0x%x\n", ret);
   return ret;
}

// Raise a synthetic BD-drive storage event so XMB updates disc state.
// `discType` rides in the top half of the event parameter; Cobra reads it back
// on the insert event to decide what the drive now holds. 0 means "let the
// active emulation decide", which is what an image mount wants.
static int fakeStorageEvent(uint64_t event, unsigned int discType)
{
   sys_timer_usleep(5000);
   return (int)scCall4(SC_COBRA, OP_FAKE_STORAGE_EVENT, event, (uint64_t)discType << 32, DEV_BDVD);
}

static int fakeDiscEject(void)
{
   int rc = fakeStorageEvent(STORAGE_EVENT_PRE_EJECT, 0);
   if (rc != 0) {
      logError("[disc] pre-eject rc=0x%x\n", rc);
      return rc;
   }

   rc = fakeStorageEvent(STORAGE_EVENT_EJECTED, 0);
   if (rc != 0) logError("[disc] eject rc=0x%x\n", rc);
   return rc;
}

static int fakeDiscInsert(unsigned int discType)
{
   int rc = fakeStorageEvent(STORAGE_EVENT_PRE_INSERT, discType);
   if (rc != 0) {
      logError("[disc] pre-insert rc=0x%x\n", rc);
      return rc;
   }

   rc = fakeStorageEvent(STORAGE_EVENT_INSERTED, discType);
   if (rc != 0) logError("[disc] insert rc=0x%x\n", rc);
   return rc;
}

// Cobra's emulation state, as sys_emu_state_t in its storage_ext.h. `size`
// must match the kernel's sizeof() exactly or the syscall returns EINVAL —
// pathSize is lv2's MAX_PATH (0x420).
enum { EMU_STATE_PATH_SIZE = 0x420 };
typedef struct {
   int32_t size;
   int32_t discEmulation;      // 0 = nothing mounted
   char    firstFilePath[EMU_STATE_PATH_SIZE];
} __attribute__((packed)) CobraEmuState;

static CobraEmuState emuState;   // ~1KB, kept out of the caller's small thread stack

// Cobra is the only honest source here: the image survives an XMB restart, so
// our own flag can be out of date after a reload. The flag covers the case
// where this syscall is unavailable (a Cobra build whose struct doesn't match).
int isDiscImageMounted(void)
{
   emuState.size = (int32_t)sizeof emuState;
   int rc = (int)scCall2(SC_COBRA, OP_GET_EMU_STATE, (uint64_t)(uintptr_t)&emuState);
   if (rc != 0) {
      static int warned = 0;   // polled every couple of seconds, so warn once
      if (!warned) {
         warned = 1;
         logWarn("[disc] get_emu_state rc=0x%x, falling back to local state\n", rc);
      }
      return imageMounted;
   }
   return emuState.discEmulation != 0;
}

int getLastMountedImage(char *pathOut, int capacity)
{
   int length = readFile(pathLastMount, pathOut, (uint64_t)capacity);
   return length > 0 ? length : 0;
}

// A real disc, or an explicit unmount, replaces the remembered image outright:
// only mounting one again puts it back.
void forgetLastMountedImage(void)
{
   deleteFile(pathLastMount);
}

int mountDiscImage(const char *isoPath)
{
   char *files[1];
   files[0] = (char *)isoPath;

   unsigned int realType = getRealDiscType();   // capture before our eject wipes it

   int rc = fakeDiscEject();
   if (rc != 0) return rc;

   rc = cobraMountPs3(files, 1);
   if (rc != 0) {
      // Cobra drops whatever was mounted before it even looks at our file, so a
      // failure here leaves the drive genuinely empty. Re-publish what is really
      // in it, or the XMB sits in the ejected state with no way back.
      fakeDiscInsert(realType);
      imageMounted = 0;
      return rc;
   }

   rc = fakeDiscInsert(0);
   if (rc != 0) return rc;

   discTypeBeforeMount = realType;
   imageMounted = 1;

   if (writeFile(pathLastMount, isoPath, (uint64_t)getStrLen(isoPath)) != 0)
      logError("[disc] could not remember %s for next boot\n", isoPath);
   return 0;
}

// The insert event carries the real disc type so a physical disc left in the
// drive shows up again; with an empty drive we stop after the eject.
UnmountResult unmountDiscImage(void)
{
   // faking an eject with nothing mounted would tell the XMB a real disc left
   if (!isDiscImageMounted()) return UNMOUNT_NOTHING_MOUNTED;

   // a disc inserted since we mounted is the one to hand back; failing that, the
   // one that was already in the drive when we mounted
   unsigned int realType = getRealDiscType();
   if (realType == 0) realType = discTypeBeforeMount;

   if (fakeDiscEject() != 0) return UNMOUNT_FAILED;

   int rc = (int)scCall1(SC_COBRA, OP_UMOUNT_DISCFILE);
   if (rc != 0) {
      logError("[disc] sc8 umount rc=0x%x\n", rc);
      return UNMOUNT_FAILED;
   }

   imageMounted = 0;
   discTypeBeforeMount = 0;
   forgetLastMountedImage();

   // the image is gone either way, so a failed re-insert is not a failed unmount
   if (realType != 0) fakeDiscInsert(realType);
   return UNMOUNT_DONE;
}
