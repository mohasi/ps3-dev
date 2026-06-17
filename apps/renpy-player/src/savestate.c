#include "savestate.h"

#include <string.h>
#include <stdlib.h>     // malloc/free (thumbnail blob, larger than the shared buf)
#include <cell/rtc.h>   // CellRtcDateTime + UTC->local (config.time_format display)

#include "printf.h"   // snprintf
#include "config.h"   // RENPY_ROOT
#include "gamepath.h" // getGameSaveDir() -- RENPY_ROOT/<game>
#include "vars.h"     // getVarsCount / enumVars / setVar / resetVars / Value
#include "vm.h"       // getVmState / setVmState
#include "file.h"     // readFile / writeFile / makeDir / fileExists / cellFsStat
#include "dbg.h"      // logWarn
#include "image-loader.h"   // savePngArgb (SDK PNG encode for the slot screenshot)

#define SAVE_MAGIC 0x52505331u   // "RPS1"
// Bump whenever the serialised layout changes. v2 (2026-06-16): the scene block gained the `ingame`
// flag and the trailing sprite show-list; older v1 saves had a different tail, so loading one with the
// current reader misaligned and silently failed (return -4). The version check now rejects them cleanly.
#define SAVE_VER   2
#define BUF_CAP    (128 * 1024)

static char buf[BUF_CAP];   // shared serialise/parse scratch (one save in flight at a time)

// ---- path ----
static void slotPath(const char *display, char *out, int cap)
{
   snprintf(out, cap, "%s/slot-%s.sav", getGameSaveDir(), display);
}

void saveThumbPath(const char *display, char *out, int cap)
{
   snprintf(out, cap, "%s/slot-%s.png", getGameSaveDir(), display);
}

int saveThumbWrite(const char *display, const unsigned char *argb, int w, int h)
{
   if (!argb || w <= 0 || h <= 0) return -1;
   makeDir(RENPY_ROOT);
   makeDir(getGameSaveDir());
   char path[256]; saveThumbPath(display, path, sizeof path);
   return savePngArgb(path, argb, w, h);   // captured ARGB8888 thumbnail -> PNG (alpha forced opaque)
}

// ---- little raw writer over buf (host byte order; saves are same-machine) ----
typedef struct { char *p; int len, cap; int ok; } Wr;
static void wRaw(Wr *w, const void *src, int n)
{
   if (!w->ok || w->len + n > w->cap) { w->ok = 0; return; }
   memcpy(w->p + w->len, src, n); w->len += n;
}
static void wU32(Wr *w, uint32_t v) { wRaw(w, &v, 4); }
static void wI32(Wr *w, int v)      { wRaw(w, &v, 4); }
static void wI64(Wr *w, long long v){ wRaw(w, &v, 8); }
static void wF64(Wr *w, double v)   { wRaw(w, &v, 8); }
static void wStr(Wr *w, const char *s)
{
   if (!s) s = "";
   int n = (int)strlen(s);
   wU32(w, (uint32_t)n);
   wRaw(w, s, n);
}

// ---- reader ----
typedef struct { const char *p; int len, pos, ok; } Rd;
static void rRaw(Rd *r, void *dst, int n)
{
   if (!r->ok || r->pos + n > r->len) { r->ok = 0; return; }
   memcpy(dst, r->p + r->pos, n); r->pos += n;
}
static uint32_t rU32(Rd *r) { uint32_t v = 0; rRaw(r, &v, 4); return v; }
static int      rI32(Rd *r) { int v = 0; rRaw(r, &v, 4); return v; }
static long long rI64(Rd *r){ long long v = 0; rRaw(r, &v, 8); return v; }
static double   rF64(Rd *r) { double v = 0; rRaw(r, &v, 8); return v; }
// reads a length-prefixed string into dst (truncated to cap-1); always NUL-terminates.
static void rStr(Rd *r, char *dst, int cap)
{
   uint32_t n = rU32(r);
   if (!r->ok || r->pos + (int)n > r->len) { r->ok = 0; if (cap) dst[0] = '\0'; return; }
   int copy = ((int)n < cap - 1) ? (int)n : cap - 1;
   memcpy(dst, r->p + r->pos, copy); dst[copy] = '\0';
   r->pos += (int)n;
}

// ---- serialise ----
static int serialize(const SaveScene *sc, const char *saveName)
{
   Wr w = { buf, 0, BUF_CAP, 1 };
   wU32(&w, SAVE_MAGIC);
   wU32(&w, SAVE_VER);
   wStr(&w, saveName);

   int pc, sp, stack[VM_CALL_STACK_MAX];
   getVmState(&pc, &sp, stack);
   wI32(&w, pc); wI32(&w, sp);
   for (int i = 0; i < sp; i++) wI32(&w, stack[i]);

   int vc = getVarsCount();
   wI32(&w, vc);
   for (int i = 0; i < vc; i++)
   {
      const char *name = NULL; Value v = { VT_NONE, 0, 0, NULL };
      enumVars(i, &name, &v);          // always valid for i < getVarsCount()
      wStr(&w, name ? name : "");
      wI32(&w, (int)v.t);
      wI64(&w, (long long)v.i);
      wF64(&w, v.f);
      wStr(&w, v.t == VT_STR ? v.s : "");
   }

   wStr(&w, sc->scene);
   wStr(&w, sc->music);
   wI32(&w, sc->nvl);
   wI32(&w, sc->ingame);
   wStr(&w, sc->who);
   wStr(&w, sc->what);
   wI32(&w, sc->showCount);
   for (int i = 0; i < sc->showCount && i < SPR_MAX; i++) { wStr(&w, sc->shows[i]); wStr(&w, sc->showAt[i]); wI32(&w, sc->showAtl[i]); }

   return w.ok ? w.len : -1;
}

