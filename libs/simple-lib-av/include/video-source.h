#pragma once

// video-source - a small seekable reader over the VFS, shared by the probe and the demuxers.
// Wraps a VfsFile with a tracked logical position and the total size, so container parsers can
// read/seek by absolute offset without caring which backend (HDD/exFAT/NTFS) the file lives on.

#include <stdint.h>
#include "vfs.h"

// container parsing reads in tiny pieces (EBML/box headers a byte or a few at a time), so a
// read-ahead buffer turns thousands of one-byte VFS syscalls into occasional block reads.
typedef struct {
   VfsFile  file;
   int      isOpen;
   uint64_t size;         // total file size in bytes
   uint64_t pos;          // logical read position the caller sees
   uint64_t filePos;      // actual descriptor position
   uint8_t *buffer;       // read-ahead buffer (heap; freed on close)
   int      bufferLen;    // valid bytes currently in the buffer
   uint64_t bufferStart;  // file offset the buffer begins at
} VideoSource;

int      openVideoSource(VideoSource *source, const char *path);            // 0 / -1
int64_t  readVideoSource(VideoSource *source, void *buffer, uint64_t length); // bytes read, or -1
int      seekVideoSource(VideoSource *source, uint64_t offset);             // absolute seek; 0 / -1
uint64_t tellVideoSource(const VideoSource *source);
uint64_t sizeVideoSource(const VideoSource *source);
void     closeVideoSource(VideoSource *source);

// read exactly `length` bytes at absolute `offset` (seek + full read). returns 0 on success,
// -1 on any short read or error. convenient for container parsers that want a fixed-size field.
int      readVideoSourceAt(VideoSource *source, uint64_t offset, void *buffer, uint64_t length);
