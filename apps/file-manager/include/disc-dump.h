#pragma once

// disc-dump - copies the disc in the Blu-ray drive, sector for sector, to a raw .iso on the
// internal drive. The image is exactly what the drive returns: a PS3 game disc therefore
// lands encrypted (the keys needed to decrypt it are a separate job), while PS2 and data
// discs land as ordinary readable images.
//
// Runs as a file-task body (see file-task.h) behind the progress overlay: prepareDiscDump on
// the main thread picks the destination and refuses early if there is no disc or no room,
// then runDiscDump does the work and getDiscDumpStatus reports how it went.

// 1 when the drive reports a disc; gates whether the action is offered at all.
int isDiscInDrive(void);

// main thread: settles the destination path and checks free space. returns 0 when a dump can
// start, or -1 with the reason written to reasonOut (for a message dialog).
int prepareDiscDump(char *reasonOut, int cap);

// destination chosen by the last prepareDiscDump; valid until the next one.
const char *getDiscDumpDestination(void);

// task body: reads the whole disc into the prepared destination, reporting bytes and
// honouring cancel.
void runDiscDump(void);

// 1 if the finished image cannot be trusted (write failure, or sectors that never read).
int discDumpHadError(void);

// one-line outcome for the finished dialog, e.g. "Dumped to /dev_hdd0/dumps/BLES00000.iso".
const char *getDiscDumpStatus(void);
