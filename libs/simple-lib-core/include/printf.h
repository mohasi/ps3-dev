#pragma once

//
// Minimal printf header for VSH-injected PRX code.
//
// WHY THIS EXISTS
// ---------------
// libc_stub.a's printf-family thunks cannot be resolved at PRX load time
// inside vsh.self — the plugin fails to load silently. webMAN-MOD and
// IRISMAN hit the same wall and ship their own printf.c (Patrick Powell
// / Holger Weiss / Hector Martin port). We do the same.
//
// By declaring vsnprintf/sprintf/snprintf in this header and providing
// definitions in common/printf.c, any translation unit that includes
// "printf.h" instead of <stdio.h> gets the hand-compiled formatter.
// Object-file symbols beat archive-member symbols during the link, so
// even if libc_stub.a is still linked for memset/memcpy/etc., the
// printf-family references get bound to our versions — which contain
// no dynamic imports and load cleanly.
//
// SUPPORTED
// ---------
// %s %c %d %i %u %o %x %X %p %n %%
// Length modifiers: hh h l ll j t z
// Flags: - + space # 0 '
// Width, precision (including '*').
// NOT supported: float/double (%f %e %g %F %E %G %a %A) — stripped
// from the port because VSH PRX has no libm.
//
// strfmt() is a convenience that formats into a shared 512-byte scratch
// buffer and returns the pointer. Not reentrant. Don't nest calls.

#include <stdarg.h>
#include <stddef.h>  // size_t

#ifdef __cplusplus
extern "C" {
#endif

int vsnprintf(char *str, size_t size, const char *format, va_list args);
int vsprintf(char *buf, const char *fmt, va_list args);
int snprintf(char *buffer, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int sprintf(char *buffer, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
char *strfmt(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
