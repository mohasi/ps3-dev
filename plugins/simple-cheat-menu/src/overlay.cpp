#include "overlay.h"
#include "cheat-sync.h"         // syncMode + settings, buildCheatPath, getAppVersion, isGameTitleId
#include "string-utilities.h"   // memSet: libc-free fill (memset doesn't resolve in a vsh prx)
#include "game-mem.h"           // readProcMem/writeProcMem + getGameScanRanges (game-mem.c)
#include "texture-patch.h"      // listPatchNames / applyPatch: the Patches tab's disk + apply backend

#include <string>
#include <new>
#include <stdint.h>
#include <sys/timer.h>   // sys_timer_usleep: bounded waits in the live Update handshake

// Draw a box over a running game through vsh's PAF compositor (never the raw
// framebuffer — that hard-locked us). A PAF widget is a C++ object whose second
// field is a std::string name; PAF reads that name during its per-frame widget
// walk. Constructing the object into a *zeroed* buffer leaves the string's
// internal pointer null, so the walk faults and the console hard-locks. The fix
// is to let the C++ compiler construct the object (so the string is a valid,
// empty COW string) and only then hand it to PAF's own constructor, exactly as
// the proven VshFpsCounter reference does. We never set a name, so the string
// stays empty and never mallocs (libc malloc doesn't resolve in a vsh prx). The
// widget objects themselves live in a heap arena (OverlayArena) taken from lv2 on
// demand — see overlayEnsureArena — not in the C++ heap.
//
// All hardware-verified in-game 2026-07-09: backdrop plane (construct, layout,
// colour) and title text (PhText ctor, vtable-70 SetText with a firmware-layout
// wstring, render styles). Rules learned the hard way (FREEZE-WRITEUP.md):
// - never reuse a paf widget pointer across the XMB<->game boundary; re-find it
//   per use, on the frame thread.
// - any string crossing into firmware must use the Dinkumware SSO layout with
//   16-bit chars (FirmwareWstring below), never our GCC COW strings.
// Placement-new into the arena's storage is fine; PhPlane needs no SetName.
// Field offsets mirror vsh's PhWidget layout (size 0x290) from the community
// PAF headers.
//
// Storage (Rung 1): the on-screen widgets follow the 10 visible SLOTS, not the
// cheats, so widget memory is fixed however many cheats a title has; the cheat
// data itself packs into shared pools (no per-cheat caps). See OverlayArena.

// step-logging shim, defined in prx.c (C). overlay.cpp must not include the
// C-only dbg.h, so prx.c lends us this. each call flushes a line to disk
// before the next paf call, so a hang leaves the last-reached step on disk.
extern "C" void overlayLog(const char *msg);
extern "C" void overlayLogHex(const char *msg, unsigned int value);

// cheat file loader. readFile is the prx-safe, allocation-free vfs primitive
// (unmatched paths route to cellFs, no initVfs needed); declared here so we
// don't pull the C-only vfs.h into this c++ TU.
extern "C" int readFile(const char *path, char *buffer, int capacity);

// on-demand heap (lv2 pages) for the arena and the transient parse buffer. shimmed
// through prx.c so this c++ TU stays clear of the C-only syscall.h. alloc returns 0
// on failure.
extern "C" void *overlayHeapAlloc(unsigned int size);
extern "C" void  overlayHeapFree(void *ptr);

// m5 apply path: the running game's pid (vsh export, 0 = no game). the game-process
// memory accessors + segment enumeration come from game-mem.h above.
extern "C" unsigned int vshmain_0624D3AE(void);

// paf exports, resolved by NID from libpaf_export_stub.a. the asm label binds
// each readable name to its NID symbol; the trailing comment gives the real
// firmware method for cross-referencing community headers.
extern "C" {
   uint32_t findPafView(const char *pluginName)                              __asm__("paf_F21655F3"); // paf::View::Find
   uint32_t findPafViewWidget(uint32_t view, const char *widgetName)         __asm__("paf_794CEACB"); // paf::View::FindWidget
   void     constructPafPlane(void *plane, void *parent, void *appear)       __asm__("paf_D0197A7D"); // paf::PhPlane::PhPlane
   void     constructPafText(void *text, void *parent, void *appear)         __asm__("paf_7F0930C6"); // paf::PhText::PhText
   void     updatePafLayoutPos(void *widget)                                 __asm__("paf_BF4B155C"); // paf::PhWidget::UpdateLayoutPos
   void     updatePafLayoutSize(void *widget)                                __asm__("paf_DF031EDD"); // paf::PhWidget::UpdateLayoutSize
   int      killPafTimerCallback(void *handler, int callbackId)              __asm__("paf_2CBA5A33"); // paf::PhHandler::KillTimerCB
   void     setPafTextStyleInt(void *textRender, int style, int value)       __asm__("paf_983EA578"); // paf::PhSText::SetStyle(int, int)
   void     setPafTextStyleFloat(void *textRender, int style, float value)   __asm__("paf_165AD4A6"); // paf::PhSText::SetStyle(int, float)
   void     preparePafWidgetUpdate(void *widget)                             __asm__("paf_384F93FC"); // paf::PhWidget::UpdatePrepare
   void    *getPafPluginInterface(uint32_t view, int identifier)             __asm__("paf_23AFB290"); // paf plugin GetInterface
}

#define PAF_COLOR_HANDLER       0x1000002   // PhHandler::ColorHandler
#define PAF_VTABLE_SET_TEXT     70          // PhText::SetText slot (from the reference)
#define PHTEXT_SIZE             0x2A4       // PhWidget 0x290 + PhText's 0x14 extra

// SetText is virtual: (this, const std::wstring &, 0), where the wstring is the
// FIRMWARE's std::wstring — the SDK's Dinkumware SSO layout (0x1C bytes: 4-byte
// allocator pad, 16-byte union of 8 inline 16-bit chars / heap pointer, length
// at 0x14, capacity at 0x18; wchar_t on PS3 is 16-bit per sdk yvals.h). NOT the
// GCC COW single-pointer string — passing that froze the console (SetText read
// length/capacity from garbage past the object and did a wild copy).
typedef void (*PafSetTextFn)(void *widget, const void *fwWstring, int unused);

// two modes by capacity: < 8 keeps the text in the inline chars; >= 8 makes
// the 0x04 union a pointer to a char buffer (see setPafWidgetText). the union
// makes the pointer a real member — no type-punning the char array.
struct FirmwareWstring {
   unsigned int allocatorPad;         // 0x00
   union {
      unsigned short  inlineChars[8]; // 0x04  SSO text when inline
      unsigned short *heapBuffer;     // 0x04  buffer pointer when heap
   } text;
   unsigned int length;               // 0x14  _Mysize
   unsigned int capacity;             // 0x18  _Myres; < 8 = inline, >= 8 = heap
};

// text render styles (PhSText::SetStyle). 0x28 is line height per the
// reference; 0x13=0x70 is set by the reference at every text widget's
// creation, meaning undocumented — text stays invisible without both.
#define PAF_STYLE_TEXT_SETUP   0x13
#define PAF_STYLE_LINE_HEIGHT  0x28

#define HEADER_TEXT_HEIGHT     22.0f
#define SUBTITLE_TEXT_HEIGHT   16.0f
#define ROW_TEXT_HEIGHT        18.0f
#define FOOTER_TEXT_HEIGHT     16.0f   // two stacked hint lines

// native XMB button glyphs: the imagefont.bin private-use codepoints (UTF-8 encoded),
// which PAF's PhText renders straight from the system font like XMB's own button hints.
#define GLYPH_CROSS    "\xEF\xA2\x81"   // U+F881
#define GLYPH_CIRCLE   "\xEF\xA2\x80"   // U+F880
#define GLYPH_TRIANGLE "\xEF\xA2\x83"   // U+F883
#define GLYPH_SQUARE   "\xEF\xA2\x82"   // U+F882
#define GLYPH_SELECT   "\xEF\xA2\x8E"   // U+F88E
#define GLYPH_START    "\xEF\xA2\x8F"   // U+F88F
#define GLYPH_PS       "\xEF\xA2\x92"   // U+F892
#define GLYPH_L1       "\xEF\xA2\x88"   // U+F888
#define GLYPH_R1       "\xEF\xA2\x8B"   // U+F88B

// panel sizing. height grows one row at a time between a 1-row min and a 10-row
// max window; more cheats than the window scroll into view. derived so the header
// pins to the top edge whatever the height. widths/heights are in the paf
// viewport's centered coords. 10 rows = 523px, well inside the screen.
#define PANEL_WIDTH          644.0f   // ~15% wider than the original 560
#define PANEL_ROW_HEIGHT      36.0f
#define PANEL_HEADER_BLOCK    92.0f   // header + subtitle + top padding
#define PANEL_FOOTER_BLOCK    93.0f   // bottom band: two 16px hint lines with 25px below, 12px between, 24px above
#define PANEL_MIN_ROWS         1
#define PANEL_MAX_ROWS        10      // rows visible at once (the scroll window / slot count)
#define ROW_LEFT_MARGIN     24.0f   // cheat name left-aligns this far in from the panel's left edge
#define ROW_RIGHT_MARGIN    24.0f   // right-most pill hugs this far in from the panel's right edge

// per-row tags: small centered text, no background. the AoB tag sits in a fixed column at the row's
// left edge (before the name), so no name-width measuring is needed; the name is indented past it. the
// crowd score badge hugs the right side.
#define TAG_TEXT_HEIGHT     13.0f
#define AOB_TAG_WIDTH       42.0f   // width of the fixed AoB column at the row's left edge
#define AOB_NAME_GAP        10.0f   // gap between the AoB column and the name
#define AOB_TAG_Y_OFFSET    -2.0f   // nudge the smaller tag text down so it sits level with the name
#define SCORE_TAG_WIDTH     40.0f   // fits "100%"

// on/off toggle, right-most on each row: green ON / grey OFF text right-aligned
// to the row's right margin. TOGGLE_GAP is deliberately wide so the toggle reads
// as a control, not another tag.
#define TOGGLE_TEXT_HEIGHT  14.0f
#define TOGGLE_WIDTH        34.0f
#define TOGGLE_GAP          23.0f   // gap between the score badge and the on/off toggle
#define HIGHLIGHT_HEIGHT    32.0f   // selection bar height (a little under the row pitch)
#define HIGHLIGHT_INSET      6.0f   // bar shrinks this far from each panel edge
#define HIGHLIGHT_Y_OFFSET   -3.0f  // nudge the bar up so it sits centered on the text

// horizontal alignment styles (PhWidget::SetStyle int overload). 0x31 justifies
// the glyphs, 0x12 moves the anchor to the matching edge so the position x is
// that edge; set both. values: 0 center, 1 left, 2 right.
#define PAF_STYLE_TEXT_ALIGN   0x31
#define PAF_STYLE_ANCHOR       0x12
#define PAF_ALIGN_CENTER       0
#define PAF_ALIGN_LEFT         1
#define PAF_ALIGN_RIGHT        2

