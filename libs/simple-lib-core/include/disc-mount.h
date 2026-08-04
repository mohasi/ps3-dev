#pragma once

// Mounting .iso disc images as the Blu-ray disc, through Cobra's syscall 8.
// Shared by the simple-disc-mount plugin (XMB menu) and the file manager app.
//
// The mounted image survives an XMB restart, so the last mounted path is kept
// in a file: whoever boots next can put it back.

typedef enum {
   UNMOUNT_DONE = 0,
   UNMOUNT_NOTHING_MOUNTED,
   UNMOUNT_FAILED,
} UnmountResult;

// Type of the disc physically in the drive, 0 when the drive is empty.
unsigned int getRealDiscType(void);

// True while Cobra is serving a disc image instead of the real drive.
int isDiscImageMounted(void);

// Mount isoPath as the disc and remember it for next boot. 0 on success.
int mountDiscImage(const char *isoPath);

// Drop the emulated disc and hand the drive back to reality.
UnmountResult unmountDiscImage(void);

// Path of the image remembered by the last successful mount. Returns its
// length, or 0 when nothing is remembered.
int getLastMountedImage(char *pathOut, int capacity);

void forgetLastMountedImage(void);