int saveStateCapture(const char *display, const SaveScene *sc, const char *saveName)
{
   int n = serialize(sc, saveName);
   if (n < 0) { logWarn("[rpp] save: serialise overflow\n"); return -1; }
   makeDir(RENPY_ROOT);        // ensure the games root exists, then this game's save folder
   makeDir(getGameSaveDir());
   char path[256]; slotPath(display, path, sizeof path);
   return writeFile(path, buf, (uint64_t)n) == 0 ? 0 : -1;
}

int saveStateLoad(const char *display, SaveScene *out)
{
   char path[256]; slotPath(display, path, sizeof path);
   int n = readFile(path, buf, BUF_CAP);
   if (n < 0) return -1;

   Rd r = { buf, n, 0, 1 };
   if (rU32(&r) != SAVE_MAGIC) return -2;
   if (rU32(&r) != SAVE_VER)   return -3;
   char saveName[64]; rStr(&r, saveName, sizeof saveName);   // label (already shown in the list)

   int pc = rI32(&r), sp = rI32(&r);
   int stack[VM_CALL_STACK_MAX];
   for (int i = 0; i < sp && i < VM_CALL_STACK_MAX; i++) stack[i] = rI32(&r);

   // Rebuild the variable store from scratch (setVar copies the strings into table storage).
   resetVars();
   int vc = rI32(&r);
   for (int i = 0; i < vc; i++)
   {
      char name[96], sval[256];
      rStr(&r, name, sizeof name);
      int t = rI32(&r);
      long long iv = rI64(&r);
      double fv = rF64(&r);
      rStr(&r, sval, sizeof sval);
      if (!r.ok) break;
      Value v; v.t = (ValType)t; v.i = (long)iv; v.f = fv; v.s = (t == VT_STR) ? sval : NULL;
      if (name[0]) setVar(name, &v, 0);
   }

   memset(out, 0, sizeof *out);
   rStr(&r, out->scene, sizeof out->scene);
   rStr(&r, out->music, sizeof out->music);
   out->nvl = rI32(&r);
   out->ingame = rI32(&r);
   rStr(&r, out->who,  sizeof out->who);
   rStr(&r, out->what, sizeof out->what);
   int showCount = rI32(&r);
   out->showCount = 0;
   for (int i = 0; i < showCount && i < SPR_MAX; i++)
   {
      rStr(&r, out->shows[out->showCount], sizeof out->shows[0]);
      rStr(&r, out->showAt[out->showCount], sizeof out->showAt[0]);
      out->showAtl[out->showCount] = rI32(&r);   // v2: restore the sprite's ATL id
      if (r.ok) out->showCount++;
   }
   if (!r.ok) return -4;

   setVmState(pc, sp, stack);
   return 0;
}

// ---- listing ----
int saveSlotExists(const char *display)
{
   char path[256]; slotPath(display, path, sizeof path);
   return fileExists(path);
}

// Format a unix time as Ren'Py's config.time_format "%b %d, %H:%M" (e.g. "Jun 14, 22:05").
static void formatTime(uint64_t unixTime, char *dst, int cap)
{
   static const char *mon[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
   CellRtcDateTime utc, local; CellRtcTick ut, lt;
   if (cap > 0) dst[0] = '\0';
   if (cellRtcSetTime_t(&utc, unixTime) < 0 || cellRtcGetTick(&utc, &ut) < 0 ||
       cellRtcConvertUtcToLocalTime(&ut, &lt) < 0 || cellRtcSetTick(&local, &lt) < 0) return;
   int m = (local.month >= 1 && local.month <= 12) ? local.month - 1 : 0;
   snprintf(dst, cap, "%s %02d, %02d:%02d", mon[m], local.day, local.hour, local.minute);
}

int saveSlotMtime(const char *display, long *out)
{
   char path[256]; slotPath(display, path, sizeof path);
   CellFsStat st;
   if (cellFsStat(path, &st) != CELL_FS_SUCCEEDED) return 0;
   if (out) *out = (long)st.st_mtime;
   return 1;
}

int saveSlotInfo(const char *display, char *outTime, int timeCap, char *outName, int nameCap)
{
   char path[256]; slotPath(display, path, sizeof path);
   CellFsStat st;
   if (cellFsStat(path, &st) != CELL_FS_SUCCEEDED) return 0;
   if (outTime && timeCap > 0) formatTime((uint64_t)st.st_mtime, outTime, timeCap);

   if (outName && nameCap > 0)
   {
      outName[0] = '\0';
      int n = readFile(path, buf, BUF_CAP);
      if (n > 0)
      {
         Rd r = { buf, n, 0, 1 };
         if (rU32(&r) == SAVE_MAGIC && rU32(&r) == SAVE_VER) rStr(&r, outName, nameCap);
      }
   }
   return 1;
}