namespace {

// Minimal mirror of vsh's PhWidget data layout. We name only the fields we
// touch; everything else is opaque bytes the firmware constructor fills —
// including the widget's real name string at 0x004 (a 0x1C-byte Dinkumware
// std::string, not our GCC one; our member is effectively padding and pad0 is
// sized from sizeof(std::string) so the offsets we use stay exact).
struct PafWidget {
   void        *vtable;                                        // 0x000
   std::string  name;                                          // 0x004
   char         pad0[0x0F0 - 0x004 - sizeof(std::string)];     // .. 0x0F0
   void        *renderHandle;                                  // 0x0F0 sRender; PhSText at +0x28
   char         pad0b[0x120 - 0x0F4];                          // .. 0x120
   float        colorScaleRGBA[4];                             // 0x120
   char         pad1[0x250 - 0x130];                           // .. 0x250
   int          positionFactor[3];                             // 0x250 x,y,z
   float        positionLayout[4];                             // 0x25C vec4
   int          sizeFactor[3];                                 // 0x26C x,y,z
   float        sizeLayout[4];                                 // 0x278 vec4
   char         pad2[0x290 - 0x288];                           // .. 0x290
};

// overlay chrome: the dimmer, the panel box, the selection bar, the header /
// subtitle / footer. one each, placement-new'd into the arena.
PafWidget *dimmer = 0;
PafWidget *box = 0;
PafWidget *highlightBar = 0;
int        highlightIndex = -1;        // the on-screen SLOT the bar sits on (not the cheat index)
int        footerForRow = -1;          // patch row the footer's Options hint currently reflects (-1 = force refresh)
int        scrollOffset = 0;           // first cheat shown in the window; cheat i sits at slot i-scrollOffset
float      highlightPanelTop = 0.0f;
void      *pageNotification = 0;
unsigned int builtUnder = 0;   // page_notification the widgets were parented to
volatile int visible = 0;      // volatile: frame thread writes, menu thread reads (overlayRequestToggle)
volatile int forgetOnHide = 0; // menu thread arms this on game exit; the frame thread's next hide then drops the widgets untouched (they died with the game) instead of re-colouring freed memory
int       findFailLogged = 0;
PafWidget *label = 0;
PafWidget *subtitle = 0;
PafWidget *footer = 0;     // top hint line (toggle / xmb / resume)
PafWidget *footer2 = 0;    // bottom hint line (mark working / mark failed)
PafWidget *message = 0;    // centered status shown over the (hidden) rows during an update

// header/subtitle text built by overlayPrepareForTitle on the menu thread and
// read by overlayShowBox on the frame thread (so the frame thread never parses).
// lastParsedTitle guards the re-parse: same title -> keep the parsed pools and
// their resolved aob addresses; a new title -> parse fresh.
char       preppedHeader[80];
char       preppedSubtitle[48];
char       lastParsedTitle[16] = { 0 };

// the render slots: one widget set per on-screen row (PANEL_MAX_ROWS of them),
// reused as the list scrolls — so widget memory is fixed no matter how many cheats
// a title has. layoutAndPaintRows fills each slot with whatever cheat scrolls into
// it (its text is set there, per scroll). indexed by slot 0..PANEL_MAX_ROWS-1.
PafWidget *rowSlot[PANEL_MAX_ROWS]     = { 0 };
PafWidget *toggleSlot[PANEL_MAX_ROWS]  = { 0 };
PafWidget *aobSlot[PANEL_MAX_ROWS]     = { 0 };
PafWidget *scoreSlot[PANEL_MAX_ROWS]   = { 0 };

// packed cheat storage. no per-cheat caps: names/ops/aob-patterns pack
// into shared pools sized to fill the 64KB page, so a cheat costs only what it
// actually uses. the only limits are the pool totals (a file that overflows them
// truncates + logs). MAX_CHEATS headers, OP_POOL_SIZE ops across all cheats.
#define MAX_CHEATS       256
#define OP_POOL_SIZE     512
#define TEXT_BLOB_SIZE   4096    // cheat names, null-terminated
#define AOB_BLOB_SIZE    3072    // find/replace patterns at their real length
#define SNAP_BLOB_SIZE   2048    // write-op original bytes for revert (aob reverts via its own find pattern)
#define MATCH_POOL_SIZE  256     // aob match addresses across all ON cheats (write-all needs every match)
#define CHEAT_FLAG_HAS_AOB 0x01

enum CheatOpKind { CHEAT_OP_WRITE = 0, CHEAT_OP_AOB = 1 };

// one decoded code line. a WRITE op pokes `value` (low `width` bytes) at `address`,
// snapshotting the width original bytes at snapOffset for revert. an AOB op scans for
// the pattern at findOffset and writes the pattern at replaceOffset (`width` bytes)
// over EVERY match; the match addresses live in matchPool[matchStart .. +matchCount)
// and it reverts by writing the find pattern back to each (the original bytes at a
// match ARE the find pattern). matchStart/matchCount are runtime — set on enable,
// cleared on disable.
struct CheatOp {
   unsigned char  kind;          // CheatOpKind
   unsigned char  width;         // write: 1/2/4 bytes; aob: pattern length
   unsigned int   address;       // write target; aob: unused (scanned)
   unsigned int   value;         // write value (low `width` bytes); aob: unused
   unsigned short findOffset;    // aob: into aobBlob
   unsigned short replaceOffset; // aob: into aobBlob
   unsigned short snapOffset;    // write: into snapBlob (width original bytes)
   unsigned short matchStart;    // aob runtime: into matchPool
   unsigned short matchCount;    // aob runtime: matches currently poked
};

// one cheat: a name + a run of ops in the shared pool. matchStart/matchCount (runtime) bound this
// cheat's slice of matchPool while ON, so disable can free the whole slice at once.
struct Cheat {
   unsigned short nameOffset;    // into textBlob
   unsigned short firstOp;       // into opPool
   unsigned char  opCount;
   unsigned char  flags;         // CHEAT_FLAG_*
   unsigned short matchStart;    // runtime: into matchPool
   unsigned short matchCount;    // runtime: aob matches held while ON
};

// aob scan window. Rung 1 keeps the fixed [SCAN_START, SCAN_END) heuristic and
// first-match; Rung 3 swaps the window for the game's real module segments and
// Rung 2 makes it write-all.
#define SCAN_START   0x10000u
#define SCAN_END     0x800000u   // 8 MB fallback window (used only when segment enumeration is unavailable)
#define SCAN_PAGE    4096          // page granularity: a cobra read is all-or-nothing, so it must not cross a mapping hole
#define SCAN_CHUNK   (16 * 1024)   // bulk read size: 1/4 the syscalls of per-page, but small enough for the kernel's per-read malloc
#define SCAN_ABORTED (-1)          // scanGameForMatches sentinel: user cancelled mid-scan
#define MAX_SCAN_RANGES  32        // game module segments to scan (a game has ~a dozen)

// the whole cheat file is read into a transient page allocated only for the parse
// (freed right after), so nothing caps the file size and no permanent buffer sits
// in the arena. one 64KB page holds ~1000 cheats' worth of text.
#define FILE_READ_CAP  (64 * 1024)

// the on-demand heap: 10 slot widgets + chrome + the packed cheat pools, ~61KB in
// one 64KB page. allocated on the first menu open and then KEPT — reused across
// games (the parent-changed rebuild path reconstructs the widgets). we do NOT free
// it on game exit: our widgets are children of page_notification, and vsh walks
// them when it tears the in-game XMB down; freeing first is a use-after-free that
// panics lv2 (confirmed on hardware). safe freeing needs real widget teardown
// (unparent from vsh) — a deferred rung. still zero resident until first invoked.
struct OverlayArena {
   char dimmerStorage[sizeof(PafWidget)]       __attribute__((aligned(16)));
   char boxStorage[sizeof(PafWidget)]          __attribute__((aligned(16)));
   char highlightBarStorage[sizeof(PafWidget)] __attribute__((aligned(16)));
   char labelStorage[PHTEXT_SIZE]              __attribute__((aligned(16)));
   char subtitleStorage[PHTEXT_SIZE]           __attribute__((aligned(16)));
   char footerStorage[PHTEXT_SIZE]             __attribute__((aligned(16)));
   char footer2Storage[PHTEXT_SIZE]            __attribute__((aligned(16)));
   char messageStorage[PHTEXT_SIZE]            __attribute__((aligned(16)));   // centered "Updating…" during an update
   char rowStorage[PANEL_MAX_ROWS][PHTEXT_SIZE]     __attribute__((aligned(16)));
   char toggleStorage[PANEL_MAX_ROWS][PHTEXT_SIZE]  __attribute__((aligned(16)));
   char aobStorage[PANEL_MAX_ROWS][PHTEXT_SIZE]     __attribute__((aligned(16)));
   char scoreStorage[PANEL_MAX_ROWS][PHTEXT_SIZE]   __attribute__((aligned(16)));   // crowd % badge, coloured by verdict

