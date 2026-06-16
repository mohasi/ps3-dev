#include "vars.h"

#include <stdlib.h>
#include <string.h>

#include "printf.h"
#include "dbg.h"

// Game state lives in a flat name->Value table. Strings produced during one eval are
// bump-allocated from a scratch arena (reset per eval); variable storage owns its own
// copied strings.

#define VAR_MAX     1024
#define EXPR_STACK  64
#define EXPR_ARENA  8192

typedef struct { char *name; Value v; } Var;   // v.s (when VT_STR) is heap-owned by the table
static Var vars[VAR_MAX];
static int varCount;
static int varsDirty = 1;   // have vars changed since the last snapshot? (drives rollback dedup)

static char exprArena[EXPR_ARENA];
static int  exprArenaUsed;

// Heap copy of a C string (strdup isn't guaranteed in this SDK's libc).
static char *dupStr(const char *s)
{
    if (!s) s = "";
    int n = (int)strlen(s);
    char *r = (char *)malloc(n + 1);
    if (r) { memcpy(r, s, n); r[n] = '\0'; }
    return r;
}

static char *arenaCopy(const char *s, int len)
{
    if (exprArenaUsed + len + 1 > EXPR_ARENA) return NULL;
    char *r = exprArena + exprArenaUsed;
    memcpy(r, s, len); r[len] = '\0';
    exprArenaUsed += len + 1;
    return r;
}

static Value vNone(void) { Value v; v.t = VT_NONE; v.i = 0; v.f = 0; v.s = NULL; return v; }
static Value vInt(long x) { Value v = vNone(); v.t = VT_INT; v.i = x; return v; }
static Value vBool(int x) { Value v = vNone(); v.t = VT_BOOL; v.i = x ? 1 : 0; return v; }
static Value vFloat(double x) { Value v = vNone(); v.t = VT_FLOAT; v.f = x; return v; }
static Value vStr(const char *s) { Value v = vNone(); v.t = VT_STR; v.s = s ? s : ""; return v; }

static int    valIsNum(const Value *v) { return v->t == VT_INT || v->t == VT_BOOL || v->t == VT_FLOAT; }
// Python 2 orders mismatched types by a fixed precedence: None is smaller than everything, numbers
// rank below other types, then by type name. For our value set that totals to None < numbers < str.
static int    typeRank(const Value *v) { return v->t == VT_NONE ? 0 : (valIsNum(v) ? 1 : 2); }
static int    valIsFloat(const Value *v) { return v->t == VT_FLOAT; }
static double valNum(const Value *v) { return v->t == VT_FLOAT ? v->f : (double)v->i; }

static int valTruthy(const Value *v)
{
    switch (v->t)
    {
        case VT_NONE:  return 0;
        case VT_BOOL:
        case VT_INT:   return v->i != 0;
        case VT_FLOAT: return v->f != 0.0;
        case VT_STR:   return v->s && v->s[0] != '\0';
    }
    return 0;
}

// String form of a value (into the scratch arena for non-strings). Used by concat + interp.
static const char *valStr(const Value *v)
{
    char tmp[64];
    switch (v->t)
    {
        case VT_STR:   return v->s ? v->s : "";
        case VT_BOOL:  return v->i ? "True" : "False";
        case VT_NONE:  return "None";
        case VT_INT:   snprintf(tmp, sizeof tmp, "%ld", v->i); break;
        case VT_FLOAT:
            // Python 2 str(float): 12 significant digits, and always a decimal point
            // (str(3.0) == "3.0", not "3"). C's %g drops the point and caps at 6 sig figs.
            snprintf(tmp, sizeof tmp, "%.12g", v->f);
            if (!strpbrk(tmp, ".eEnN")) { size_t n = strlen(tmp); if (n + 2 < sizeof tmp) { tmp[n] = '.'; tmp[n + 1] = '0'; tmp[n + 2] = '\0'; } }
            break;
        default:       return "";
    }
    { char *r = arenaCopy(tmp, (int)strlen(tmp)); return r ? r : ""; }
}

static Var *varFind(const char *name)
{
    for (int i = 0; i < varCount; i++) if (strcmp(vars[i].name, name) == 0) return &vars[i];
    return NULL;
}

int getVar(const char *name, Value *out)
{
    Var *v = varFind(name);
    if (!v) { *out = vNone(); return 0; }
    *out = v->v;
    return 1;
}

void setVar(const char *name, const Value *val, int ifAbsent)
{
    Var *v = varFind(name);
    if (v && ifAbsent) return;
    if (!v)
    {
        if (varCount >= VAR_MAX) { logWarn("[rpp] var table full, dropping %s\n", name); return; }
        v = &vars[varCount++];
        v->name = dupStr(name);
        v->v = vNone();
    }
    if (v->v.t == VT_STR && v->v.s) free((void *)v->v.s);   // release prior owned string
    v->v = *val;
    if (val->t == VT_STR) v->v.s = dupStr(val->s ? val->s : "");
    varsDirty = 1;   // a game mutation -> the next rollback frame needs a fresh snapshot
}

