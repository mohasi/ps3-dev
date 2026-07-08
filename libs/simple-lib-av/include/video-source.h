#pragma once

// video-source - a small seekable reader shared by the probe and the demuxers. Wraps either a local
// VfsFile or a streamed http(s):// url behind one read/seek interface with a tracked position, total
// size and a read-ahead buffer, so container parsers read/seek by absolute offset without caring whether
// the media is on disk or a remote stream.

#include <stdint.h>
#include "vfs.h"
#include "http.h"   // HttpStream, openHttpStream/... for url sources

// container parsing reads in tiny pieces (EBML/box headers a byte or a few at a time), so a
// read-ahead buffer turns thousands of one-byte reads into occasional block reads.
typedef struct {
   int         isOpen;
   int         isHttp;       // source kind: streamed url (http) vs local file (VfsFile)
   VfsFile     file;         // valid when !isHttp
   HttpStream *http;         // valid when isHttp
   uint64_t    size;         // total size in bytes
   uint64_t    pos;          // logical read position the caller sees
   uint64_t    filePos;      // actual backend position
   uint8_t    *buffer;       // read-ahead buffer (heap; freed on close)
   int         bufferLen;    // valid bytes currently in the buffer
   uint64_t    bufferStart;  // offset the buffer begins at
} VideoSource;

int      openVideoSource(VideoSource *source, const char *path);              // 0 / -1
int64_t  readVideoSource(VideoSource *source, void *buffer, uint64_t length); // bytes read, or -1
int      seekVideoSource(VideoSource *source, uint64_t offset);              // absolute seek; 0 / -1
uint64_t getVideoSourcePosition(const VideoSource *source);
uint64_t getVideoSourceSize(const VideoSource *source);
void     closeVideoSource(VideoSource *source);

// read exactly `length` bytes at absolute `offset` (seek + full read). returns 0 on success,
// -1 on any short read or error. convenient for container parsers that want a fixed-size field.
int      readVideoSourceAt(VideoSource *source, uint64_t offset, void *buffer, uint64_t length);