   Cheat         cheats[MAX_CHEATS];
   CheatOp       opPool[OP_POOL_SIZE];
   char          textBlob[TEXT_BLOB_SIZE];
   unsigned char aobBlob[AOB_BLOB_SIZE];
   unsigned char snapBlob[SNAP_BLOB_SIZE];    // write-op revert snapshots
   unsigned int  matchPool[MATCH_POOL_SIZE];  // aob match addresses of the ON cheats
};

OverlayArena *arena = 0;
int cheatCount = 0;   // parsed cheats (<= MAX_CHEATS), each a render slot as it scrolls in
int cheatTotal = 0;   // name: blocks in the file (may exceed MAX_CHEATS)
int matchPoolUsed = 0;   // high-water of matchPool held by ON cheats (compacted on disable)

// the menu has two tabs. the Cheats tab lists parsed cheats; the Patches tab lists the title's texture
// patch folders and applies one. L1/R1 switch tabs (menu thread). the Patches tab reuses the same row
// widgets (no arena room for a second set), so activeTab picks which list layoutAndPaintRows paints.
enum OverlayTab { TAB_CHEATS = OVERLAY_TAB_CHEATS, TAB_PATCHES = OVERLAY_TAB_PATCHES };
volatile int activeTab = TAB_CHEATS;

// the running title's patch folder names (patches/<titleId>/<name>), listed on open + tab switch. kept
// in BSS, not the arena — tiny, and the arena is packed full. gameName is kept so the header line can be
// rebuilt with the active tab's word ("Cheats" / "Patches") without another firmware name lookup.
#define MAX_PATCHES     32
#define PATCH_NAME_CAP  40
#define MAX_PATCH_PARTS 24   // parts a single patch can expose (see texture-patch getPatchParts)
char patchNames[MAX_PATCHES][PATCH_NAME_CAP];
char patchApplied[MAX_PATCHES];   // 1 = patch is doing something in the game right now; toggled by ✕
unsigned char patchHasParts[MAX_PATCHES];   // 1 = the patch exposes parts (Triangle drills into its options)
unsigned char patchPartOn[MAX_PATCHES][MAX_PATCH_PARTS];   // per-part on/off for a parts patch (in memory, like patchApplied)
int  patchCount = 0;
char gameName[64];

// drill-in: when >= 0, the Patches tab is showing the parts of patchNames[drillPatch] instead of the
// patch list. drillParts holds that patch's part metadata; the live on/off is patchPartOn[drillPatch].
int drillPatch = -1;
PatchPart drillParts[MAX_PATCH_PARTS];
int drillPartCount = 0;

// crowd score for the running (titleId, version), parsed from the compiled file's `score` lines and
// kept in fixed memory (not the arena — tiny, and keeps arena room for cheats). NO_SCORE = the file
// carries no evidence for this build.
#define NO_SCORE 0xFF
#define LOW_SCORE_AMBER 50   // a green (works-here) badge needs a crowd score above this — a low-score cheat
                             // that merely matched a value is not trustworthy (the 25%-score cheat that
                             // locked the console had matched), so show caution amber, not inviting green.
unsigned char  cheatConfidence[MAX_CHEATS];

// local build compatibility per cheat, from live memory vs the crowd's working-val originals:
// UNKNOWN = nothing to check against (or memory unreadable); OK = live bytes match a known-good
// original; FAIL = they match none (the cheat was proven on builds unlike this one). colours the badge.
enum CheatVerdict { VERDICT_UNKNOWN, VERDICT_OK, VERDICT_FAIL };
unsigned char cheatVerdict[MAX_CHEATS];

// FNV-1a of each cheat's op lines (its online identity, for the vote path). kept out of the arena so
// the arena has room for the score-badge widgets; the vote path reads it via overlayGetCheatHash.
unsigned int cheatOpHash[MAX_CHEATS];

// display order: a permutation of 0..cheatCount-1 sorted by crowd score (high first), ties broken
// alphabetically — so the cheats most likely to work here float to the top. rebuilt on each parse.
// the menu navigates and paints in display rows; getRowCheat(row) maps a row to its real cheat index.
unsigned short displayOrder[MAX_CHEATS];

// per-cheat on/off state machine across three threads, kept in BSS (not the arena) —
// tiny at MAX_CHEATS, and simplest for the lock-free access from the worker / menu /
// frame threads:
//   rowDesiredOn - user intent. MENU writes on toggle/cancel; WORKER also forces it to
//                  the terminal value (OFF on enable-fail, ON on revert-fail) so a dead
//                  job settles instead of hanging PENDING. two writers by design; the
//                  race is benign: char stores are atomic, the state converges, worst
//                  case a one-tick display glitch a re-press heals.
//   rowServiced  - the last desire the worker finished (worker writes)
//   rowApplied   - whether the cheat is live in game memory (worker writes)
//   rowFailed    - the last poke didn't apply or revert (worker writes); shown as FAIL. rowDesiredOn
//                  is PRESERVED on failure (not flipped), so the worker always retries toward the
//                  user's intent — a failed revert (desired=off) retries revert, never re-applies.
// desired!=serviced -> PENDING; else failed -> FAIL; else applied -> ON/OFF.
volatile char rowDesiredOn[MAX_CHEATS] = { 0 };
volatile char rowServiced[MAX_CHEATS]  = { 0 };
volatile char rowApplied[MAX_CHEATS]   = { 0 };
volatile char rowFailed[MAX_CHEATS]    = { 0 };
volatile int  jobsActive = 0;          // worker services rows only while the menu is open
volatile int  workerBusy = 0;          // worker is inside applyCheat (reading/poking the pools); the Update park waits on this
volatile int  togglePaintDirty = 0;    // a state changed; the frame thread repaints

// live Update (Triangle) coordination. re-parsing rewrites the cheat pools, so the frame thread
// must not read them mid-parse. the menu thread sets refreshFreeze and waits for the frame thread
// to park (refreshFrozen); after the parse it lifts the freeze and sets contentDirty so the frame
// thread repaints once (via overlayFlushContent, which also guards against freed widgets).
volatile int  refreshFreeze = 0;
volatile int  refreshFrozen = 0;

// the cheats that were ON at the start of an Update, captured by a name hash (indices/order can
// change across the re-parse). after the new file is parsed, any cheat whose name still matches
// is re-enabled so the worker re-applies it fresh — the user keeps their selection through an update.
unsigned int savedOnHashes[MAX_CHEATS];
int          savedOnCount = 0;

// update mode: while an Update is in flight the rows/highlight are hidden and a centered message is
// shown in their place (our dimmer hides the XMB toasts, so progress is shown inside the panel). all
// menu input except XMB/PS is a no-op meanwhile. contentDirty asks the frame thread to switch modes.
volatile int  updating = 0;
const char   *updateMessage = 0;   // string literal shown centered while updating (set by menu thread)
volatile int  contentDirty = 0;    // frame thread re-lays the content (rows vs message) on the next tick
volatile int  pendingReload = 0;   // an update finished while the menu was closed: reload on next open

// FNV-1a hash of a cheat name — a stable identity across a re-parse (the pools move, the name
// doesn't). used to carry the ON selection through a live Update.
unsigned int hashCheatName(const char *name)
{
   unsigned int hash = 2166136261u;
   for (const char *p = name; *p; p++) { hash ^= (unsigned char)*p; hash *= 16777619u; }
   return hash;
}

// accessors into the packed pools (arena must be non-null).
const char *getCheatName(int c) { return arena->textBlob + arena->cheats[c].nameOffset; }
int         cheatHasAob(int c)  { return (arena->cheats[c].flags & CHEAT_FLAG_HAS_AOB) != 0; }

// the real cheat index shown at a display row (rows are score-sorted; see displayOrder).
int getRowCheat(int row) { return (row >= 0 && row < cheatCount) ? displayOrder[row] : 0; }

// sort key: higher crowd score first (NO_SCORE sinks to the bottom), ties broken alphabetically.
int cheatSortsBefore(int a, int b)
{
   int scoreA = cheatConfidence[a] == NO_SCORE ? -1 : cheatConfidence[a];
   int scoreB = cheatConfidence[b] == NO_SCORE ? -1 : cheatConfidence[b];
   if (scoreA != scoreB) return scoreA > scoreB;
   return strCmpICase(getCheatName(a), getCheatName(b)) < 0;
}

// rebuild displayOrder for the parsed cheats. insertion sort — cheatCount <= MAX_CHEATS and this runs
// once per parse (never per frame), so O(n^2) is fine and needs no scratch buffer.
void buildDisplayOrder()
{
   for (int i = 0; i < cheatCount; i++) displayOrder[i] = (unsigned short)i;
   for (int i = 1; i < cheatCount; i++) {
      unsigned short cheat = displayOrder[i];
      int j = i;
      while (j > 0 && cheatSortsBefore(cheat, displayOrder[j - 1])) { displayOrder[j] = displayOrder[j - 1]; j--; }
      displayOrder[j] = cheat;
   }
}

// allocate the arena on first use (menu thread). returns 0 if the page couldn't be
// taken, so the caller can refuse to open rather than fault. zero-filled to match
// the old BSS semantics (some fields are read before they are written).
OverlayArena *overlayEnsureArena()
{
   if (arena) return arena;
   arena = (OverlayArena *)overlayHeapAlloc((unsigned int)sizeof(OverlayArena));
   if (!arena) { overlayLog("arena: alloc failed"); return 0; }
   memSet(arena, 0, sizeof(OverlayArena));
   matchPoolUsed = 0;
   lastParsedTitle[0] = 0;   // fresh arena holds no parsed cheats: force a re-parse
   return arena;
}

enum ToggleDisplay { TOGGLE_OFF = 0, TOGGLE_PENDING = 1, TOGGLE_ON = 2, TOGGLE_FAIL = 3 };

// the text each state paints. PENDING is the ellipsis "..." (queued or scanning) —
// spelled out here so it reads as a deliberate label, not a stray placeholder. FAIL is
// a poke that didn't apply or revert (see rowFailed).
#define TOGGLE_LABEL_ON        "ON"
#define TOGGLE_LABEL_OFF       "OFF"
#define TOGGLE_LABEL_PENDING   "..."
#define TOGGLE_LABEL_FAIL      "FAIL"

// shared scratch for building a firmware wstring of any length. SetText copies
// the source synchronously into the widget's own string, so one scratch buffer
// serves every call. wchar_t on ps3 is 16-bit, so the buffer is 16-bit chars.
unsigned short  textScratch[128];
FirmwareWstring textSource;

// set a widget's text from a UTF-8 C string, decoded to 16-bit units via the shared
// utf8ToUtf16 (wchar_t is 16-bit on ps3, and game titles are UTF-8 with the odd
// ® / ™ — decoding avoids mojibake). <=7 units use the Dinkumware SSO inline union;
// longer strings put a pointer to our scratch in the union with capacity >= 8 to
// select heap mode (SetText then copies via vsh's allocator, never ours). astral code
// points become a surrogate pair; malformed bytes are dropped.
void setPafWidgetText(PafWidget *widget, const char *text)
{
   const int maxChars = (int)(sizeof(textScratch) / sizeof(textScratch[0])) - 1;
   utf8ToUtf16(text, textScratch, maxChars);
   int length = 0;
   while (textScratch[length]) length++;

   textSource.allocatorPad = 0;
   textSource.length = length;
   if (length <= 7) {
      for (int i = 0; i <= length; i++) textSource.text.inlineChars[i] = textScratch[i];
      textSource.capacity = 7;      // < 8 selects inline mode
   } else {
      textSource.text.heapBuffer = textScratch;   // union pointer at 0x04
      textSource.capacity = length;   // >= 8 selects heap mode
   }
   ((PafSetTextFn)((void **)widget->vtable)[PAF_VTABLE_SET_TEXT])(widget, &textSource, 0);
}

// direct m_Data writes + the matching update export — the reference's
// SetPosition/SetSize/SetColor idiom (factors 6,5,0 = viewport coords,
// colour committed by killing paf's colour animation timer).
void setPafWidgetPosition(PafWidget *widget, float x, float y)
{
   widget->positionFactor[0] = 6; widget->positionFactor[1] = 5; widget->positionFactor[2] = 0;
   widget->positionLayout[0] = x; widget->positionLayout[1] = y; widget->positionLayout[2] = 0.0f; widget->positionLayout[3] = 0.0f;
   updatePafLayoutPos(widget);
}

void setPafWidgetSize(PafWidget *widget, float width, float height)
{
   widget->sizeFactor[0] = 6; widget->sizeFactor[1] = 5; widget->sizeFactor[2] = 0;
   widget->sizeLayout[0] = width; widget->sizeLayout[1] = height; widget->sizeLayout[2] = 0.0f; widget->sizeLayout[3] = 0.0f;
   updatePafLayoutSize(widget);
}

void setPafWidgetColor(PafWidget *widget, float red, float green, float blue, float alpha)
{
   widget->colorScaleRGBA[0] = red; widget->colorScaleRGBA[1] = green; widget->colorScaleRGBA[2] = blue;
   widget->colorScaleRGBA[3] = alpha;
   killPafTimerCallback(widget, PAF_COLOR_HANDLER);
}

// construct a PhText into arena storage with its text, size and position.
// the render styles (0x13=0x70 setup + 0x28 height) are required or the text
// is invisible; colour is applied later via setOverlayVisible.
PafWidget *makeTextWidget(void *storage, const char *text, float x, float y, float height, int align)
{
   memSet(storage, 0, PHTEXT_SIZE);   // clear any stale prior widget in this storage
   PafWidget *widget = new (storage) PafWidget;   // default-init: memSet already zeroed; no redundant zero-fill
   constructPafText(widget, pageNotification, 0);
   setPafWidgetText(widget, text);

   void *renderHandle = widget->renderHandle;
   void *textRender = renderHandle ? *(void **)((char *)renderHandle + 0x28) : 0;
   if (textRender) {
      setPafTextStyleInt(textRender, PAF_STYLE_TEXT_SETUP, 0x70);
      setPafTextStyleFloat(textRender, PAF_STYLE_LINE_HEIGHT, height);
   }

   // horizontal alignment (only when non-center, so the proven centered path for
   // the header/subtitle is untouched). PhWidget::SetStyle(int,int) dispatches to
   // the render object's vtable method 12 with (sRender, style, value), and
   // renderHandle IS sRender - matches the VshFpsCounter reference.
   if (renderHandle && align != PAF_ALIGN_CENTER) {
      typedef void (*StyleIntFn)(void *sRender, int style, int value);
      void **renderVtable = *(void ***)renderHandle;
      StyleIntFn setStyle = (StyleIntFn)renderVtable[12];
      setStyle(renderHandle, PAF_STYLE_TEXT_ALIGN, align);
      setStyle(renderHandle, PAF_STYLE_ANCHOR, align);
   }
   preparePafWidgetUpdate(widget);
   setPafWidgetPosition(widget, x, y);
   return widget;
}

// construct a PhPlane into arena storage at a position/size. colour is applied
// later via setOverlayVisible. used for the dimmer, the highlight bar and the
// panel box (all the same shape, so it lives in one place).
PafWidget *makePlaneWidget(void *storage, float x, float y, float width, float height)
{
   memSet(storage, 0, sizeof(PafWidget));   // clear any stale prior widget
   PafWidget *widget = new (storage) PafWidget;   // default-init: memSet already zeroed; no redundant zero-fill
   constructPafPlane(widget, pageNotification, 0);
   setPafWidgetPosition(widget, x, y);
   setPafWidgetSize(widget, width, height);
   return widget;
}

// centered-coord y of a row's center. the row band starts PANEL_HEADER_BLOCK
// below the panel's top edge and steps down one PANEL_ROW_HEIGHT per row.
float getRowCenterY(int index)
{
   return highlightPanelTop - PANEL_HEADER_BLOCK - (index + 0.5f) * PANEL_ROW_HEIGHT;
}

// copy the value after a "key:" prefix on the current line (blanks trimmed, stopped
// at end-of-line) into blob at *used, null-terminated. returns the blob offset of
// the copied string, or 0 (the guaranteed-empty string at textBlob[0]) if it didn't
// fit. buffer/start/end bound the source.
unsigned short copyValueToBlob(const char *buffer, int start, int end, int prefixLen, char *blob, int *used, int cap)
{
   int at = start + prefixLen;
   while (at < end && (buffer[at] == ' ' || buffer[at] == '\t')) at++;
   int begin = *used;
   while (at < end && buffer[at] != '\n' && buffer[at] != '\r' && *used < cap - 1)
      blob[(*used)++] = buffer[at++];
   // only a real overflow (value content still pending, blob full) rewinds to the empty
   // string; a value that ended exactly at cap-1 fit and is kept.
   if (at < end && buffer[at] != '\n' && buffer[at] != '\r') { *used = begin; return 0; }
   blob[(*used)++] = 0;
   return (unsigned short)begin;
}

// read one whitespace-delimited hex token as a uint32 (low 32 bits). skips
// leading blanks, stops at the first non-hex char, advances *at past it, and
// reports the digit count so the caller can tell "no token" (0) from a real 0.
unsigned int readHexToken(const char *buffer, int *at, int end, int *digits)
{
   while (*at < end && (buffer[*at] == ' ' || buffer[*at] == '\t')) (*at)++;
   unsigned int value = 0;
   int count = 0;
   for (int nibble; *at < end && (nibble = hexDigit(buffer[*at])) >= 0; (*at)++) {
      value = (value << 4) | (unsigned int)nibble;
      count++;
   }
   *digits = count;
   return value;
}

// read a run of hex digits (two chars per byte) into blob at *used, up to cap.
// returns the byte count; a trailing odd nibble ends the run.
int readHexBytesToBlob(const char *buffer, int *at, int end, unsigned char *blob, int *used, int cap)
{
   while (*at < end && (buffer[*at] == ' ' || buffer[*at] == '\t')) (*at)++;
   int count = 0;
   while (*at + 1 < end && *used < cap) {
      int high = hexDigit(buffer[*at]);
      int low  = hexDigit(buffer[*at + 1]);
      if (high < 0 || low < 0) break;
      blob[(*used)++] = (unsigned char)((high << 4) | low);
      count++;
      *at += 2;
   }
   return count;
}

// decode a single code line into op, packing aob patterns into the aob blob at
// *aobUsed. handles "w8/w16/w32 <addr> <val>" and "aob <find> <repl>". returns 1 on
// a recognised, well-formed line, else 0. tokens self-terminate at the newline.
int parseCheatOp(const char *buffer, int start, int end, CheatOp *op, unsigned char *aobBlob, int *aobUsed)
{
   op->kind = CHEAT_OP_WRITE; op->width = 0; op->address = 0; op->value = 0;
   op->findOffset = 0; op->replaceOffset = 0; op->snapOffset = 0; op->matchStart = 0; op->matchCount = 0;
   int at = start;

   // aob: two byte patterns packed into the aob blob, no address (scanned on enable)
   if (startsWith(buffer + start, "aob")) {
      at += 3;
      op->kind = CHEAT_OP_AOB;
      int findStart = *aobUsed;
      int findLen = readHexBytesToBlob(buffer, &at, end, aobBlob, aobUsed, AOB_BLOB_SIZE);
      int replStart = *aobUsed;
      int replLen = readHexBytesToBlob(buffer, &at, end, aobBlob, aobUsed, AOB_BLOB_SIZE);
      op->findOffset = (unsigned short)findStart;
      op->replaceOffset = (unsigned short)replStart;
      // reject an empty or >255-byte pattern: width is a byte, so a longer pattern would
      // truncate mod 256 and mass-match a short prefix. reclaim the find bytes on reject.
      int patternLen = findLen < replLen ? findLen : replLen;
      if (patternLen <= 0 || patternLen > 255) { *aobUsed = findStart; return 0; }
      op->width = (unsigned char)patternLen;
      return 1;
   }

   // write: opcode sets the width, then <addr> <val>
   int width = 0;
   if      (startsWith(buffer + start, "w32")) { width = 4; at += 3; }
   else if (startsWith(buffer + start, "w16")) { width = 2; at += 3; }
   else if (startsWith(buffer + start, "w8"))  { width = 1; at += 2; }
   else return 0;

   int digits = 0;
   op->kind = CHEAT_OP_WRITE;
   op->width = (unsigned char)width;
   op->address = readHexToken(buffer, &at, end, &digits);
   if (digits == 0) return 0;
   op->value = readHexToken(buffer, &at, end, &digits);
   return digits > 0;
}

// does this line begin a cheat op? first whitespace token in {w8,w16,w32,aob}, exactly what
// reconcile.py tests — so the plugin hashes the same set of lines the server does.
int isOpLine(const char *line)
{
   return startsWith(line, "w8 ") || startsWith(line, "w16 ") || startsWith(line, "w32 ") || startsWith(line, "aob ");
}

// fold one op line into a cheat's running op-hash — the cheat's online identity, matched against
// the repo by the vote path. mirrors reconcile.py's opsHash exactly: op lines joined with '\n', any
// trailing " working-val=..." suffix stripped, then FNV-1a. streamed one line at a time so we never
// keep the raw text. the caller has already confirmed this is an op line (isOpLine).
void hashOpLine(unsigned int *opHash, const char *buffer, int lineStart, int bufferEnd, int *hashedAny)
{
   int end = lineStart;
   while (end < bufferEnd && buffer[end] != '\n') end++;
   if (end > lineStart && buffer[end - 1] == '\r') end--;   // crlf files: reconcile strips the \r
   for (int k = lineStart; k + 12 <= end; k++) {            // drop a trailing " working-val=<...>"
      if (startsWith(buffer + k, "working-val=")) {
         end = k;
         while (end > lineStart && (buffer[end - 1] == ' ' || buffer[end - 1] == '\t')) end--;
         break;
      }
   }
   unsigned int hash = *opHash;
   if (*hashedAny) { hash ^= (unsigned char)'\n'; hash *= 16777619u; }
   for (int k = lineStart; k < end; k++) { hash ^= (unsigned char)buffer[k]; hash *= 16777619u; }
   *opHash = hash;
   *hashedAny = 1;
}

// read a base-10 number, skipping any leading non-digits (a space, or the '+'/':' separators in a
// score line). advances *at past the digits. used only inside a single known-format score line.
int readDecimal(const char *buffer, int *at, int end)
{
   while (*at < end && (buffer[*at] < '0' || buffer[*at] > '9')) (*at)++;
   int value = 0;
   while (*at < end && buffer[*at] >= '0' && buffer[*at] <= '9') { value = value * 10 + (buffer[*at] - '0'); (*at)++; }
   return value;
}

// parse a "score <titleId> <version>: <confidence> ..." line, but only accept the one whose
// titleId+version match the running build (the "proven here" line; other variants' lines are ignored
// for now). fills confidence (the displayed %) and returns 1 on a match, else 0.
int parseScoreForBuild(const char *buffer, int start, int end, const char *titleId, const char *version, int *confidence)
{
   int at = start + 6;   // past "score "
   for (int k = 0; titleId[k]; k++, at++) if (at >= end || buffer[at] != titleId[k]) return 0;
   if (at >= end || buffer[at] != ' ') return 0;
   at++;
   for (int k = 0; version[k]; k++, at++) if (at >= end || buffer[at] != version[k]) return 0;
   if (at >= end || buffer[at] != ':') return 0;
   at++;
   *confidence = readDecimal(buffer, &at, end);
   return 1;
}

// working-val evidence for one op line: if it carries a `working-val=<hex>:<n>,...`, read the 4 live
// bytes at `address` and check them against the listed originals. returns 1 (live matches one), 0 (has
// data but matches none, read ok), or -1 (no working-val on the line, or the read failed — no evidence).
int checkWorkingVal(const char *buffer, int lineStart, int bufferEnd, unsigned int pid, unsigned int address)
{
   int lineEnd = lineStart;
   while (lineEnd < bufferEnd && buffer[lineEnd] != '\n') lineEnd++;

   int at = -1;
   for (int k = lineStart; k + 12 <= lineEnd; k++) if (startsWith(buffer + k, "working-val=")) { at = k + 12; break; }
   if (at < 0) return -1;   // no crowd byte-data for this line

   unsigned char live[4];
   if (!pid || readProcMem(pid, address, live, 4) != 0) return -1;   // can't read -> no evidence
   unsigned int liveValue = ((unsigned int)live[0] << 24) | ((unsigned int)live[1] << 16) | ((unsigned int)live[2] << 8) | live[3];

   // originals are "<hex>:<count>,<hex>:<count>,..."; live matches if it equals any <hex>.
   while (at < lineEnd) {
      unsigned int original = 0;
      int digits = 0;
      for (int nibble; at < lineEnd && (nibble = hexDigit(buffer[at])) >= 0; at++) { original = (original << 4) | (unsigned int)nibble; digits++; }
      if (digits > 0 && original == liveValue) return 1;
      while (at < lineEnd && buffer[at] != ',') at++;   // skip this original's :count to the next
      if (at < lineEnd) at++;                            // past the ','
   }
   return 0;   // has data, no match
}

// parse the running title's file into the packed pools; the results land in the
// globals cheatCount / cheatTotal. one pass: a "name:" line starts a new cheat
// (its ops follow); write/aob lines decode into the op pool. the whole file is
// read into a transient page freed at the end, so file size isn't capped.
// cheatTotal counts every block even past MAX_CHEATS; cheatCount is what we hold.
void parseCheatsForTitle(const char *titleId, const char *version)
{
   cheatCount = 0;
   cheatTotal = 0;
   if (!titleId[0]) return;

   unsigned int gamePid = vshmain_0624D3AE();   // for the working-val live-memory check (0 = no game running)

   char path[160];
   buildCheatPath(path, sizeof(path), titleId);

   char *file = (char *)overlayHeapAlloc(FILE_READ_CAP);
   if (!file) { overlayLog("cheats: parse buffer alloc failed"); return; }
   int bytes = readFile(path, file, FILE_READ_CAP);
   if (bytes <= 0) { overlayHeapFree(file); return; }   // no local file is the normal no-cheats case

   arena->textBlob[0] = 0;   // offset 0 is the shared empty string (overflow / no-value fallback)
   int textUsed = 1, opUsed = 0, aobUsed = 0, snapUsed = 0;
   int current = -1;      // cheat whose following lines we're reading (-1 = none / past cap)
   int hashedOp = 0;      // has the current cheat fed any op line into its hash yet (for the '\n' join)
   for (int i = 0; i < bytes; ) {
      if (startsWith(file + i, "name:")) {
         cheatTotal++;
         hashedOp = 0;
         if (cheatCount < MAX_CHEATS) {
            current = cheatCount++;
            Cheat *c = &arena->cheats[current];
            c->nameOffset = copyValueToBlob(file, i, bytes, 5, arena->textBlob, &textUsed, TEXT_BLOB_SIZE);
            c->firstOp = (unsigned short)opUsed;
            c->opCount = 0;
            c->flags = 0;
            c->matchStart = 0;
            c->matchCount = 0;
            cheatOpHash[current] = 2166136261u;   // FNV-1a offset basis; hashOpLine folds each op line in
            cheatConfidence[current] = NO_SCORE;
            cheatVerdict[current] = VERDICT_UNKNOWN;
         } else {
            current = -1;   // past the header cap; still counted in cheatTotal
         }
      } else if (current >= 0) {
         Cheat *c = &arena->cheats[current];
         if (startsWith(file + i, "score ")) {
            int confidence;
            if (parseScoreForBuild(file, i, bytes, titleId, version, &confidence))
               cheatConfidence[current] = (unsigned char)(confidence < 0 ? 0 : confidence > 254 ? 254 : confidence);   // % is 0..100, never overflows
         } else if (isOpLine(file + i)) {
            // hash EVERY op line (before the pool caps below) so the identity matches reconcile even
            // if a later op overflows a pool and isn't stored.
            hashOpLine(&cheatOpHash[current], file, i, bytes, &hashedOp);
            if (opUsed < OP_POOL_SIZE && c->opCount < 255) {   // opCount is a byte: cap so it never wraps
               CheatOp *op = &arena->opPool[opUsed];
               if (parseCheatOp(file, i, bytes, op, arena->aobBlob, &aobUsed)) {
                  // local verdict: only w32 ops carry a working-val. FAIL sticks (any mismatch = red);
                  // a match sets OK unless already FAIL; no working-val leaves the cheat UNKNOWN.
                  if (op->kind == CHEAT_OP_WRITE && op->width == 4) {
                     int wv = checkWorkingVal(file, i, bytes, gamePid, op->address);
                     if (wv == 0)                                                cheatVerdict[current] = VERDICT_FAIL;
                     else if (wv == 1 && cheatVerdict[current] != VERDICT_FAIL)  cheatVerdict[current] = VERDICT_OK;
                  }
                  // only write ops need a revert snapshot; an aob op reverts by writing
                  // its find pattern back to each match, so it reserves no snapBlob space.
                  int snapNeed = op->kind == CHEAT_OP_WRITE ? op->width : 0;
                  if (snapUsed + snapNeed <= SNAP_BLOB_SIZE) {
                     op->snapOffset = (unsigned short)snapUsed;
                     snapUsed += snapNeed;
                     opUsed++;
                     c->opCount++;
                     if (op->kind == CHEAT_OP_AOB) c->flags |= CHEAT_FLAG_HAS_AOB;
                  } else {
                     overlayLog("cheats: snapshot blob full - op dropped");
                  }
               }
            }
         }
      }
      while (i < bytes && file[i] != '\n') i++;   // to line end
      i++;                                        // past '\n'
   }

   overlayHeapFree(file);
   buildDisplayOrder();   // score-sort the rows now that every cheat's confidence is known
   if (cheatTotal > cheatCount) overlayLogHex("cheats: header cap hit, total", (unsigned int)cheatTotal);
   overlayLogHex("cheats: parsed", (unsigned int)cheatCount);
}

// the running game's title id and name from game_plugin. GetInterface(view, 1)
// returns a struct of fw function pointers; gameInfo (slot 8) fills a 0x120
// buffer with the title id at +0x04 and the name at +0x14. there is no version
// field in it (confirmed by dumping the buffer), and getStr can't read the
// running title's PARAM.SFO from vsh (CELL_GAME_ERROR_FAILURE), so the running
// game's version isn't shown — each cheat row shows the version it targets.
void getGameHeader(char *titleIdOut, char *nameOut)
{
   titleIdOut[0] = nameOut[0] = 0;
   uint32_t view = findPafView("game_plugin");
   if (!view) return;
   void *pluginInterface = getPafPluginInterface(view, 1);
   if (!pluginInterface) return;

   char info[0x120];
   typedef int (*GameInfoFn)(void *buffer);
   ((GameInfoFn)((void **)pluginInterface)[8])(info);
   strCopy(titleIdOut, 11, info + 0x04);   // "BCES-01742" is 10 chars + NUL; 10 would clip the last digit
   strCopy(nameOut, 64, info + 0x14);
}

// "<name> - <word>" (word is the active tab, "Cheats" / "Patches"), or just "<word>" when the name
// lookup failed. the leading " - " is part of the separator, so pass the bare tab word.
void buildHeaderText(char *out, const char *name, const char *word)
{
   int n = 0;
   if (name[0]) {
      while (name[n] && n < 63) { out[n] = name[n]; n++; }
      const char *sep = " - ";
      for (int i = 0; sep[i] && n < 76; i++) out[n++] = sep[i];
   }
   for (int i = 0; word[i] && n < 79; i++) out[n++] = word[i];
   out[n] = 0;
}

// the header word for the current view: the tab name, or the patch name when drilled into its options.
const char *getTabWord()
{
   if (activeTab == TAB_PATCHES && drillPatch >= 0) return patchNames[drillPatch];
   return activeTab == TAB_PATCHES ? "Patches" : "Cheats";
}

// the displayed state of a cheat, derived from the lock-free flags: a transition in flight
// (desired != last serviced) is PENDING; a settled cheat that failed its last poke is FAIL; otherwise
// ON/OFF from whether it is applied.
int getToggleDisplay(int cheat)
{
   if (rowDesiredOn[cheat] != rowServiced[cheat]) return TOGGLE_PENDING;
   if (rowFailed[cheat]) return TOGGLE_FAIL;
   return rowApplied[cheat] ? TOGGLE_ON : TOGGLE_OFF;
}

// paint a slot's on/off widget from the cheat currently in it (alpha 0 hides).
void paintToggleByState(int slot, int cheat, float alpha)
{
   PafWidget *toggle = toggleSlot[slot];
   if (!toggle) return;
   int state = (alpha > 0.0f && cheat >= 0 && cheat < cheatCount) ? getToggleDisplay(cheat) : TOGGLE_OFF;
   switch (state) {
      case TOGGLE_ON:      setPafWidgetText(toggle, TOGGLE_LABEL_ON);      setPafWidgetColor(toggle, 0.204f, 0.827f, 0.600f, alpha); break;  // emerald
      case TOGGLE_PENDING: setPafWidgetText(toggle, TOGGLE_LABEL_PENDING); setPafWidgetColor(toggle, 0.984f, 0.749f, 0.141f, alpha); break;  // amber
      case TOGGLE_FAIL:    setPafWidgetText(toggle, TOGGLE_LABEL_FAIL);    setPafWidgetColor(toggle, 0.937f, 0.325f, 0.314f, alpha); break;  // red
      default:             setPafWidgetText(toggle, TOGGLE_LABEL_OFF);     setPafWidgetColor(toggle, 0.392f, 0.455f, 0.545f, alpha); break;  // slate
   }
}

// rows in the active tab's list (cheats or patches). every layout/scroll/nav path reads this, so a
// tab switch resizes the panel and re-clamps the selection without any per-site tab checks.
int getRowCount()
{
   if (activeTab == TAB_PATCHES && drillPatch >= 0) return drillPartCount;   // drilled into a patch: its parts
   return activeTab == TAB_PATCHES ? patchCount : cheatCount;
}

// number of rows on screen at once: the whole list when it fits, else the window.
int getWindowRows()
{
   int rows = getRowCount();
   return rows < PANEL_MAX_ROWS ? rows : PANEL_MAX_ROWS;
}

// (re)list the running title's patch folders into patchNames, carrying each patch's on/off state across
// the re-list by name — the same way cheats keep their selection through a re-parse. called on open and
// on every switch to the Patches tab (a deploy may have added folders since last time).
void loadPatchList(const char *titleId)
{
   // remember each patch's on/off state (whole + per-part) by name, so it survives the re-list — the
   // same way cheats keep their selection through a re-parse.
   unsigned int oldHash[MAX_PATCHES];
   char oldApplied[MAX_PATCHES];
   unsigned char oldPartOn[MAX_PATCHES][MAX_PATCH_PARTS];
   int oldCount = patchCount;
   for (int i = 0; i < oldCount; i++) {
      oldHash[i] = hashCheatName(patchNames[i]);
      oldApplied[i] = patchApplied[i];
      for (int p = 0; p < MAX_PATCH_PARTS; p++) oldPartOn[i][p] = patchPartOn[i][p];
   }

   patchCount = listPatchNames(titleId, patchNames[0], PATCH_NAME_CAP, MAX_PATCHES);
   for (int i = 0; i < patchCount; i++) {
      patchApplied[i] = 0;
      for (int p = 0; p < MAX_PATCH_PARTS; p++) patchPartOn[i][p] = 0;
      unsigned int hash = hashCheatName(patchNames[i]);
      for (int j = 0; j < oldCount; j++) if (oldHash[j] == hash) {
         patchApplied[i] = oldApplied[j];
         for (int p = 0; p < MAX_PATCH_PARTS; p++) patchPartOn[i][p] = oldPartOn[j][p];
         break;
      }
      PatchPart probe[MAX_PATCH_PARTS];   // a patch with parts is drilled into (Triangle), not applied whole
      patchHasParts[i] = (unsigned char)(getPatchParts(titleId, patchNames[i], probe, MAX_PATCH_PARTS) > 0);
   }
   drillPatch = -1;   // a re-list invalidates the drilled-in view (indices may have moved)
}

// small unsigned decimal (0..999) into out, null-terminated. for the score badge.
void writeDecimal(char *out, int value)
{
   if (value < 0) value = 0;
   int n = 0;
   if (value >= 100) out[n++] = (char)('0' + value / 100);
   if (value >= 10)  out[n++] = (char)('0' + (value / 10) % 10);
   out[n++] = (char)('0' + value % 10);
   out[n] = 0;
}

// the score-badge colour for a local verdict: green (works here) / amber (unknown) / red (won't).
void getVerdictColor(int verdict, float *red, float *green, float *blue)
{
   if (verdict == VERDICT_OK)        { *red = 0.204f; *green = 0.827f; *blue = 0.600f; }   // emerald
   else if (verdict == VERDICT_FAIL) { *red = 0.937f; *green = 0.325f; *blue = 0.314f; }   // red
   else                              { *red = 0.984f; *green = 0.749f; *blue = 0.141f; }   // amber (unknown)
}

// fill each render slot with the cheat currently scrolled into it (slot s shows
// cheat scrollOffset+s): set its name/aob/score/toggle text, position it, and
// colour it (alpha 0 hides). drives both the initial show and each scroll step, so
// one function owns the slot layout. runs on the frame thread (called from show/scroll).
void layoutAndPaintRows(int show)
{
   float aobCenterX   = -PANEL_WIDTH * 0.5f + ROW_LEFT_MARGIN + AOB_TAG_WIDTH * 0.5f;          // fixed left column
   float nameLeftX    = -PANEL_WIDTH * 0.5f + ROW_LEFT_MARGIN + AOB_TAG_WIDTH + AOB_NAME_GAP;   // name indented past it
   float rightEdge    =  PANEL_WIDTH * 0.5f - ROW_RIGHT_MARGIN;
   float toggleCenter = rightEdge - TOGGLE_WIDTH * 0.5f;

   // the Patches tab reuses these same slots to list patch names — just the name column, no aob/score. when
   // drilled into a patch it lists that patch's parts instead, each with an on/off toggle.
   int onPatchesTab = (activeTab == TAB_PATCHES);
   int onPartsList  = (onPatchesTab && drillPatch >= 0);
   int rowCount = getRowCount();

   // sweep ALL slots (not just the current window): slots past the count paint at alpha 0, so a
   // list that shrank on a live Update doesn't leave stale rows visible below the shorter panel.
   for (int s = 0; s < PANEL_MAX_ROWS; s++) {
      int   row      = scrollOffset + s;
      int   onScreen = show && row >= 0 && row < rowCount;
      int   cheat    = (onScreen && !onPatchesTab) ? getRowCheat(row) : 0;   // cheat rows are score-sorted; map to the real cheat
      float alpha    = onScreen ? 1.0f : 0.0f;
      float chromeAlpha = onPatchesTab ? 0.0f : alpha;   // the toggle/aob/score chrome belongs to the Cheats tab only
      float rowY     = getRowCenterY(s);

      if (rowSlot[s]) {
         const char *name = !onScreen ? "" : onPartsList ? drillParts[row].name : onPatchesTab ? patchNames[row] : getCheatName(cheat);
         setPafWidgetText(rowSlot[s], name);
         setPafWidgetPosition(rowSlot[s], onPatchesTab ? nameLeftX - AOB_TAG_WIDTH - AOB_NAME_GAP : nameLeftX, rowY);   // no AoB column on the Patches tab: shift the name to the left margin
         setPafWidgetColor(rowSlot[s], 0.945f, 0.961f, 0.976f, alpha);   // SLATE_100 name
      }
      if (toggleSlot[s]) setPafWidgetPosition(toggleSlot[s], toggleCenter, rowY);

      // AoB tag: fixed left column, shown only for an on-screen aob cheat (alpha 0 hides it otherwise)
      if (aobSlot[s]) {
         setPafWidgetPosition(aobSlot[s], aobCenterX, rowY + AOB_TAG_Y_OFFSET);
         int showAob = onScreen && !onPatchesTab && cheatHasAob(cheat);
         setPafWidgetColor(aobSlot[s], 0.220f, 0.741f, 0.973f, showAob ? 1.0f : 0.0f);   // SKY_400
      }

      // crowd score % on the right, coloured by the local build verdict (green ok / amber unknown / red won't)
      if (scoreSlot[s]) {
         if (onScreen && !onPatchesTab && cheatConfidence[cheat] != NO_SCORE) {
            char scoreLabel[8];
            writeDecimal(scoreLabel, cheatConfidence[cheat]);
            int labelLen = 0; while (scoreLabel[labelLen]) labelLen++;
            scoreLabel[labelLen] = '%'; scoreLabel[labelLen + 1] = '\0';
            setPafWidgetText(scoreSlot[s], scoreLabel);
            setPafWidgetPosition(scoreSlot[s], rightEdge - TOGGLE_WIDTH - TOGGLE_GAP - SCORE_TAG_WIDTH * 0.5f, rowY);
            int verdict = cheatVerdict[cheat];
            if (verdict == VERDICT_OK && cheatConfidence[cheat] <= LOW_SCORE_AMBER) verdict = VERDICT_UNKNOWN;  // low score: don't invite with green
            float red, green, blue;
            getVerdictColor(verdict, &red, &green, &blue);
            setPafWidgetColor(scoreSlot[s], red, green, blue, alpha);
         } else if (onScreen && onPatchesTab && !onPartsList && patchHasParts[row]) {
            setPafWidgetText(scoreSlot[s], GLYPH_TRIANGLE);   // "has options" marker; Triangle drills in
            setPafWidgetPosition(scoreSlot[s], rightEdge - TOGGLE_WIDTH - TOGGLE_GAP - SCORE_TAG_WIDTH * 0.5f, rowY);
            setPafWidgetColor(scoreSlot[s], 0.580f, 0.639f, 0.722f, alpha);   // SLATE_400 hint
         } else {
            setPafWidgetColor(scoreSlot[s], 0.5f, 0.5f, 0.5f, 0.0f);   // hide
         }
      }
      if (toggleSlot[s]) {
         if (onPatchesTab) {
            // parts list: the part's own on/off; patch list: the whole patch's on/off. ✕ flips either.
            int on = onScreen && (onPartsList ? patchPartOn[drillPatch][row] : patchApplied[row]);
            setPafWidgetText(toggleSlot[s], on ? TOGGLE_LABEL_ON : TOGGLE_LABEL_OFF);
            if (on) setPafWidgetColor(toggleSlot[s], 0.204f, 0.827f, 0.600f, alpha);   // emerald ON
            else    setPafWidgetColor(toggleSlot[s], 0.392f, 0.455f, 0.545f, alpha);   // slate OFF
         } else {
            paintToggleByState(s, cheat, chromeAlpha);
         }
      }
   }
}

// hide/show without destroying: alpha 0 keeps the widgets alive, so re-show is
// just a colour write (widget destruction is its own unproven rung, deferred)
void setOverlayVisible(int show)
{
   // colours from simple-lib-app's slate palette (the app sample's clear look):
   // dimmer = near-black 90% over the frozen xmb; panel = SLATE_900 (the app's
   // clear colour); header = SLATE_100; subtitle = SLATE_400.
   setPafWidgetColor(dimmer, 0.02f, 0.02f, 0.04f, show ? 0.9f : 0.0f);
   setPafWidgetColor(box, 0.059f, 0.090f, 0.165f, show ? 1.0f : 0.0f);   // SLATE_900 #0F172A
   if (highlightBar) setPafWidgetColor(highlightBar, 0.200f, 0.255f, 0.333f, show ? 1.0f : 0.0f); // SLATE_700 selection
   setPafWidgetColor(label, 0.945f, 0.961f, 0.976f, show ? 1.0f : 0.0f); // SLATE_100 #F1F5F9
   setPafWidgetColor(subtitle, 0.580f, 0.639f, 0.722f, show ? 1.0f : 0.0f); // SLATE_400 #94A3B8

   float alpha = show ? 1.0f : 0.0f;
   if (footer)  setPafWidgetColor(footer,  0.392f, 0.455f, 0.545f, alpha); // SLATE_500 hint
   if (footer2) setPafWidgetColor(footer2, 0.392f, 0.455f, 0.545f, alpha); // SLATE_500 hint
   if (message) setPafWidgetColor(message, 0.945f, 0.961f, 0.976f, 0.0f);  // shown only via repaintContent while updating
   layoutAndPaintRows(show);   // slots filled from the current scroll window
   visible = show;
   jobsActive = show;   // worker services rows only while the menu is open
   if (show) contentDirty = 1;   // apply the current mode (normal / mid-update) on this open
}

// clear the runtime applied-cheat state: what is live in the game and the aob matches
// resolved for it. this state belongs to ONE game process — its addresses are meaningless
// in any other — so a new game (or a widget rebuild) must start from a clean slate.
void resetAppliedState()
{
   for (int c = 0; c < MAX_CHEATS; c++)
      rowDesiredOn[c] = rowServiced[c] = rowApplied[c] = rowFailed[c] = 0;   // fresh game: all off
   matchPoolUsed = 0;   // no cheats ON -> no aob matches held
}

// drop every widget pointer and reset per-cheat state WITHOUT touching the widgets
// (touching stale ones is the freeze). used by the rebuild path (before building
// anew under a new parent) and by teardown (before freeing the arena they lived in).
void forgetOverlayWidgets()
{
   box = dimmer = highlightBar = label = subtitle = footer = footer2 = message = 0;
   for (int s = 0; s < PANEL_MAX_ROWS; s++)
      rowSlot[s] = toggleSlot[s] = aobSlot[s] = scoreSlot[s] = 0;
   resetAppliedState();
   highlightIndex = -1;
   scrollOffset = 0;
}

}  // namespace