// Python 2 floored integer division (C's / truncates toward zero; Python floors:
// -7 / 2 == -4). Classic Ren'Py scripts run under Python 2, where `/` on two ints IS
// floor division.
static long floorDivL(long a, long b)
{
    long q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

// Python floored modulo: the result's sign follows the divisor (-7 % 2 == 1).
static long floorModL(long a, long b)
{
    long r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

// floor(x) without <math.h> (vars.c stays freestanding); fine for VN-scale magnitudes.
static double floorD(double x) { long t = (long)x; return (x < 0 && (double)t != x) ? (double)(t - 1) : (double)t; }

// Python float modulo also follows the divisor's sign: a - floor(a/b)*b.
static double floorModD(double a, double b)
{
    double q = a / b;
    long fq = (long)q;
    if (q < 0 && (double)fq != q) fq--;   // floor
    return a - (double)fq * b;
}

// Applies an arithmetic/comparison/logical op to two values, returning a new Value.
static Value applyBinary(unsigned char op, Value a, Value b)
{
    switch (op)
    {
        case EX_ADD:
            if (a.t == VT_STR || b.t == VT_STR)
            {
                const char *sa = valStr(&a), *sb = valStr(&b);
                int la = (int)strlen(sa), lb = (int)strlen(sb);
                // sa may already sit in the arena; build the concat past the current head.
                char *r = arenaCopy(sa, la);
                if (r) { char *r2 = arenaCopy(sb, lb); if (r2) memmove(r + la, r2, lb + 1); }
                return vStr(r ? r : "");
            }
            if (valIsFloat(&a) || valIsFloat(&b)) return vFloat(valNum(&a) + valNum(&b));
            return vInt(a.i + b.i);
        case EX_SUB:
            if (valIsFloat(&a) || valIsFloat(&b)) return vFloat(valNum(&a) - valNum(&b));
            return vInt(a.i - b.i);
        case EX_MUL:
            if (valIsFloat(&a) || valIsFloat(&b)) return vFloat(valNum(&a) * valNum(&b));
            return vInt(a.i * b.i);
        case EX_DIV:
            // Python 2 `/`: int/int floors; any float operand -> true division.
            if (valIsFloat(&a) || valIsFloat(&b)) { double d = valNum(&b); return vFloat(d != 0.0 ? valNum(&a) / d : 0.0); }
            return vInt(b.i != 0 ? floorDivL(a.i, b.i) : 0);
        case EX_FLOORDIV:
            // `//` always floors, for floats too (7.0 // 2.0 == 3.0, not 3.5).
            if (valIsFloat(&a) || valIsFloat(&b)) { double d = valNum(&b); return vFloat(d != 0.0 ? floorD(valNum(&a) / d) : 0.0); }
            return vInt(b.i != 0 ? floorDivL(a.i, b.i) : 0);
        case EX_MOD:
            // Python modulo: result sign follows the divisor (floored), for ints and floats.
            if (valIsFloat(&a) || valIsFloat(&b)) { double d = valNum(&b); return vFloat(d != 0.0 ? floorModD(valNum(&a), d) : 0.0); }
            return vInt(b.i != 0 ? floorModL(a.i, b.i) : 0);

        case EX_EQ: case EX_NE:
        {
            int eq;
            if (a.t == VT_STR && b.t == VT_STR) eq = strcmp(a.s ? a.s : "", b.s ? b.s : "") == 0;
            else if ((a.t == VT_INT || a.t == VT_BOOL) && (b.t == VT_INT || b.t == VT_BOOL)) eq = (a.i == b.i);   // exact (no double rounding of large ints)
            else if (valIsNum(&a) && valIsNum(&b)) eq = (valNum(&a) == valNum(&b));
            else if (a.t == VT_NONE && b.t == VT_NONE) eq = 1;
            else eq = 0;                                   // mismatched types: not equal (Python 2: 1 == "1" is False)
            return vBool(op == EX_EQ ? eq : !eq);
        }
        case EX_LT: case EX_LE: case EX_GT: case EX_GE:
        {
            int c;
            if (a.t == VT_STR && b.t == VT_STR) c = strcmp(a.s ? a.s : "", b.s ? b.s : "");
            else if ((a.t == VT_INT || a.t == VT_BOOL) && (b.t == VT_INT || b.t == VT_BOOL)) c = a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
            else if (valIsNum(&a) && valIsNum(&b)) { double d = valNum(&a) - valNum(&b); c = d < 0 ? -1 : (d > 0 ? 1 : 0); }
            else { int ra = typeRank(&a), rb = typeRank(&b); c = ra < rb ? -1 : (ra > rb ? 1 : 0); }   // Python 2: None < numbers < str
            switch (op) { case EX_LT: return vBool(c < 0); case EX_LE: return vBool(c <= 0);
                          case EX_GT: return vBool(c > 0); default: return vBool(c >= 0); }
        }

        case EX_AND: return valTruthy(&a) ? b : a;         // Python and/or return an operand
        case EX_OR:  return valTruthy(&a) ? a : b;

        case EX_MAX: case EX_MIN:
        {
            // Same ordering as the comparisons above (str/num/Python-2 cross-type rank). max()/min()
            // return the FIRST argument on a tie, matching Python and our left-fold compilation.
            int c;
            if (a.t == VT_STR && b.t == VT_STR) c = strcmp(a.s ? a.s : "", b.s ? b.s : "");
            else if ((a.t == VT_INT || a.t == VT_BOOL) && (b.t == VT_INT || b.t == VT_BOOL)) c = a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
            else if (valIsNum(&a) && valIsNum(&b)) { double d = valNum(&a) - valNum(&b); c = d < 0 ? -1 : (d > 0 ? 1 : 0); }
            else { int ra = typeRank(&a), rb = typeRank(&b); c = ra < rb ? -1 : (ra > rb ? 1 : 0); }
            if (op == EX_MAX) return c >= 0 ? a : b;
            return c <= 0 ? a : b;
        }
    }
    return vNone();
}

int evalExpr(const RbcProgram *p, int idx, Value *out)
{
    *out = vNone();
    const RbcExpr *e = getRbcExpr(p, idx);
    if (!e) return 0;
    exprArenaUsed = 0;

    Value st[EXPR_STACK];
    int top = 0;
    for (int k = 0; k < e->opCount; k++)
    {
        unsigned char op = e->ops[k].op;
        int arg = e->ops[k].arg;
        switch (op)
        {
            case EX_PUSH_INT:   if (top >= EXPR_STACK) return 0; st[top++] = vInt(arg); break;
            case EX_PUSH_BOOL:  if (top >= EXPR_STACK) return 0; st[top++] = vBool(arg); break;
            case EX_PUSH_NONE:  if (top >= EXPR_STACK) return 0; st[top++] = vNone(); break;
            case EX_PUSH_FLOAT: if (top >= EXPR_STACK) return 0; st[top++] = vFloat(atof(getRbcStr(p, arg))); break;
            case EX_PUSH_STR:   if (top >= EXPR_STACK) return 0; st[top++] = vStr(getRbcStr(p, arg)); break;
            case EX_LOAD_VAR:   { if (top >= EXPR_STACK) return 0; Value v; getVar(getRbcStr(p, arg), &v); st[top++] = v; break; }
            case EX_NEG:        { if (top < 1) return 0; Value a = st[top-1]; st[top-1] = valIsFloat(&a) ? vFloat(-a.f) : vInt(-a.i); break; }
            case EX_NOT:        { if (top < 1) return 0; Value a = st[top-1]; st[top-1] = vBool(!valTruthy(&a)); break; }
            default:            { if (top < 2) return 0; Value b = st[--top]; Value a = st[--top]; st[top++] = applyBinary(op, a, b); break; }
        }
    }
    if (top != 1) return 0;
    *out = st[0];
    return 1;
}

int isCondTrue(const RbcProgram *p, int exprId)
{
    if (exprId < 0) return 1;
    Value v;
    if (!evalExpr(p, exprId, &v)) return 1;   // unevaluable -> assume true (legacy behaviour)
    return valTruthy(&v);
}

void applyAssign(const RbcProgram *p, const RbcInstr *in)
{
    const char *name = getRbcStr(p, in->a);
    Value rhs;
    if (!evalExpr(p, in->c, &rhs)) return;
    if (in->b != 0)   // compound: x += e -> x = x + e
    {
        unsigned char op = EX_ADD;
        switch (in->b) { case 1: op = EX_ADD; break; case 2: op = EX_SUB; break;
                         case 3: op = EX_MUL; break; case 4: op = EX_DIV; break; case 5: op = EX_MOD; break; }
        Value cur; if (!getVar(name, &cur)) cur = vInt(0);
        rhs = applyBinary(op, cur, rhs);
    }
    setVar(name, &rhs, 0);
}

void resetVars(void)
{
    for (int i = 0; i < varCount; i++)
    {
        if (vars[i].v.t == VT_STR && vars[i].v.s) free((void *)vars[i].v.s);
        if (vars[i].name) free(vars[i].name);
    }
    varCount = 0;
    varsDirty = 1;
}

// ---- rollback snapshots ----
// A snapshot is an owned copy of the whole var table (names + values, with copied strings). Rollback
// keeps one per history frame and restores it when a past line is shown, so overlays / HUD / [var]
// interpolation reflect that line's state -- this is Ren'Py rolling the store back, not just visuals.
typedef struct { char *name; Value v; } SnapEntry;
typedef struct { int count; SnapEntry *e; } VarSnap;

int  varsAreDirty(void) { return varsDirty; }
void clearVarsDirty(void) { varsDirty = 0; }

void *snapshotVars(void)
{
    VarSnap *s = (VarSnap *)malloc(sizeof(VarSnap));
    if (!s) return NULL;
    s->count = varCount;
    s->e = varCount ? (SnapEntry *)malloc(sizeof(SnapEntry) * varCount) : NULL;
    if (varCount && !s->e) { free(s); return NULL; }
    for (int i = 0; i < varCount; i++)
    {
        s->e[i].name = dupStr(vars[i].name);
        s->e[i].v = vars[i].v;
        if (vars[i].v.t == VT_STR) s->e[i].v.s = dupStr(vars[i].v.s ? vars[i].v.s : "");
    }
    return s;
}

void restoreVarsSnap(const void *snap)
{
    if (!snap) return;
    const VarSnap *s = (const VarSnap *)snap;
    resetVars();
    for (int i = 0; i < s->count; i++) setVar(s->e[i].name, &s->e[i].v, 0);
    varsDirty = 0;   // restoring is not a game mutation
}

void freeVarsSnap(void *snap)
{
    if (!snap) return;
    VarSnap *s = (VarSnap *)snap;
    for (int i = 0; i < s->count; i++)
    {
        if (s->e[i].v.t == VT_STR && s->e[i].v.s) free((void *)s->e[i].v.s);
        if (s->e[i].name) free(s->e[i].name);
    }
    free(s->e);
    free(s);
}

// Enumerate the whole store for the save blob: count + (name, value) by index.
int getVarsCount(void) { return varCount; }

int enumVars(int idx, const char **name, Value *out)
{
    if (idx < 0 || idx >= varCount) return 0;
    *name = vars[idx].name;
    *out  = vars[idx].v;
    return 1;
}

void initVars(const RbcProgram *p)
{
    resetVars();
    // default/define values. Init-block assignments (and overlay registration) now run via the
    // `__init__` prologue (runVmInit at startup), not a position heuristic here.
    for (int i = 0; i < p->instrCount; i++)
    {
        const RbcInstr *in = &p->code[i];
        if (in->op == RBC_DEFAULT && in->c >= 0)
        {
            Value v; if (evalExpr(p, in->c, &v)) setVar(getRbcStr(p, in->a), &v, 1);
        }
    }
    logInfo("[rpp] vars: %d defaulted\n", varCount);
}

void interpolate(const char *src, char *dst, int cap)
{
    int j = 0;
    if (!src) { if (cap > 0) dst[0] = '\0'; return; }
    for (const char *p = src; *p && j < cap - 1; )
    {
        if (p[0] == '[' && p[1] == '[') { dst[j++] = '['; p += 2; continue; }
        if (p[0] == '[')
        {
            const char *q = p + 1;
            char name[128]; int n = 0;
            while (*q && *q != ']' && *q != '!' && *q != ':' && n < (int)sizeof(name) - 1)
            {
                char c = *q;
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) break;
                name[n++] = c; q++;
            }
            name[n] = '\0';
            // Ren'Py interpolation: [name!conv:fmt]. Parse the optional !conversion and :format.
            char conv = 0;
            const char *close = q;
            if (*close == '!') { conv = close[1]; close += (close[1] && close[1] != ']' && close[1] != ':') ? 2 : 1; }
            while (*close && *close != ']') close++;           // tolerate / skip the :format spec
            Value v;
            if (n > 0 && *close == ']' && getVar(name, &v))
            {
                exprArenaUsed = 0;                            // valStr may use the arena
                const char *s = valStr(&v);
                char conv_buf[256];                           // apply !u / !l / !c (capitalize)
                if (conv == 'u' || conv == 'l' || conv == 'c')
                {
                    int k = 0;
                    for (; s[k] && k < (int)sizeof conv_buf - 1; k++)
                    {
                        char c = s[k];
                        if (conv == 'u' && c >= 'a' && c <= 'z') c -= 32;
                        else if (conv == 'l' && c >= 'A' && c <= 'Z') c += 32;
                        else if (conv == 'c' && k == 0 && c >= 'a' && c <= 'z') c -= 32;
                        conv_buf[k] = c;
                    }
                    conv_buf[k] = '\0';
                    s = conv_buf;
                }
                while (*s && j < cap - 1) dst[j++] = *s++;
                p = close + 1;
                continue;
            }
            // not a known variable: emit the literal '[' and continue past it
            dst[j++] = '['; p++;
            continue;
        }
        dst[j++] = *p++;
    }
    dst[j] = '\0';
}
