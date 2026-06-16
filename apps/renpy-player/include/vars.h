#pragma once

// Game variable store + RPN expression evaluator + [var] text interpolation.
//
// Conditions (if/while/menu) and assignments ($ x=..., default/define) are compiled to RPN
// by the PC tool; this module runs a tiny stack machine over them. Semantics follow the
// Python 2 that classic Ren'Py games run under (floored integer division and floored
// modulo; `and`/`or` return an operand; mixed-type equality is False).

#include "rbc.h"

typedef enum { VT_NONE, VT_INT, VT_BOOL, VT_FLOAT, VT_STR } ValType;
typedef struct { ValType t; long i; double f; const char *s; } Value;

// One-time init pass before play starts: establish default/define values and run the
// assignments that sit in the init region (before the entry label) -- mirroring how
// Ren'Py establishes defaults at game start. Calls resetVars first.
void initVars(const RbcProgram *p);

// Frees all variable storage.
void resetVars(void);

// Enumerate the store for the save blob: getVarsCount() entries, each read by enumVars(idx,...).
// Restore is just resetVars() then setVar() for each saved (name, value).
int  getVarsCount(void);
int  enumVars(int idx, const char **name, Value *out);

// Reads name into *out. Returns 1 if defined (else *out = None, returns 0).
int  getVar(const char *name, Value *out);

// Stores name=val. ifAbsent (default semantics): only set when not already defined.
// String values are copied into table-owned storage.
void setVar(const char *name, const Value *val, int ifAbsent);

// Rollback snapshots: snapshotVars() returns an owned copy of the whole store; restoreVarsSnap()
// replaces the live store with a snapshot (used when showing a past line so its state is faithful);
// freeVarsSnap() releases one. varsAreDirty()/clearVarsDirty() let the caller dedup unchanged frames
// (share one snapshot) so memory stays bounded. Snapshots are opaque (void*).
void *snapshotVars(void);
void  restoreVarsSnap(const void *snap);
void  freeVarsSnap(void *snap);
int   varsAreDirty(void);
void  clearVarsDirty(void);

// Evaluates expr program `idx` into *out. Returns 1 on success, 0 if the program is
// missing/malformed (out set to None).
int  evalExpr(const RbcProgram *p, int idx, Value *out);

// Truthiness of a condition expr id. -1 (no expr) => true ("take the block" default);
// an unevaluable expr is also treated as true (matching the pre-evaluator behaviour).
int  isCondTrue(const RbcProgram *p, int exprId);

// Executes an Assign op (A=varname, B=op kind, C=value expr id).
void applyAssign(const RbcProgram *p, const RbcInstr *in);

// Ren'Py [name] interpolation into dst. `[[` is a literal '['. `!s`-style flags and
// `:fmt` specs are dropped; unknown variables stay verbatim. {tags} are left untouched
// (renderFont parses those).
void interpolate(const char *src, char *dst, int cap);