extern "C" unsigned int overlayFindPageNotification(void)
{
   uint32_t view = findPafView("system_plugin");
   if (!view) { pageNotification = 0; return 0; }
   pageNotification = (void *)(uintptr_t)findPafViewWidget(view, "page_notification");
   return (unsigned int)(uintptr_t)pageNotification;
}

// the Patches list footer comes in two forms: a plain patch applies with ✕ (base line); a patch with
// parts is opened with ✕ and drilled into with △ (options line). overlayHighlightRow swaps between them
// as the highlight moves, so the Options hint only shows for a patch that actually has options.
#define FOOTER_PATCH_LIST     GLYPH_L1 GLYPH_R1 " Tabs   " GLYPH_CROSS " Apply   " GLYPH_SQUARE " Dump   " GLYPH_CIRCLE " XMB   " GLYPH_PS " Resume"
#define FOOTER_PATCH_OPTIONS  GLYPH_L1 GLYPH_R1 " Tabs   " GLYPH_CROSS " Open   " GLYPH_TRIANGLE " Options   " GLYPH_SQUARE " Dump   " GLYPH_CIRCLE " XMB   " GLYPH_PS " Resume"
#define FOOTER_PATCH_EMPTY    GLYPH_L1 GLYPH_R1 " Tabs   " GLYPH_SQUARE " Dump   " GLYPH_CIRCLE " XMB   " GLYPH_PS " Resume"

