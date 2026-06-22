#pragma once
//
// path.h - pure '/'-rooted path-string helpers. libc-free, prx-safe, and
// completely filesystem-agnostic: nothing here opens, reads or routes a path,
// it only manipulates the string. (The VFS-backed file operations live in
// vfs.h / vfs-ops.c.) Split out of the old file.h so a consumer that just needs
// joinPath/getBaseName does not drag in the whole filesystem surface.
//
#include <stdint.h>
#include <stddef.h>             // NULL
#include "string-utilities.h"   // getStrLen, strCopy, strEq

#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 512
#endif

// joins dir + name into buf with exactly one separator. libc-free, prx-safe.
// returns buf on success, or NULL if the result did not fit in bufSize (buf is
// still left holding the NUL-terminated truncation so callers that ignore the
// return are no worse off than before, but tree walkers MUST check for NULL and
// abort the entry -- a silently truncated path is a wrong-target/incomplete op).
static inline char *joinPath(char *buf, int bufSize, const char *dir, const char *name)
{
   if (!buf || bufSize <= 0) return NULL;
   int o = 0, truncated = 0;
   while (dir[o] && o < bufSize - 1) { buf[o] = dir[o]; o++; }
   if (dir[o]) truncated = 1;                                  // dir itself overflowed
   if (o > 0 && buf[o - 1] != '/' && o < bufSize - 1) buf[o++] = '/';
   int i = 0;
   for (; name[i] && o < bufSize - 1; i++) buf[o++] = name[i];
   if (name[i]) truncated = 1;                                 // name (or separator) overflowed
   buf[o] = '\0';
   return truncated ? NULL : buf;
}

// truncates path in-place to its parent. "/a/b/c" -> "/a/b", "/a/b/" -> "/a",
// "/" stays "/". no-op on empty paths.
static inline void toParentPath(char *path)
{
   int len = getStrLen(path);
   if (len <= 1) return;
   if (path[len - 1] == '/') len--;
   while (len > 1 && path[len - 1] != '/') len--;
   if (len <= 1) { path[0] = '/'; path[1] = '\0'; }
   else          { path[len - 1] = '\0'; }   // cut at the separating '/', not after it
}

// copies the parent of path into parent without mutating the input.
static inline void getParentPath(const char *path, char *parent, int cap)
{
   if (!path || !parent || cap <= 0) return;

   strCopy(parent, cap, path);
   toParentPath(parent);
}

// returns the final path component (the name) of path. for "/a/b/c" -> "c",
// for "/a/b/" -> "" (trailing slash), for "name" -> "name". points into path.
static inline const char *getBaseName(const char *path)
{
   const char *b = path;
   for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
   return b;
}

// copies the device mount root of an absolute path into out: "/dev_usb000/x/y"
// -> "/dev_usb000", "/dev_hdd0/..." -> "/dev_hdd0", "/dev_blind" -> "/dev_blind".
// a bare root or a path with no second '/' is copied whole. returns out.
// out must be at least 33 bytes: vfs.h allows mount segments up to 31 chars, and a
// root is '/' + segment + NUL = 33 bytes for the longest segment. a shorter buffer
// truncates long roots and can make two distinct volumes compare equal (see
// moveTree). the copy is bounded by outSize, so a short buffer is memory-safe but
// yields a truncated -- and possibly colliding -- root.
static inline char *deviceRootOf(const char *path, char *out, int outSize)
{
   int o = 0;
   if (path[0] == '/' && o < outSize - 1) {
      out[o++] = '/';
      for (int i = 1; path[i] && path[i] != '/' && o < outSize - 1; i++)
         out[o++] = path[i];
   }
   out[o] = '\0';
   return out;
}

static inline const char *getExtension(const char *name)
{
   const char *dot = NULL;
   for (const char *p = name; *p; p++) {
      if (*p == '.') dot = p;
   }
   return dot ? dot + 1 : NULL;
}

// returns 1 if name is usable as a single path component: non-empty, shorter
// than MAX_PATH_LEN, not the "." or ".." aliases, and free of control bytes and
// the FAT/exFAT reserved characters (/ \ : * ? " < > |). use to vet names from
// untrusted sources (e.g. on-screen keyboard input) before a create or rename.
static inline int isValidFileName(const char *name)
{
   if (!name || name[0] == '\0') return 0;
   if (strEq(name, ".") || strEq(name, "..")) return 0;
   if (getStrLen(name) >= MAX_PATH_LEN) return 0;
   for (const unsigned char *c = (const unsigned char *)name; *c; c++) {
      if (*c < 0x20 || *c == 0x7F) return 0;  // control characters (incl. DEL)
      if (*c == '/' || *c == '\\' || *c == ':' || *c == '*' || *c == '?' ||
          *c == '"' || *c == '<'  || *c == '>' || *c == '|')
         return 0;
   }
   return 1;
}
