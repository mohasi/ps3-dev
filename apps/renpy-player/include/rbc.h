#pragma once

#include <stdint.h>

// Decoded game.rbc bytecode (produced by renpy-to-ps3). Opcode order MUST match the
// C# IrOp enum. Integers in the file are little-endian; rbc.c assembles bytes explicitly
// because the PS3 PPU is big-endian.
typedef enum {
    RBC_LABEL = 0, RBC_SAY, RBC_SCENE, RBC_SHOW, RBC_HIDE, RBC_WITH, RBC_JUMP, RBC_CALL,
    RBC_RETURN, RBC_MENUSTART, RBC_CHOICE, RBC_MENUEND, RBC_IFFALSEGOTO, RBC_PYEXEC,
    RBC_DEFAULT, RBC_IMAGE, RBC_USER, RBC_PAUSE, RBC_ASSIGN, RBC_NOP, RBC_END,
    RBC_IMAGEMAP,     // A=result varname str id  B=imagemap table id (interactive menu)
    RBC_OVERLAY_SHOW, // A=overlay name str id (config.overlay_functions.append)
    RBC_OVERLAY_HIDE  // A=overlay name str id (config.overlay_functions.remove)
} RbcOp;

// RPN expression opcodes for the condition/assignment evaluator. MUST stay in lockstep with
// the C# ExprOp enum (RenPy/ExprCompiler.cs). `arg` meaning: PushInt=value, PushBool=0/1,
// PushFloat/PushStr/LoadVar=string-table id, operators unused.
typedef enum {
    EX_PUSH_INT = 0, EX_PUSH_BOOL, EX_PUSH_NONE, EX_PUSH_FLOAT, EX_PUSH_STR, EX_LOAD_VAR,
    EX_NEG, EX_NOT,
    EX_ADD, EX_SUB, EX_MUL, EX_DIV, EX_MOD,
    EX_EQ, EX_NE, EX_LT, EX_LE, EX_GT, EX_GE,
    EX_AND, EX_OR,
    EX_FLOORDIV,  // `//` floor division (must match ExprCompiler.ExprOp ordinal)
    EX_MAX, EX_MIN   // built-in max()/min(), emitted as a left-fold of these binary ops
} RbcExprOpCode;

typedef struct {
    unsigned char op;
    int           arg;
} RbcExprOp;

typedef struct {
    int        opCount;
    RbcExprOp *ops;
} RbcExpr;

// ATL (animation) keyframe data. MUST stay in lockstep with the C# AtlWarper / AtlProp enums
// (RenPy/AtlCompiler.cs). Property values are stored as value*1000 (milli) to avoid floats in the file.
typedef enum { ATL_WARP_INSTANT = 0, ATL_WARP_LINEAR, ATL_WARP_PAUSE, ATL_WARP_EASE, ATL_WARP_EASEIN, ATL_WARP_EASEOUT } RbcAtlWarper;
typedef enum {
    ATL_XPOS = 0, ATL_YPOS, ATL_XANCHOR, ATL_YANCHOR, ATL_XALIGN, ATL_YALIGN,
    ATL_ZOOM, ATL_XZOOM, ATL_YZOOM, ATL_ALPHA, ATL_ROTATE
} RbcAtlProp;

typedef struct { unsigned char prop; int milli; } RbcAtlProperty;   // value * 1000
typedef struct {
    unsigned char   warper;
    int             durMs;
    int             propCount;
    RbcAtlProperty *props;
} RbcAtlKey;
typedef struct {
    int        repeatCount;   // 0 = play once; -1 = loop forever; N = repeat N times
    int        keyCount;
    RbcAtlKey *keys;
} RbcAtl;

// Imagemap menu. Two faithful forms share one struct (mirrors _layout/imagemap_common.rpym's _ImageMapper):
//   - kind == "" (simple renpy.imagemap): full-screen `ground`; the `hover` image is drawn clipped to the
//     focused hotspot; selecting one returns its `name` (the value).
//   - kind != "" (themed screen: "navigation"/"load_save"/"preferences"/"yesno_prompt"/"main_menu"):
//     ground + idle/hover/selectedIdle/selectedHover state images, each hotspot a NAMED button/bar/slot.
//     The player's generic imagemap renderer replays the engine algorithm and dispatches by `name`.
typedef struct { int x0, y0, x1, y1; char *name; } RbcHotspot;
typedef struct {
    char       *kind;          // "" = simple; else themed screen name
    char       *ground;
    char       *idle;          // themed idle state (falls back to ground)
    char       *hover;         // simple: hover image; themed: hover state
    char       *selectedIdle;  // themed
    char       *selectedHover; // themed
    int         hotspotCount;
    RbcHotspot *hotspots;
} RbcImageMap;

// Overlay (HUD): a named set of guarded widgets (drawn each frame while the overlay is active).
typedef enum { OV_IMAGE = 0, OV_TEXT = 1, OV_IMAGEBUTTON = 2 } RbcOvKind;
typedef struct {
    unsigned char kind;
    int   x, y;        // native-pixel position
    char *a;           // Image: file; Text: template; ImageButton: idle file
    char *b;           // ImageButton: hover file
    char *action;      // ImageButton: "call:<label>" / "menu:<prompt>" / "" (inert)
    int   guardExpr;   // RPN expr id gating this widget (-1 = always)
} RbcOvWidget;
typedef struct {
    char        *name;
    int          widgetCount;
    RbcOvWidget *widgets;
} RbcOverlay;

typedef struct {
    unsigned char op;
    int a, b, c;
} RbcInstr;

typedef struct {
    uint32_t entryAddr;
    int       instrCount;
    RbcInstr *code;
    int       stringCount;
    char    **strings;   // each NUL-terminated
    int       exprCount;  // v2+: compiled RPN expression programs (0 for v1 bundles)
    RbcExpr  *exprs;
    int       atlCount;   // compiled ATL keyframe programs
    RbcAtl   *atls;
    int       imapCount;  // imagemap menus
    RbcImageMap *imaps;
    int       overlayCount;  // HUD overlays
    RbcOverlay *overlays;
    int       labelCount; // label table (name -> instruction address)
    char    **labelNames;
    int      *labelAddrs;
} RbcProgram;

// Address of a named label, or -1 if there is none. Used to start at a label other than the
// entry (e.g. `splashscreen`) or to jump to one (gallery, etc.).
int getRbcLabelAddr(const RbcProgram *p, const char *name);

// Bounds-safe expr-program accessor (NULL for out-of-range ids).
const RbcExpr *getRbcExpr(const RbcProgram *p, int id);

// Bounds-safe ATL-program accessor (NULL for out-of-range ids).
const RbcAtl *getRbcAtl(const RbcProgram *p, int id);

// Bounds-safe imagemap accessor (NULL for out-of-range ids).
const RbcImageMap *getRbcImageMap(const RbcProgram *p, int id);

// Find the themed imagemap for a screen ("load_save"/"preferences"/"navigation"/...); NULL if absent.
const RbcImageMap *getRbcImageMapByKind(const RbcProgram *p, const char *kind);

// Find a HUD overlay by name (NULL if none).
const RbcOverlay *getRbcOverlayByName(const RbcProgram *p, const char *name);

// Parse a game.rbc byte buffer into out. Returns 0 on success, negative on error.
// The input buffer may be freed by the caller after parsing (data is copied).
int  parseRbc(const unsigned char *buf, long len, RbcProgram *out);
void freeRbc(RbcProgram *p);

// Bounds-safe string-table accessor ("" for out-of-range ids).
const char *getRbcStr(const RbcProgram *p, int id);