// the normal top hint line for the current mode. update mode shows a shorter line (below) with
// only the buttons that still work.
namespace { const char *getFooterTopLine(void)
{
   if (activeTab == TAB_PATCHES) {
      if (drillPatch >= 0)   // inside a patch's options: ✕ toggles a part, ○ backs out to the patch list
         return GLYPH_CROSS " Toggle    " GLYPH_CIRCLE " Back    " GLYPH_PS " Resume";
      if (patchCount == 0)   // nothing to apply; dumping is still how you make a patch
         return FOOTER_PATCH_EMPTY;
      return FOOTER_PATCH_LIST;   // base line (no Options); overlayHighlightRow swaps in the Options variant per row
   }
   return (syncMode != SYNC_OFFLINE)
      ? GLYPH_L1 GLYPH_R1 " Tabs    " GLYPH_CROSS " Toggle    " GLYPH_TRIANGLE " Update    " GLYPH_CIRCLE " XMB    " GLYPH_PS " Resume"
      : GLYPH_L1 GLYPH_R1 " Tabs    " GLYPH_CROSS " Toggle    " GLYPH_CIRCLE " XMB    " GLYPH_PS " Resume";
} }
#define FOOTER_UPDATING  GLYPH_CIRCLE " XMB    " GLYPH_PS " Resume"

// (re)position the panel and its fixed chrome for the current cheat count, so a count change
// (a live Update, or a reopen after one) resizes the box and moves the header/subtitle/footer
// without rebuilding any widget. the rows themselves are positioned by layoutAndPaintRows. the
// footer is one line during an update (mark line hidden) — same as a non-contribute mode.
namespace { void layoutPanel(void)
{
   int rowCount = getRowCount();
   int visibleRows = rowCount < PANEL_MIN_ROWS ? PANEL_MIN_ROWS
                   : rowCount > PANEL_MAX_ROWS ? PANEL_MAX_ROWS : rowCount;
   float panelHeight = PANEL_HEADER_BLOCK + visibleRows * PANEL_ROW_HEIGHT + PANEL_FOOTER_BLOCK;
   float panelTop = panelHeight * 0.5f;
   highlightPanelTop = panelTop;   // getRowCenterY / overlayHighlightRow read this

   if (box)          setPafWidgetSize(box, PANEL_WIDTH, panelHeight);
   if (highlightBar) setPafWidgetPosition(highlightBar, 0.0f, getRowCenterY(0) + HIGHLIGHT_Y_OFFSET);

   float headerY = panelTop - 34.0f;
   if (label)    setPafWidgetPosition(label,    0.0f, headerY);
   if (subtitle) setPafWidgetPosition(subtitle, 0.0f, headerY - 30.0f);   // a touch lower, clearer gap

   // the update message sits at the vertical center of the row band, where the rows would be.
   if (message) setPafWidgetPosition(message, 0.0f, (panelTop - PANEL_HEADER_BLOCK) - visibleRows * PANEL_ROW_HEIGHT * 0.5f);

   // footer band: bottom hint line 25px above the panel's bottom edge; the mark line (contribute mode,
   // and not while updating) sits one line + 12px above it.
   bool twoLines = (syncMode == SYNC_CONTRIBUTE) && !updating && activeTab == TAB_CHEATS;   // the mark line is a Cheats-tab control
   float bottomY = -panelTop + 25.0f + FOOTER_TEXT_HEIGHT * 0.5f;
   float topY    = twoLines ? bottomY + FOOTER_TEXT_HEIGHT + 12.0f : bottomY;
   if (footer)  setPafWidgetPosition(footer,  0.0f, topY);
   if (footer2) setPafWidgetPosition(footer2, 0.0f, bottomY);
} }

// frame thread: lay out the panel content for the current mode. updating -> hide the rows/highlight
// and show the centered message; normal -> hide the message and show the rows. called on a mode
// change and after a re-parse. the SLATE_100 / alpha values match setOverlayVisible.
namespace { void repaintContent(void)
{
   layoutPanel();   // FIRST: size the box + set highlightPanelTop, so the row layout below uses the current geometry
   footerForRow = -1;   // footer just reset to a base line below; let overlayHighlightRow re-apply the per-row Options hint
   char header[80];
   buildHeaderText(header, gameName, getTabWord());   // "<name> - Cheats" / "- Patches" for the active tab
   if (label)    setPafWidgetText(label, header);
   if (subtitle) setPafWidgetText(subtitle, preppedSubtitle);
   if (updating) {
      layoutAndPaintRows(0);   // hide every row/toggle/tag (alpha 0)
      if (highlightBar) setPafWidgetColor(highlightBar, 0.200f, 0.255f, 0.333f, 0.0f);
      if (footer)  setPafWidgetText(footer, FOOTER_UPDATING);              // only XMB / Resume work now
      if (footer2) setPafWidgetColor(footer2, 0.392f, 0.455f, 0.545f, 0.0f);  // hide the mark line
      if (message) { setPafWidgetText(message, updateMessage ? updateMessage : ""); setPafWidgetColor(message, 0.945f, 0.961f, 0.976f, 1.0f); }
   } else if (activeTab == TAB_PATCHES && patchCount == 0) {
      // no patches for this game: show a centered note where the rows would be, no highlight bar
      layoutAndPaintRows(0);
      if (highlightBar) setPafWidgetColor(highlightBar, 0.200f, 0.255f, 0.333f, 0.0f);
      if (footer)  setPafWidgetText(footer, getFooterTopLine());
      if (footer2) setPafWidgetColor(footer2, 0.392f, 0.455f, 0.545f, 0.0f);
      if (message) { setPafWidgetText(message, "No patches for this game"); setPafWidgetColor(message, 0.945f, 0.961f, 0.976f, 1.0f); }
   } else {
      if (message) setPafWidgetColor(message, 0.945f, 0.961f, 0.976f, 0.0f);   // hide the message
      if (highlightBar) setPafWidgetColor(highlightBar, 0.200f, 0.255f, 0.333f, 1.0f);
      if (footer)  setPafWidgetText(footer, getFooterTopLine());            // restore the full hint line for this tab
      if (footer2) setPafWidgetColor(footer2, 0.392f, 0.455f, 0.545f, activeTab == TAB_CHEATS ? 1.0f : 0.0f);   // mark line is Cheats-only
      highlightIndex = -1;     // force overlayHighlightRow to re-place the bar
      layoutAndPaintRows(1);   // show the rows again (positioned by the layoutPanel above)
   }
} }

