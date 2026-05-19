#pragma once

// shared framed wire protocol over a TCP stream.
//
// every frame on the wire is "<verb> <len>\n[<len bytes>]" where verb is a
// short ASCII tag (LOG / OK / ERR / REGISTER / ...). callers never split a
// frame across calls except for the streaming case (known total length,
// payload produced incrementally) which uses sendFrameHeader + sendBytes.
//
// used by both the bridge server (simple-debug-bridge/server.h) and the
// producer-side bridge client (bridge.h / future bridge-client.h).

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netex/net.h>

#include "printf.h"

// send a full buffer, retrying on partial writes. -1 on any socket error.
static inline int sendBytes(int fd, const void *buf, int len)
{
   const char *p = (const char *)buf;
   int remaining = len;
   while (remaining > 0) {
      int n = send(fd, p, remaining, 0);
      if (n <= 0) return -1;
      p += n;
      remaining -= n;
   }
   return len;
}

// read exactly len bytes into buf, blocking until satisfied. -1 if the
// peer disconnects or errors mid-read. counterpart to sendBytes.
static inline int receiveExact(int fd, void *buf, int len)
{
   char *p = (char *)buf;
   int remaining = len;
   while (remaining > 0) {
      int n = recv(fd, p, remaining, 0);
      if (n <= 0) return -1;
      p += n;
      remaining -= n;
   }
   return len;
}

// write just the "<verb> <len>\n" header. used when the caller streams the
// payload bytes itself (sendBytes / sendFileWindow / captureRegion).
static inline int sendFrameHeader(int fd, const char *verb, uint32_t len)
{
   char header[32];
   int headerLen = snprintf(header, sizeof header, "%s %u\n", verb, (unsigned)len);
   return sendBytes(fd, header, headerLen);
}

// write header + payload in one call. covers the common case where the
// payload is already buffered (text replies, log lines, REGISTER frames).
static inline int sendFrame(int fd, const char *verb, const void *payload, int len)
{
   if (sendFrameHeader(fd, verb, (uint32_t)len) < 0) return -1;
   if (len > 0 && sendBytes(fd, payload, len) < 0) return -1;
   return 0;
}

// read one line (up to '\n') from fd. NUL-terminates the buffer, strips a
// trailing '\r', returns the line length. -1 on disconnect / recv error.
// truncates silently at maxLen-1 and returns what was read.
static inline int receiveLine(int fd, char *buf, int maxLen)
{
   int offset = 0;
   while (offset < maxLen - 1) {
      int n = recv(fd, buf + offset, 1, 0);
      if (n <= 0) return -1;
      if (buf[offset] == '\n') {
         buf[offset] = '\0';
         if (offset > 0 && buf[offset - 1] == '\r') buf[--offset] = '\0';
         return offset;
      }
      offset++;
   }
   buf[offset] = '\0';
   return offset;
}