extern "C" void overlayShowBox(void)
{
   // called every frame while menuOpen: return silently once shown so we don't
   // spam the log per frame (only the transitions below log).
   if (visible) return;
   if (!arena) return;   // menu can't open without its heap (prepare allocates it first)

   // re-find page_notification every open, on this (the frame) thread. vsh tears
   // down and rebuilds it — and everything parented to it — across a game
   // change, so a widget built under a previous game is stale. reusing one
   // hard-locked the console (killColorTimer walked a freed widget). re-finding
   // lets us tell "same parent, just re-show" from "parent changed, rebuild".
   unsigned int parent = overlayFindPageNotification();
   if (!parent) {
      if (!findFailLogged) { overlayLog("overlay: page_notification find failed - retrying"); findFailLogged = 1; }
      return;   // retry next frame; a later find can still succeed
   }
   findFailLogged = 0;

   // built under this same parent: hide was alpha-0, so re-show is just colours.
   // reopen at the top of the list (the menu thread resets the selection to 0).
   if (box && parent == builtUnder) {
      scrollOffset = 0;
      highlightIndex = -1;
      layoutPanel();   // the cheat count may have changed since last open (e.g. after an Update)
      setOverlayVisible(1);
      overlayLog("overlay: re-shown");
      return;
   }

   // first open, or the game switched (parent rebuilt): (re)construct fresh under
   // the current parent. the old widgets died with their parent — drop the stale
   // pointers WITHOUT touching them (that is the freeze), and build anew.
   if (box) overlayLogHex("show: parent changed - rebuilding, was", builtUnder);
   forgetOverlayWidgets();

   // the cheat list is already parsed (overlayPrepareForTitle, on the menu thread), so the frame
   // thread never touches the file. build every widget at a placeholder position; layoutPanel
   // sizes the box and places the chrome for the current count, layoutAndPaintRows places the rows.

   // planes, back to front (paf draws later children on top): a full-screen dark dimmer, then the
   // panel box on top of it, then the selection highlight bar (before the row text so text is on top).
   dimmer = makePlaneWidget(arena->dimmerStorage, 0.0f, 0.0f, 1920.0f, 1080.0f);
   box    = makePlaneWidget(arena->boxStorage,    0.0f, 0.0f, PANEL_WIDTH, PANEL_ROW_HEIGHT);
   highlightBar = makePlaneWidget(arena->highlightBarStorage, 0.0f, 0.0f, PANEL_WIDTH - 2.0f * HIGHLIGHT_INSET, HIGHLIGHT_HEIGHT);
   highlightIndex = 0;

   // header ("<name> - Cheats") + a smaller dim subtitle (title id + version), text parented to
   // page_notification like the reference, never to a custom plane.
   label    = makeTextWidget(arena->labelStorage,    preppedHeader,   0.0f, 0.0f, HEADER_TEXT_HEIGHT,   PAF_ALIGN_CENTER);
   subtitle = makeTextWidget(arena->subtitleStorage, preppedSubtitle, 0.0f, 0.0f, SUBTITLE_TEXT_HEIGHT, PAF_ALIGN_CENTER);

   // build ALL PANEL_MAX_ROWS slots (not just the current window): a live Update can grow the list,
   // and the extra slots must already exist to show the new rows. unused slots paint at alpha 0.
   float rowX = -PANEL_WIDTH * 0.5f + ROW_LEFT_MARGIN;
   for (int s = 0; s < PANEL_MAX_ROWS; s++) {
      rowSlot[s]     = makeTextWidget(arena->rowStorage[s], "", rowX, 0.0f, ROW_TEXT_HEIGHT, PAF_ALIGN_LEFT);
      toggleSlot[s]  = makeTextWidget(arena->toggleStorage[s], TOGGLE_LABEL_OFF, 0.0f, 0.0f, TOGGLE_TEXT_HEIGHT, PAF_ALIGN_CENTER);
      aobSlot[s]     = makeTextWidget(arena->aobStorage[s], "AoB", 0.0f, 0.0f, TAG_TEXT_HEIGHT, PAF_ALIGN_CENTER);
      scoreSlot[s]   = makeTextWidget(arena->scoreStorage[s], "", 0.0f, 0.0f, TAG_TEXT_HEIGHT, PAF_ALIGN_CENTER);
   }

   // centered button-hint footer. the top line (toggle / xmb / resume, plus update when online) is
   // always built; the mark line (mark working / failed) only in contribute mode. repaintContent
   // swaps the top line to a shorter one and hides the mark line while an update is in flight.
   footer = makeTextWidget(arena->footerStorage, getFooterTopLine(), 0.0f, 0.0f, FOOTER_TEXT_HEIGHT, PAF_ALIGN_CENTER);
   if (syncMode == SYNC_CONTRIBUTE)
      footer2 = makeTextWidget(arena->footer2Storage, GLYPH_SELECT " Mark Failed    " GLYPH_START " Mark Working", 0.0f, 0.0f, FOOTER_TEXT_HEIGHT, PAF_ALIGN_CENTER);

   // centered update message, hidden until an update is in flight (repaintContent shows it).
   message = makeTextWidget(arena->messageStorage, "", 0.0f, 0.0f, HEADER_TEXT_HEIGHT, PAF_ALIGN_CENTER);

   layoutPanel();   // size the box + place the chrome for the current count
   builtUnder = parent;   // remember the parent so a later open can just re-show
   setOverlayVisible(1);
   overlayLog("overlay: shown");
}

// MENU thread: read the running title and parse its cheat file, off the frame
// thread so the show is instant and the file I/O never stalls the compositor.
// returns the renderable cheat count so the caller opens the menu only when there
// is something to show. re-parses only when the title changed: a same-title re-open
// keeps the parsed pools and their resolved aob addresses (so a live cheat can
// still be reverted); getGameHeader (a paf lookup) is the one call here that
// touches firmware from this thread.
// build the header + "<titleId> • v<version>" subtitle text and parse the title's cheat file
// into the pools. shared by the open path (new title) and the live Update refresh (same title,
// freshly downloaded file). the caller has already read the header via getGameHeader.
namespace { void loadTitle(const char *titleId, const char *name)
{
   strCopy(gameName, sizeof(gameName), name);   // kept so repaintContent can rebuild the header per tab
   buildHeaderText(preppedHeader, name, "Cheats");

   char version[16];
   getAppVersion(titleId, version, sizeof(version));   // empty if unavailable

   int end = 0;
   appendStr(preppedSubtitle, sizeof(preppedSubtitle), &end, titleId);
   if (version[0]) {
      appendStr(preppedSubtitle, sizeof(preppedSubtitle), &end, "  \xE2\x80\xA2  v");   // U+2022 bullet
      appendStr(preppedSubtitle, sizeof(preppedSubtitle), &end, version);
   }
   preppedSubtitle[end] = '\0';

   parseCheatsForTitle(titleId, version);   // rebuilds the pools; version selects the matching score line
   strCopy(lastParsedTitle, sizeof(lastParsedTitle), titleId);
} }

static int applyCheat(int cheat, int enable);   // defined below; used by the deferred-reload path

extern "C" int overlayPrepareForTitle(void)
{
   if (!overlayEnsureArena()) return 0;   // no heap -> report no cheats, so the menu stays closed

   loadSyncMode();   // re-read each open so fetch/vote actions honour the current mode (footer text refreshes on the next game load)

   // an Update finished while the menu was closed: the new file is on disk but the OLD pools are
   // still loaded (and their cheats may be applied). revert them here — worker is idle, menu closed,
   // so this is safe and synchronous — then force the re-parse below. cheats come back OFF.
   if (pendingReload) {
      pendingReload = 0;
      for (int c = 0; c < cheatCount; c++) if (rowApplied[c]) applyCheat(c, 0);
      resetAppliedState();
      lastParsedTitle[0] = 0;
   }

   char titleId[16], name[64];
   getGameHeader(titleId, name);
   if (strCmpICase(titleId, lastParsedTitle) != 0) loadTitle(titleId, name);   // new title: (re)parse

   // list the title's texture patches every open (a deploy may have added one since last time), then
   // open on the last-used tab — but fall back if that tab has nothing to show for this title.
   loadPatchList(titleId);
   if (activeTab == TAB_PATCHES && patchCount == 0) activeTab = TAB_CHEATS;
   if (activeTab == TAB_CHEATS && cheatCount == 0 && patchCount > 0) activeTab = TAB_PATCHES;
   return cheatCount + patchCount;   // open the menu if the title has cheats OR patches
}

// MENU thread: the toast to show when the current title has no local cheats to open, or
// NULL to show nothing. non-games stay silent (they never have cheats). offline says so;
// fetch/contribute stay silent here because rung 1b fetches at game launch instead.
extern "C" const char *overlayNoCheatsMessage(void)
{
   if (!isGameTitleId(lastParsedTitle)) return 0;
   if (syncMode == SYNC_OFFLINE) return "No cheats or patches found!  ";
   return 0;
}

// MENU thread: the running game's title id (empty string if none / not ready yet). used by
// the launch-time fetch, which needs the id before any menu has been prepared.
extern "C" void overlayGetTitleId(char *out, int cap)
{
   char name[64];
   if (cap > 0) out[0] = '\0';
   getGameHeader(out, name);   // writes the id into out (<= 11 chars); name is discarded
}

// FRAME thread: hide the overlay. normally a graceful alpha-0 recolour (widgets kept for
// a fast re-show). but if the game exited, vsh tore down page_notification and our widgets
// with it, so re-colouring them would walk freed memory (the killColorTimer hard-lock):
// in that case the menu thread has armed forgetOnHide, and we just drop the pointers.
extern "C" void overlayHideBox(void)
{
   int forget = forgetOnHide;
   forgetOnHide = 0;   // consume the arm regardless of visibility, so a later normal close stays graceful
   if (forget) {
      // the game exited (only overlayOnGameExit arms this): vsh freed page_notification and our
      // widgets with it, so drop the pointers WITHOUT touching them (touching = the killColorTimer
      // freeze). do this even when the menu was already closed (visible==0) — otherwise the stale
      // pointers survive into the next game and a re-show under a recycled parent walks freed memory.
      forgetOverlayWidgets();
      visible = 0;
      jobsActive = 0;
      overlayLog("overlay: hidden (game gone - forgot widgets)");
      return;
   }
   if (!visible) return;
   setOverlayVisible(0);
   overlayLog("overlay: hidden");
}

// MENU thread, on game exit: (1) the applied-cheat state was resolved inside the now-dead
// process, so its match addresses are stale and would poke a wrong spot in the NEXT game —
// clear it so the next open reads every row OFF (a re-enable re-scans the new game fresh);
// (2) arm the frame thread's next hide to forget-without-touch, since vsh is freeing our
// widgets. keyed on the inGame gap — not a pid or widget address, which lv2 can recycle
// across a same-title relaunch. the caller publishes this before clearing menuOpen so the
// hide frame observes the arm.
extern "C" void overlayOnGameExit(void)
{
   resetAppliedState();
   drillPatch = -1;
   for (int i = 0; i < MAX_PATCHES; i++) {   // a relaunched game reloads originals: nothing applied
      patchApplied[i] = 0;
      for (int p = 0; p < MAX_PATCH_PARTS; p++) patchPartOn[i][p] = 0;
   }
   clearAppliedState(lastParsedTitle);   // the game's texture snapshots died with the process
   forgetOnHide = 1;
}

// move the selection to a cheat and keep it in view. index is the absolute cheat
// index; when it leaves the window we scroll (slide every slot) so it comes back
// into view, then park the highlight bar on its on-screen slot. runs on the frame
// thread (called from the paf hook), so touching widgets here is safe. no-op when
// hidden, no rows, or nothing moved (keeps it off the per-frame path once settled).
extern "C" void overlayHighlightRow(int index)
{
   int rowCount = getRowCount();
   if (!visible || !highlightBar || rowCount <= 0) return;
   if (index < 0) index = 0;
   if (index >= rowCount) index = rowCount - 1;

   // patch list: show the Options hint only while the highlighted patch has options. only on a real row
   // change (not every frame — re-setting text per frame risks wedging the RSX).
   if (activeTab == TAB_PATCHES && drillPatch < 0 && !updating && footer && index != footerForRow) {
      setPafWidgetText(footer, patchHasParts[index] ? FOOTER_PATCH_OPTIONS : FOOTER_PATCH_LIST);
      footerForRow = index;
   }

   // scroll the window so the selection is inside it
   int visibleRows = getWindowRows();
   int newOffset = scrollOffset;
   if (index < newOffset)                      newOffset = index;
   else if (index >= newOffset + visibleRows)  newOffset = index - visibleRows + 1;
   if (newOffset != scrollOffset) {
      scrollOffset = newOffset;
      layoutAndPaintRows(1);   // refill every slot for the new window
   }

   int slot = index - scrollOffset;
   if (slot == highlightIndex) return;   // edge-scroll keeps the bar put while the list moves under it
   highlightIndex = slot;
   setPafWidgetPosition(highlightBar, 0.0f, getRowCenterY(slot) + HIGHLIGHT_Y_OFFSET);
}

extern "C" int overlayGetRowCount(void)
{
   return getRowCount();
}

// MENU thread: a row's op-hash — its online identity, used to build the vote path. 0 if out of range.
// takes a display row (the menu's selection) and maps it to the real cheat.
extern "C" unsigned int overlayGetCheatHash(int row)
{
   if (!arena || row < 0 || row >= cheatCount) return 0;
   return cheatOpHash[getRowCheat(row)];
}

// MENU thread: is the row's cheat live in the game right now (so a WORKED vote has real evidence + a
// snapshot to send)? Mark Working is gated on this.
extern "C" int overlayIsCheatApplied(int row)
{
   return arena && row >= 0 && row < cheatCount && rowApplied[getRowCheat(row)];
}

// MENU thread: build the CHEAT_WORKED working-val body for a cheat — one "<opIdx>=<orig>" line per
// w32 op, where orig is the 8-hex pre-write snapshot (uppercase) and opIdx counts all op lines (so it
// matches reconcile). empty unless the cheat is applied (only then is the snapshot valid). returns the
// length written.
extern "C" int overlayBuildVoteBody(int row, char *out, int cap)
{
   out[0] = 0;
   if (!arena || row < 0 || row >= cheatCount) return 0;
   int cheat = getRowCheat(row);
   if (!rowApplied[cheat]) return 0;
   static const char hexUpper[] = "0123456789ABCDEF";
   Cheat *c = &arena->cheats[cheat];
   int end = 0;
   for (int k = 0; k < c->opCount; k++) {
      CheatOp *op = &arena->opPool[c->firstOp + k];
      if (op->kind != CHEAT_OP_WRITE || op->width != 4) continue;   // only w32 carries working-val
      char line[16];
      int n = 0;
      if (k >= 100) line[n++] = (char)('0' + k / 100);
      if (k >= 10)  line[n++] = (char)('0' + (k / 10) % 10);
      line[n++] = (char)('0' + k % 10);
      line[n++] = '=';
      for (int b = 0; b < 4; b++) {
         unsigned char byte = arena->snapBlob[op->snapOffset + b];
         line[n++] = hexUpper[byte >> 4];
         line[n++] = hexUpper[byte & 0xF];
      }
      line[n++] = '\n';
      line[n] = 0;
      appendStr(out, cap, &end, line);
   }
   out[end] = 0;
   return end;
}

// match the pattern across `len` bytes already read into buf from game address baseAddr,
// appending each hit's game address to matchPool. returns the count, or -1 if the pool
// filled mid-way (caller stops).
static int scanBufferForMatches(const unsigned char *buffer, int len, const unsigned char *pattern, int patternLen, unsigned int baseAddr)
{
   int found = 0;
   for (int off = 0; off + patternLen <= len; off++) {
      int matched = 0;
      while (matched < patternLen && buffer[off + matched] == pattern[matched]) matched++;
      if (matched == patternLen) {
         if (matchPoolUsed >= MATCH_POOL_SIZE) { overlayLog("scan: match pool full - extra matches dropped"); return -1; }
         arena->matchPool[matchPoolUsed++] = baseAddr + off;
         found++;
      }
   }
   return found;
}

// scan one [start, end) range for the pattern, appending EVERY match address to matchPool
// (write-all). returns the number appended, or SCAN_ABORTED if the user cancelled mid-scan.
// reads in SCAN_CHUNK bulk blocks with a pattern-1 overlap so a straddling match is caught.
// a cobra read is all-or-nothing, so a block spanning an unmapped page fails whole — then we
// drop to per-page reads for that block so the mapped pages inside it are still scanned (the
// same page-granular sweep as before, just now only where a hole forces it). stops early if
// the pool fills. cheat/wantOn identify the job so the scan bails the moment it is cancelled.
static int scanRange(unsigned int pid, const unsigned char *pattern, int patternLen, int cheat, int wantOn, unsigned int start, unsigned int end, unsigned char *scanBuffer)
{
   int found = 0, chunk = 0;
   for (unsigned int addr = start; addr < end; ) {
      // stay cancellable: every ~1 MB, bail if this job was cancelled
      if ((chunk++ & 63) == 0 && rowDesiredOn[cheat] != wantOn) { overlayLog("scan: cancelled"); return SCAN_ABORTED; }

      unsigned int want = end - addr < SCAN_CHUNK ? end - addr : SCAN_CHUNK;
      if (readProcMem(pid, addr, scanBuffer, want) == 0) {
         int n = scanBufferForMatches(scanBuffer, (int)want, pattern, patternLen, addr);
         if (n < 0) return found;                       // pool full
         found += n;
         if (want < SCAN_CHUNK) break;                  // last (partial) block done
         unsigned int step = SCAN_CHUNK - (patternLen - 1);   // overlap so a straddling match is not missed
         if (addr + step < addr) break;                 // 32-bit wrap at top of memory: nothing above, stop
         addr += step;
         continue;
      }

      // the block has an unmapped page: sweep it page by page so mapped pages are not lost.
      unsigned int blockEnd = addr + want;
      while (addr < blockEnd) {
         unsigned int pageWant = blockEnd - addr < SCAN_PAGE ? blockEnd - addr : SCAN_PAGE;
         if (readProcMem(pid, addr, scanBuffer, pageWant) != 0) {
            if (addr + SCAN_PAGE < addr) return found;  // wrap: swept to the top, end the scan (a break would re-loop from 0)
            addr += SCAN_PAGE; continue;                // unmapped: skip page
         }
         int n = scanBufferForMatches(scanBuffer, (int)pageWant, pattern, patternLen, addr);
         if (n < 0) return found;
         found += n;
         if (pageWant < SCAN_PAGE) break;
         unsigned int step = SCAN_PAGE - (patternLen - 1);   // overlap within the fallback too
         if (addr + step < addr) return found;          // wrap: end the scan
         addr += step;
      }
   }
   return found;
}

// scan the game for the pattern across its real module segments (Rung 3), appending
// every match to matchPool. falls back to the fixed [SCAN_START, SCAN_END) window when
// the CFW can't enumerate segments. returns the total appended, or SCAN_ABORTED if
// cancelled. runs on the worker thread; never touches paf widgets or the pad.
static int scanGameForMatches(unsigned int pid, const unsigned char *pattern, int patternLen, int cheat, int wantOn)
{
   if (patternLen <= 0) return 0;

   // the bulk scan buffer is a transient page (the arena has no room to grow it): taken for
   // the sweep and freed right after, so nothing extra is resident between scans.
   unsigned char *scanBuffer = (unsigned char *)overlayHeapAlloc(SCAN_CHUNK);
   if (!scanBuffer) { overlayLog("scan: scratch alloc failed"); return 0; }

   // reserve one range slot for the synthesized EBOOT range appended below
   GameRange ranges[MAX_SCAN_RANGES];
   int rangeCount = getGameScanRanges(pid, ranges, MAX_SCAN_RANGES - 1);
   if (rangeCount <= 0) { ranges[0].base = SCAN_START; ranges[0].size = SCAN_END - SCAN_START; rangeCount = 1; }   // fallback window

   // the primary EBOOT image is not in the prx module list (only sprx libs are), so its
   // segment — where a game's own static data lives — is never enumerated. it sits below
   // the lowest module, so cover [SCAN_START, lowestModuleBase) as one extra range.
   else if (rangeCount < MAX_SCAN_RANGES) {
      unsigned int lowestBase = ranges[0].base;
      for (int r = 1; r < rangeCount; r++) if (ranges[r].base < lowestBase) lowestBase = ranges[r].base;
      if (lowestBase > SCAN_START) { ranges[rangeCount].base = SCAN_START; ranges[rangeCount].size = lowestBase - SCAN_START; rangeCount++; }
   }

   int found = 0;
   for (int r = 0; r < rangeCount; r++) {
      int n = scanRange(pid, pattern, patternLen, cheat, wantOn, ranges[r].base, ranges[r].base + ranges[r].size, scanBuffer);
      if (n == SCAN_ABORTED) { found = SCAN_ABORTED; break; }
      found += n;
      if (matchPoolUsed >= MATCH_POOL_SIZE) break;   // pool full: stop scanning further ranges
   }

   overlayHeapFree(scanBuffer);
   // exactly one match is the expected outcome; only anything else is worth a line
   if (found != SCAN_ABORTED && found != 1) overlayLogHex("scan: matches", (unsigned int)found);
   return found;
}

// read-only compatibility check for the score badge: does every aob op of this cheat still find its
// find-pattern on THIS build? returns VERDICT_OK (all present -> the target code exists here, cheat is
// compatible), VERDICT_FAIL (a pattern is missing -> won't apply), or -1 (cancelled / no game). NEVER
// pokes, so it cannot lock the console. borrows the free tail of matchPool and restores it — the caller
// must run this only for a NOT-applied cheat (an applied cheat's find-pattern is overwritten by its
// replace, which would read as a false miss).
static int scanAobVerdict(unsigned int pid, int cheat)
{
   Cheat *c = &arena->cheats[cheat];
   int savedUsed = matchPoolUsed;
   int verdict = VERDICT_OK;
   for (int i = 0; i < c->opCount; i++) {
      if (!jobsActive) { matchPoolUsed = savedUsed; return -1; }   // menu closed / live Update parked us: bail between ops
      CheatOp *op = &arena->opPool[c->firstOp + i];
      if (op->kind != CHEAT_OP_AOB) continue;   // a w32 op has a fixed address — always "present"
      matchPoolUsed = savedUsed;   // reuse the tail per op; we only need found-vs-not, not the matches
      int found = scanGameForMatches(pid, arena->aobBlob + op->findOffset, op->width, cheat, rowDesiredOn[cheat]);
      if (found == SCAN_ABORTED) { matchPoolUsed = savedUsed; return -1; }   // user toggled this cheat: apply takes over
      if (found == 0) { verdict = VERDICT_FAIL; break; }
   }
   matchPoolUsed = savedUsed;   // discard the verdict's matches (not a real ON slice)
   return verdict;
}

// poke bytes into the game and read them back to confirm they stuck. returns 1 if
// the readback matches, 0 if not (write refused, or the address isn't what we think).
// verifies in <=64-byte chunks so any op width (a long aob pattern) is covered.
static int writeProcMemVerified(unsigned int pid, unsigned int addr, const unsigned char *bytes, int width)
{
   writeProcMem(pid, addr, bytes, width);
   unsigned char readback[64];
   for (int off = 0; off < width; ) {
      int n = width - off < 64 ? width - off : 64;
      if (readProcMem(pid, addr + off, readback, n) != 0) return 0;   // readback failed: unverified
      for (int b = 0; b < n; b++)
         if (readback[b] != bytes[off + b]) return 0;
      off += n;
   }
   return 1;
}

// where an op lands in the target. lowest address = deepest in the game's live code, so
// it is the activation point (a branch into a cave); caves sit above it. an aob op's
// address is its first match, a write op's is its fixed address.
static unsigned int getOpPrimaryAddress(const CheatOp *op)
{
   if (op->kind != CHEAT_OP_AOB) return op->address;
   return op->matchCount ? arena->matchPool[op->matchStart] : 0;   // empty slice (e.g. rollback of a no-match op): don't read matchPool
}

// poke an op's ON bytes: an aob writes its replace pattern over every match, a write op
// writes its value. returns 1 only if every poke verified.
static int pokeOpOn(unsigned int pid, const CheatOp *op)
{
   if (op->kind == CHEAT_OP_AOB) {
      const unsigned char *replacePattern = arena->aobBlob + op->replaceOffset;
      for (int m = 0; m < op->matchCount; m++)
         if (!writeProcMemVerified(pid, arena->matchPool[op->matchStart + m], replacePattern, op->width)) return 0;
      return 1;
   }
   unsigned char writeBytes[4];
   for (int b = 0; b < op->width; b++) writeBytes[b] = (unsigned char)(op->value >> (8 * (op->width - 1 - b)));   // big-endian
   return writeProcMemVerified(pid, op->address, writeBytes, op->width);
}

// poke an op's original (OFF) bytes: an aob writes its find pattern back over every match
// (a match holds exactly the find pattern), a write op writes its pre-poke snapshot.
static int pokeOpOff(unsigned int pid, const CheatOp *op)
{
   int allVerified = 1;
   if (op->kind == CHEAT_OP_AOB) {
      const unsigned char *findPattern = arena->aobBlob + op->findOffset;
      for (int m = 0; m < op->matchCount; m++)
         if (!writeProcMemVerified(pid, arena->matchPool[op->matchStart + m], findPattern, op->width)) allVerified = 0;
      return allVerified;
   }
   return writeProcMemVerified(pid, op->address, arena->snapBlob + op->snapOffset, op->width);
}

// order a cheat's ops into pokeOrder by primary address. ascending for revert (kill the
// live-code hook first, then tear the cave down), descending for enable (build the cave
// first, land the activating branch last). insertion sort — opCount is small (<=255).
static void orderOpsByAddress(int cheat, unsigned short *pokeOrder, int ascending)
{
   Cheat *c = &arena->cheats[cheat];
   for (int i = 0; i < c->opCount; i++) {
      unsigned short opIndex = (unsigned short)(c->firstOp + i);
      unsigned int key = getOpPrimaryAddress(&arena->opPool[opIndex]);
      int j = i;
      while (j > 0) {
         unsigned int prev = getOpPrimaryAddress(&arena->opPool[pokeOrder[j - 1]]);
         if (ascending ? prev <= key : prev >= key) break;
         pokeOrder[j] = pokeOrder[j - 1];
         j--;
      }
      pokeOrder[j] = opIndex;
   }
}

// revert a cheat to OFF: write every op's original bytes back, hook first (ascending
// address) so the branch into the cave dies before the cave is torn down — the mirror of
// enable's ordering, so a revert during live gameplay is safe too. returns 1 if every
// write-back verified (caller then frees the match slice), 0 if any failed (the cheat may
// be partly live; caller keeps it ON).
static int revertCheat(unsigned int pid, int cheat)
{
   unsigned short pokeOrder[256];
   orderOpsByAddress(cheat, pokeOrder, 1);
   Cheat *c = &arena->cheats[cheat];
   int allVerified = 1;
   for (int i = 0; i < c->opCount; i++)
      if (!pokeOpOff(pid, &arena->opPool[pokeOrder[i]])) allVerified = 0;
   return allVerified;
}

// reclaim a cheat's slice of matchPool when it goes OFF: clear its ops' match refs, then
// compact the pool (shift the tail down over the freed slice) and fix up every other
// cheat/op whose matches sat after it. keeps the pool from leaking across repeated toggles.
static void freeCheatMatches(int cheat)
{
   Cheat *c = &arena->cheats[cheat];
   int start = c->matchStart, n = c->matchCount;
   for (int k = 0; k < c->opCount; k++) { CheatOp *op = &arena->opPool[c->firstOp + k]; op->matchStart = 0; op->matchCount = 0; }
   c->matchStart = 0;
   c->matchCount = 0;
   if (n <= 0) return;

   for (int i = start; i + n < matchPoolUsed; i++) arena->matchPool[i] = arena->matchPool[i + n];
   matchPoolUsed -= n;

   for (int other = 0; other < cheatCount; other++) {
      Cheat *oc = &arena->cheats[other];
      for (int k = 0; k < oc->opCount; k++) {
         CheatOp *op = &arena->opPool[oc->firstOp + k];
         if (op->matchCount > 0 && op->matchStart >= start + n) op->matchStart = (unsigned short)(op->matchStart - n);
      }
      if (oc->matchCount > 0 && oc->matchStart >= start + n) oc->matchStart = (unsigned short)(oc->matchStart - n);
   }
}

// roll a failed/cancelled enable back to OFF: revert every op (writing an original over an
// un-poked op is a harmless no-op), then free the cheat's match slice.
static void rollbackEnable(unsigned int pid, int cheat)
{
   revertCheat(pid, cheat);
   freeCheatMatches(cheat);
}

// apply (enable) or revert (disable) a cheat's ops against the running game. enable runs in
// three phases: scan every aob for its matches, snapshot every write op's original bytes
// (memory still pristine, so revert restores the true original), then poke every op in
// descending address order so the cave is built before the activating branch — the lowest
// address, in the game's live code — lands last. that keeps a toggle safe even while the
// patched routine is executing. any op that fails to verify rolls the cheat back.
//   enable  -> ops applied (>0 = ON; 0 = nothing landed, revert to OFF; -1 = cancelled)
//   disable -> 1 if the revert verified (now OFF), 0 if it didn't (stays ON)
static int applyCheat(int cheat, int enable)
{
   unsigned int pid = vshmain_0624D3AE();
   if (!pid) { overlayLog("apply: no game running"); return 0; }
   Cheat *c = &arena->cheats[cheat];

   if (!enable) {
      int reverted = revertCheat(pid, cheat);
      overlayLogHex("apply: cheat revert verified", (unsigned int)reverted);
      if (reverted) freeCheatMatches(cheat);   // now OFF: reclaim its match slice
      return reverted;
   }

   // phase 1: scan every aob op for its matches (no poke yet). a missing or cancelled scan
   // fails the whole cheat before anything is written.
   c->matchStart = (unsigned short)matchPoolUsed;   // this cheat's aob matches begin here
   c->matchCount = 0;
   for (int k = 0; k < c->opCount; k++) {
      CheatOp *op = &arena->opPool[c->firstOp + k];
      if (op->kind != CHEAT_OP_AOB) continue;
      op->matchStart = (unsigned short)matchPoolUsed;
      int count = scanGameForMatches(pid, arena->aobBlob + op->findOffset, op->width, cheat, enable);
      op->matchCount = (unsigned short)(count > 0 ? count : 0);
      c->matchCount = (unsigned short)(matchPoolUsed - c->matchStart);
      if (count == SCAN_ABORTED) { rollbackEnable(pid, cheat); return -1; }   // cancelled
      // no match fails the whole cheat (not a silent skip) — else a sibling write op would
      // mask a broken/incorrect pattern and still show ON.
      if (count == 0) { overlayLog("apply: aob pattern not found - cheat fails"); rollbackEnable(pid, cheat); return 0; }
   }

   // phase 2: snapshot the original bytes of every write op while memory is pristine.
   // known limitation: snapshots are per-cheat, so two ON cheats writing the SAME address
   // revert to the wrong value if disabled out of enable-order (each restores its own
   // snapshot, not a shared original). no cheat set we ship overlaps, so this is left as a
   // documented edge rather than paid for with cross-cheat address tracking.
   for (int k = 0; k < c->opCount; k++) {
      CheatOp *op = &arena->opPool[c->firstOp + k];
      if (op->kind != CHEAT_OP_AOB) readProcMem(pid, op->address, arena->snapBlob + op->snapOffset, op->width);
   }

   // phase 3: poke every op high address to low, so the activating branch lands last.
   unsigned short pokeOrder[256];
   orderOpsByAddress(cheat, pokeOrder, 0);
   for (int i = 0; i < c->opCount; i++) {
      CheatOp *op = &arena->opPool[pokeOrder[i]];
      if (!pokeOpOn(pid, op)) {
         overlayLogHex("apply: poke did not verify at", getOpPrimaryAddress(op));
         rollbackEnable(pid, cheat); return 0;
      }
   }

   overlayLogHex("apply: cheat enabled, ops", (unsigned int)c->opCount);
   return c->opCount;   // >0 = ON; a cheat with 0 ops returns 0 (can't turn ON)
}

// MENU thread: flip a cheat's desired state. never blocks — the worker does the
// scan/poke. toggling a pending cheat flips the desire back, which cancels the
// in-flight job (the scan sees the change and bails).
extern "C" void overlayRequestToggle(int row)
{
   if (!visible || row < 0 || row >= cheatCount) return;
   int cheat = getRowCheat(row);
   if (rowFailed[cheat]) {
      // a failed row: retry the SAME operation by un-settling toward the preserved desire (a failed
      // revert stays desired=off, so this retries revert — it never re-applies). clears on the retry.
      rowFailed[cheat] = 0;
      rowServiced[cheat] = rowDesiredOn[cheat] ? 0 : 1;
   } else {
      rowDesiredOn[cheat] = rowDesiredOn[cheat] ? 0 : 1;
   }
   togglePaintDirty = 1;   // show PENDING immediately
}

// MENU thread: enter update mode with a centered message (a string literal, safe to hold), or leave
// it with msg == NULL. the frame thread switches the panel content (rows <-> message) on its next tick.
extern "C" int  overlayIsUpdating(void)             { return updating; }
extern "C" void overlaySetUpdating(const char *msg) { updateMessage = msg; updating = msg != 0; __sync_synchronize(); contentDirty = 1; }

// MENU thread: which tab is showing (OVERLAY_TAB_*), so the input handler routes Cross/Square correctly.
extern "C" int overlayGetTab(void) { return activeTab; }

// MENU thread: switch tabs (L1/R1). entering Patches re-lists the title's patch folders first so the
// frame thread has the names before it repaints; the caller resets its selection to 0 after. the frame
// thread picks up the change via contentDirty (same path the update-message mode uses). returns the new
// row count.
extern "C" int overlaySwitchTab(int tab)
{
   activeTab = (tab == TAB_PATCHES) ? TAB_PATCHES : TAB_CHEATS;
   drillPatch = -1;   // switching tabs drops any drilled-in options view
   if (activeTab == TAB_PATCHES) loadPatchList(lastParsedTitle);
   __sync_synchronize();
   contentDirty = 1;
   return getRowCount();
}

// MENU thread: toggle the selected patch (✕ on the Patches tab) — apply it if off, revert it if on.
// blocks this thread for a moment while it scans the game's textures; the frame thread keeps compositing,
// so a progress message set by the caller still paints. returns 1 = applied, 2 = reverted, 0 = nothing
// matched (the patch's originals aren't on screen). the new ON/OFF state lands in patchApplied.
extern "C" int overlayToggleSelectedPatch(int row)
{
   if (activeTab != TAB_PATCHES || row < 0 || row >= patchCount) return 0;
   unsigned int pid = vshmain_0624D3AE();
   if (patchApplied[row]) {
      revertPatch(pid, lastParsedTitle, patchNames[row]);
      patchApplied[row] = 0;
      return 2;
   }
   int replaced = applyPatch(pid, lastParsedTitle, patchNames[row]);
   patchApplied[row] = (char)(replaced > 0);
   return replaced > 0 ? 1 : 0;
}

// MENU thread: does the selected patch expose parts (so ✕/△ drill into its options instead of applying whole)?
extern "C" int overlayPatchHasParts(int row)
{
   return activeTab == TAB_PATCHES && row >= 0 && row < patchCount && patchHasParts[row];
}

// MENU thread: drill into the selected patch's parts. returns the part count, 0 if it has none (the caller
// applies the whole patch instead). the frame thread re-lays the panel via contentDirty.
extern "C" int overlayEnterPatchOptions(int row)
{
   if (activeTab != TAB_PATCHES || drillPatch >= 0 || row < 0 || row >= patchCount) return 0;
   drillPartCount = getPatchParts(lastParsedTitle, patchNames[row], drillParts, MAX_PATCH_PARTS);
   if (drillPartCount <= 0) return 0;
   drillPatch = row;
   __sync_synchronize();
   contentDirty = 1;
   return drillPartCount;
}

// MENU thread: leave the parts list, back to the patch list.
extern "C" void overlayExitPatchOptions(void)
{
   drillPatch = -1;
   __sync_synchronize();
   contentDirty = 1;
}

extern "C" int overlayInPatchOptions(void) { return drillPatch >= 0; }

// MENU thread: toggle the selected part. a pick-one variant turns its group siblings off first (radio),
// then the whole patch is rebuilt to last-wins. returns 1 = turned on, 2 = turned off, 0 = no game.
extern "C" int overlayToggleSelectedPart(int row)
{
   if (drillPatch < 0 || row < 0 || row >= drillPartCount) return 0;
   unsigned int pid = vshmain_0624D3AE();
   if (!pid) return 0;

   unsigned char *partOn = patchPartOn[drillPatch];
   int turningOn = !partOn[row];
   if (turningOn && drillParts[row].pickOne)   // radio: only one variant of this group on at a time
      for (int i = 0; i < drillPartCount; i++)
         if (drillParts[i].group == drillParts[row].group) partOn[i] = 0;
   partOn[row] = (unsigned char)turningOn;

   rebuildPatch(pid, lastParsedTitle, patchNames[drillPatch], partOn);

   int anyOn = 0;   // the patch-list row shows ON while any part is on
   for (int i = 0; i < drillPartCount; i++) if (partOn[i]) anyOn = 1;
   patchApplied[drillPatch] = (char)anyOn;

   __sync_synchronize();
   contentDirty = 1;   // repaint the parts toggles
   return turningOn ? 1 : 2;
}

// MENU thread: the menu is visible right now (used to choose in-menu message vs toast).
extern "C" int  overlayIsMenuVisible(void)          { return visible; }

// MENU thread: an update landed while the menu was closed — reload the new file on the next open.
extern "C" void overlayScheduleReload(void)         { pendingReload = 1; }

// FRAME thread, each frame while the menu is open: apply a pending content-mode switch (update
// start/end, or the post-parse repaint after a live Update). re-finds page_notification and bails
// if it changed/vanished — a game exit frees our widgets, and repainting them then hard-locks
// (the killColorTimer class); every other widget-touch guards this way, so this one must too.
extern "C" void overlayFlushContent(void)
{
   if (!contentDirty) return;
   contentDirty = 0;
   if (!box || overlayFindPageNotification() != builtUnder || !builtUnder) return;   // widgets stale/freed
   repaintContent();
}

// FRAME thread, top of the paf hook: park all overlay work while a live refresh re-parses the
// pools (returns 1 = caller skips this frame). the post-parse repaint itself rides contentDirty
// through overlayFlushContent (which guards against freed widgets), so this is a pure predicate.
extern "C" int overlayFrameFrozen(void)
{
   if (refreshFreeze) { refreshFrozen = 1; return 1; }   // parked: menu thread is re-parsing
   refreshFrozen = 0;
   return 0;
}

// MENU thread, after a successful Update download: revert the currently-applied cheats, re-read the
// updated file, and re-enable the ones still present — all in place, menu open. the cheats stayed
// ON during the download; they only blink here (revert → re-parse → re-apply), because the new file
// may have moved their addresses.
extern "C" void overlayRefreshFromFile(void)
{
   // remember which cheats are ON (by name hash, to survive the re-parse) so we can re-enable them.
   savedOnCount = 0;
   for (int c = 0; c < cheatCount; c++)
      if (rowDesiredOn[c]) savedOnHashes[savedOnCount++] = hashCheatName(getCheatName(c));

   // park BOTH pool readers so the menu thread is their sole owner during the rewrite: the FRAME
   // thread (refreshFreeze -> refreshFrozen ack) and the WORKER (jobsActive=0, then wait until it has
   // left applyCheat — workerBusy==0). rowApplied alone can't prove the worker is idle: it stays 0 for
   // the whole duration of an enable, so an in-flight enable would otherwise race the parse.
   refreshFreeze = 1;
   jobsActive = 0;
   __sync_synchronize();
   for (int attempt = 0; attempt < 200 && workerBusy;   attempt++) sys_timer_usleep(5000);   // worker leaves its job (<=1s)

   // the worker did NOT stop in time — it is still inside applyCheat holding pointers into the very pools
   // we are about to rewrite, so a rewrite now would race it into wrong pokes. never proceed: leave the
   // cheats running on the OLD file and pick the new one up on the next open, where the worker is provably
   // idle. (with scanRange's cancel this should be unreachable; it's the safety net if a scan outlasts 1s.)
   if (workerBusy) {
      overlayLog("refresh: worker did not park, deferring reload to next open");
      pendingReload = 1;
      updating = 0;          // leave update mode so the (unchanged) rows come back
      refreshFreeze = 0;
      jobsActive = 1;
      __sync_synchronize();
      contentDirty = 1;
      return;
   }

   for (int attempt = 0; attempt < 60 && !refreshFrozen; attempt++) sys_timer_usleep(2000);  // frame thread parks (<=120ms)

   // sole owner now: revert every applied cheat synchronously against the OLD pools (worker parked, so
   // no matchPool race), then clear the applied state so no stale index survives into the new pools.
   for (int c = 0; c < cheatCount; c++) if (rowApplied[c]) applyCheat(c, 0);
   resetAppliedState();

   char titleId[16], name[64];
   getGameHeader(titleId, name);
   loadTitle(titleId, name);
   if (scrollOffset >= cheatCount) scrollOffset = 0;   // list may have shrunk

   // re-enable the cheats that were ON before the update: match the new file's cheats against the
   // saved name hashes and flip their desire ON. the worker re-applies each fresh (new scan/poke),
   // so the selection survives even if addresses or ordering changed. dropped/renamed cheats fall away.
   for (int c = 0; c < cheatCount; c++) {
      unsigned int hash = hashCheatName(getCheatName(c));
      for (int i = 0; i < savedOnCount; i++)
         if (savedOnHashes[i] == hash) { rowDesiredOn[c] = 1; break; }
   }
   savedOnCount = 0;
   updating = 0;   // leaving update mode: the repaint below shows the rows again

   __sync_synchronize();
   contentDirty = 1;   // frame thread repaints (rows + new title) on its next tick, via overlayFlushContent
   refreshFreeze = 0;
   jobsActive = 1;       // menu still open: the worker re-applies the re-enabled cheats
}

// MENU thread, on menu exit: cancel every in-flight job by reverting its desire to
// whatever is already applied. settled ON cheats are left applied (they keep working
// during play); only pending transitions are undone.
extern "C" void overlayCancelAllPending(void)
{
   for (int c = 0; c < cheatCount; c++)
      if (getToggleDisplay(c) == TOGGLE_PENDING) rowDesiredOn[c] = rowApplied[c];
   togglePaintDirty = 1;
}

// WORKER thread: reconcile one cheat toward its desired state (scan/poke/verify or
// revert), then return so cancels/exit are seen promptly between jobs. no-op when
// the menu is closed. returns 1 if it did work (so the worker can pace itself).
extern "C" int overlayServiceJobs(void)
{
   if (!jobsActive) return 0;

   for (int c = 0; c < cheatCount; c++) {
      int want = rowDesiredOn[c];
      if (want == rowServiced[c]) continue;   // already handled this desire

      // claim "busy" and re-check jobsActive: a live Update parks us (jobsActive=0) then waits for
      // workerBusy to clear before it rewrites the pools. this handshake proves we are NOT inside
      // applyCheat (reading/poking the pools) when the menu thread re-parses them.
      workerBusy = 1;
      __sync_synchronize();
      if (!jobsActive) { workerBusy = 0; return 0; }

      togglePaintDirty = 1;
      // on failure keep rowDesiredOn (the user's intent) and settle serviced to it, so the row shows
      // FAIL and a retry reconciles toward the SAME intent — a failed revert never turns into an apply.
      if (want) {
         int applied = applyCheat(c, 1);       // scan + poke; <0 = cancelled mid-scan
         if      (applied > 0)  { rowApplied[c] = 1; rowServiced[c] = 1; rowFailed[c] = 0; }   // applied -> ON
         else if (applied < 0)  { rowApplied[c] = 0; }                                         // cancelled: leave desire, re-reconcile
         else                   { rowApplied[c] = 0; rowServiced[c] = 1; rowFailed[c] = 1; }   // enable failed -> FAIL (writes reverted)
      } else {
         if (applyCheat(c, 0)) { rowApplied[c] = 0; rowServiced[c] = 0; rowFailed[c] = 0; }    // reverted -> OFF
         else                  { rowApplied[c] = 1; rowServiced[c] = 0; rowFailed[c] = 1; }    // revert failed -> FAIL (still live)
      }
      // a freshly-applied aob cheat proved its patterns exist here -> compatible (green badge). only when
      // UNKNOWN, so a w32 working-val verdict is not overwritten.
      if (rowApplied[c] && cheatHasAob(c) && cheatVerdict[c] == VERDICT_UNKNOWN) { cheatVerdict[c] = VERDICT_OK; contentDirty = 1; }
      togglePaintDirty = 1;
      workerBusy = 0;
      return 1;   // one cheat per call
   }

   // no apply job pending: resolve one aob cheat's compatibility verdict for the score badge. read-only
   // (scanAobVerdict never pokes -> can't lock the console), one cheat per call so badges fill in green/
   // red over a few seconds without blocking. skips applied cheats (already known compatible; their
   // find-pattern is overwritten) and cheats whose verdict is already known.
   for (int c = 0; c < cheatCount; c++) {
      if (cheatVerdict[c] != VERDICT_UNKNOWN || !cheatHasAob(c) || rowApplied[c]) continue;
      unsigned int pid = vshmain_0624D3AE();
      if (!pid) return 0;   // no game to read: idle rather than spin (a relaunch re-parses anyway)
      workerBusy = 1;
      __sync_synchronize();
      if (!jobsActive) { workerBusy = 0; return 0; }
      int verdict = scanAobVerdict(pid, c);
      workerBusy = 0;
      if (verdict < 0) return 1;   // cancelled (toggled mid-scan): retry on a later call
      cheatVerdict[c] = (unsigned char)verdict;
      __sync_synchronize();
      contentDirty = 1;   // frame thread repaints the badge (overlayFlushContent, guarded)
      return 1;
   }
   return 0;
}

// FRAME thread: repaint the visible slots' ON/OFF widgets from their cheats' state.
// call every frame from the paf hook; only does work after a state changed.
extern "C" void overlayFlushTogglePaint(void)
{
   if (!togglePaintDirty) return;
   togglePaintDirty = 0;
   for (int s = 0; s < PANEL_MAX_ROWS; s++) {   // all slots: hide toggles past a shrunk count too
      int row = scrollOffset + s;
      int onScreen = activeTab == TAB_CHEATS && row >= 0 && row < cheatCount;   // the Patches tab has no toggles
      paintToggleByState(s, onScreen ? getRowCheat(row) : 0, onScreen ? 1.0f : 0.0f);
   }
}
