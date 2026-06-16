#include "rbc.h"

#include <string.h>
#include <stdlib.h>

#include "dbg.h"

static uint32_t leU32(const unsigned char *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static int leI32(const unsigned char *b) { return (int)leU32(b); }

// Reads a u32-length-prefixed UTF-8 string at *pp into a fresh malloc'd NUL-terminated buffer,
// advancing *pp. Returns NULL on overflow/OOM.
static char *readInlineStr(const unsigned char *buf, long *pp, long len)
{
    long p = *pp;
    if (p + 4 > len) return NULL;
    uint32_t n = leU32(buf + p); p += 4;
    if (p + (long)n > len) return NULL;
    char *s = (char *)malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, buf + p, n); s[n] = '\0';
    *pp = p + n;
    return s;
}

int parseRbc(const unsigned char *buf, long len, RbcProgram *out)
{
    memset(out, 0, sizeof(*out));
    if (len < 20 || memcmp(buf, "RPYB", 4) != 0) { logError("[rbc] bad magic / short\n"); return -1; }

    long p = 4;
    uint32_t instrN    = leU32(buf + p); p += 4;
    uint32_t stringN   = leU32(buf + p); p += 4;
    uint32_t labelN    = leU32(buf + p); p += 4;
    out->entryAddr     = leU32(buf + p); p += 4;

    // Instructions: fixed 13 bytes each { u8 op; i32 a; i32 b; i32 c }.
    if (p + (long)instrN * 13 > len) { logError("[rbc] instr region overflow\n"); return -2; }
    out->instrCount = (int)instrN;
    out->code = (RbcInstr *)malloc(sizeof(RbcInstr) * (instrN ? instrN : 1));
    if (!out->code) return -3;
    for (uint32_t i = 0; i < instrN; i++)
    {
        const unsigned char *q = buf + p;
        out->code[i].op = q[0];
        out->code[i].a = leI32(q + 1);
        out->code[i].b = leI32(q + 5);
        out->code[i].c = leI32(q + 9);
        p += 13;
    }

    // String table: u32 len + bytes (copied, NUL-terminated).
    out->stringCount = (int)stringN;
    out->strings = (char **)malloc(sizeof(char *) * (stringN ? stringN : 1));
    if (!out->strings) { freeRbc(out); return -4; }
    for (uint32_t i = 0; i < stringN; i++) out->strings[i] = NULL;
    for (uint32_t i = 0; i < stringN; i++)
    {
        if (p + 4 > len) { logError("[rbc] string len overflow\n"); freeRbc(out); return -5; }
        uint32_t slen = leU32(buf + p); p += 4;
        if (p + (long)slen > len) { logError("[rbc] string data overflow\n"); freeRbc(out); return -6; }
        char *s = (char *)malloc(slen + 1);
        if (!s) { freeRbc(out); return -7; }
        memcpy(s, buf + p, slen);
        s[slen] = '\0';
        out->strings[i] = s;
        p += slen;
    }

    // Labels follow (u32 nameLen + bytes + u32 addr). Kept so the runtime can start at or
    // jump to a named label (splashscreen, gallery, ...), not just the entry.
    out->labelCount = (int)labelN;
    out->labelNames = (char **)malloc(sizeof(char *) * (labelN ? labelN : 1));
    out->labelAddrs = (int *)malloc(sizeof(int) * (labelN ? labelN : 1));
    if (!out->labelNames || !out->labelAddrs) { freeRbc(out); return -8; }
    for (uint32_t i = 0; i < labelN; i++) out->labelNames[i] = NULL;
    for (uint32_t i = 0; i < labelN; i++)
    {
        if (p + 4 > len) { logError("[rbc] label len overflow\n"); freeRbc(out); return -8; }
        uint32_t nameLen = leU32(buf + p); p += 4;
        if (p + (long)nameLen + 4 > len) { logError("[rbc] label data overflow\n"); freeRbc(out); return -9; }
        char *name = (char *)malloc(nameLen + 1);
        if (!name) { freeRbc(out); return -9; }
        memcpy(name, buf + p, nameLen); name[nameLen] = '\0';
        out->labelNames[i] = name;
        out->labelAddrs[i] = (int)leU32(buf + p + nameLen);
        p += nameLen + 4;   // name bytes + u32 addr
    }

    // Expr section (format v2+): u32 exprCount; each { u32 opCount; opCount * { u8 op; i32 arg } }.
    // v1 bundles have no expr section -- leave exprCount 0 and stop.
    if (p + 4 <= len)
    {
        uint32_t exprN = leU32(buf + p); p += 4;
        out->exprCount = (int)exprN;
        out->exprs = (RbcExpr *)malloc(sizeof(RbcExpr) * (exprN ? exprN : 1));
        if (!out->exprs) { freeRbc(out); return -10; }
        for (uint32_t i = 0; i < exprN; i++) { out->exprs[i].opCount = 0; out->exprs[i].ops = NULL; }
        for (uint32_t i = 0; i < exprN; i++)
        {
            if (p + 4 > len) { logError("[rbc] expr count overflow\n"); freeRbc(out); return -11; }
            uint32_t opN = leU32(buf + p); p += 4;
            if (p + (long)opN * 5 > len) { logError("[rbc] expr ops overflow\n"); freeRbc(out); return -12; }
            out->exprs[i].opCount = (int)opN;
            out->exprs[i].ops = (RbcExprOp *)malloc(sizeof(RbcExprOp) * (opN ? opN : 1));
            if (!out->exprs[i].ops) { freeRbc(out); return -13; }
            for (uint32_t j = 0; j < opN; j++)
            {
                const unsigned char *q = buf + p;
                out->exprs[i].ops[j].op  = q[0];
                out->exprs[i].ops[j].arg = leI32(q + 1);
                p += 5;
            }
        }
    }

    // ATL section (format v3+): u32 atlCount; each { i32 repeatCount; u32 keyCount;
    //   keyCount * { u8 warper; i32 durMs; u8 propCount; propCount * { u8 prop; i32 milli } } }.
    if (p + 4 <= len)
    {
        uint32_t atlN = leU32(buf + p); p += 4;
        out->atlCount = (int)atlN;
        out->atls = (RbcAtl *)malloc(sizeof(RbcAtl) * (atlN ? atlN : 1));
        if (!out->atls) { freeRbc(out); return -14; }
        for (uint32_t i = 0; i < atlN; i++) { out->atls[i].repeatCount = 0; out->atls[i].keyCount = 0; out->atls[i].keys = NULL; }
        for (uint32_t i = 0; i < atlN; i++)
        {
            if (p + 8 > len) { logError("[rbc] atl header overflow\n"); freeRbc(out); return -15; }
            out->atls[i].repeatCount = leI32(buf + p); p += 4;
            uint32_t keyN = leU32(buf + p); p += 4;
            out->atls[i].keyCount = (int)keyN;
            out->atls[i].keys = (RbcAtlKey *)malloc(sizeof(RbcAtlKey) * (keyN ? keyN : 1));
            if (!out->atls[i].keys) { freeRbc(out); return -16; }
            for (uint32_t k = 0; k < keyN; k++) { out->atls[i].keys[k].propCount = 0; out->atls[i].keys[k].props = NULL; }
            for (uint32_t k = 0; k < keyN; k++)
            {
                if (p + 6 > len) { logError("[rbc] atl key overflow\n"); freeRbc(out); return -17; }
                out->atls[i].keys[k].warper = buf[p]; p += 1;
                out->atls[i].keys[k].durMs = leI32(buf + p); p += 4;
                uint32_t propN = buf[p]; p += 1;
                if (p + (long)propN * 5 > len) { logError("[rbc] atl props overflow\n"); freeRbc(out); return -18; }
                out->atls[i].keys[k].propCount = (int)propN;
                out->atls[i].keys[k].props = (RbcAtlProperty *)malloc(sizeof(RbcAtlProperty) * (propN ? propN : 1));
                if (!out->atls[i].keys[k].props) { freeRbc(out); return -19; }
                for (uint32_t j = 0; j < propN; j++)
                {
                    out->atls[i].keys[k].props[j].prop  = buf[p];
                    out->atls[i].keys[k].props[j].milli = leI32(buf + p + 1);
                    p += 5;
                }
            }
        }
    }

    // Imagemap section: u32 count; each {
    //   str kind; str ground; str idle; str hover; str selectedIdle; str selectedHover;
    //   u32 hotspotCount; hotspotCount * { i32 x0; i32 y0; i32 x1; i32 y1; str name } }
    //   (strings inline u32 len + utf8).
    if (p + 4 <= len)
    {
        uint32_t imN = leU32(buf + p); p += 4;
        out->imapCount = (int)imN;
        out->imaps = (RbcImageMap *)malloc(sizeof(RbcImageMap) * (imN ? imN : 1));
        if (!out->imaps) { freeRbc(out); return -20; }
        for (uint32_t i = 0; i < imN; i++)
        {
            out->imaps[i].kind = NULL; out->imaps[i].ground = NULL; out->imaps[i].idle = NULL;
            out->imaps[i].hover = NULL; out->imaps[i].selectedIdle = NULL; out->imaps[i].selectedHover = NULL;
            out->imaps[i].hotspotCount = 0; out->imaps[i].hotspots = NULL;
        }
        for (uint32_t i = 0; i < imN; i++)
        {
            out->imaps[i].kind          = readInlineStr(buf, &p, len);
            out->imaps[i].ground        = readInlineStr(buf, &p, len);
            out->imaps[i].idle          = readInlineStr(buf, &p, len);
            out->imaps[i].hover         = readInlineStr(buf, &p, len);
            out->imaps[i].selectedIdle  = readInlineStr(buf, &p, len);
            out->imaps[i].selectedHover = readInlineStr(buf, &p, len);
            if (!out->imaps[i].kind || !out->imaps[i].ground || !out->imaps[i].idle || !out->imaps[i].hover
                || !out->imaps[i].selectedIdle || !out->imaps[i].selectedHover || p + 4 > len) { freeRbc(out); return -21; }
            uint32_t hsN = leU32(buf + p); p += 4;
            out->imaps[i].hotspotCount = (int)hsN;
            out->imaps[i].hotspots = (RbcHotspot *)malloc(sizeof(RbcHotspot) * (hsN ? hsN : 1));
            if (!out->imaps[i].hotspots) { freeRbc(out); return -22; }
            for (uint32_t h = 0; h < hsN; h++) out->imaps[i].hotspots[h].name = NULL;
            for (uint32_t h = 0; h < hsN; h++)
            {
                if (p + 16 > len) { freeRbc(out); return -23; }
                out->imaps[i].hotspots[h].x0 = leI32(buf + p); p += 4;
                out->imaps[i].hotspots[h].y0 = leI32(buf + p); p += 4;
                out->imaps[i].hotspots[h].x1 = leI32(buf + p); p += 4;
                out->imaps[i].hotspots[h].y1 = leI32(buf + p); p += 4;
                out->imaps[i].hotspots[h].name = readInlineStr(buf, &p, len);
                if (!out->imaps[i].hotspots[h].name) { freeRbc(out); return -24; }
            }
        }
    }

    // Overlay (HUD) section: u32 count; each { str name; u32 widgetCount;
    //   widgetCount * { u8 kind; i32 x; i32 y; str a; str b; str action; i32 guardExpr } }.
    if (p + 4 <= len)
    {
        uint32_t ovN = leU32(buf + p); p += 4;
        out->overlayCount = (int)ovN;
        out->overlays = (RbcOverlay *)malloc(sizeof(RbcOverlay) * (ovN ? ovN : 1));
        if (!out->overlays) { freeRbc(out); return -25; }
        for (uint32_t i = 0; i < ovN; i++) { out->overlays[i].name = NULL; out->overlays[i].widgetCount = 0; out->overlays[i].widgets = NULL; }
        for (uint32_t i = 0; i < ovN; i++)
        {
            out->overlays[i].name = readInlineStr(buf, &p, len);
            if (!out->overlays[i].name || p + 4 > len) { freeRbc(out); return -26; }
            uint32_t wN = leU32(buf + p); p += 4;
            out->overlays[i].widgetCount = (int)wN;
            out->overlays[i].widgets = (RbcOvWidget *)malloc(sizeof(RbcOvWidget) * (wN ? wN : 1));
            if (!out->overlays[i].widgets) { freeRbc(out); return -27; }
            for (uint32_t w = 0; w < wN; w++) { out->overlays[i].widgets[w].a = NULL; out->overlays[i].widgets[w].b = NULL; out->overlays[i].widgets[w].action = NULL; }
            for (uint32_t w = 0; w < wN; w++)
            {
                RbcOvWidget *wd = &out->overlays[i].widgets[w];
                if (p + 9 > len) { freeRbc(out); return -28; }
                wd->kind = buf[p]; p += 1;
                wd->x = leI32(buf + p); p += 4;
                wd->y = leI32(buf + p); p += 4;
                wd->a = readInlineStr(buf, &p, len);
                wd->b = readInlineStr(buf, &p, len);
                wd->action = readInlineStr(buf, &p, len);
                if (!wd->a || !wd->b || !wd->action || p + 4 > len) { freeRbc(out); return -29; }
                wd->guardExpr = leI32(buf + p); p += 4;
            }
        }
    }

    logInfo("[rbc] parsed: instrs=%d strings=%d exprs=%d atls=%d imaps=%d overlays=%d entry=%u\n",
            out->instrCount, out->stringCount, out->exprCount, out->atlCount, out->imapCount, out->overlayCount, out->entryAddr);
    return 0;
}

void freeRbc(RbcProgram *p)
{
    if (p->code) { free(p->code); p->code = NULL; }
    if (p->strings)
    {
        for (int i = 0; i < p->stringCount; i++) if (p->strings[i]) free(p->strings[i]);
        free(p->strings);
        p->strings = NULL;
    }
    if (p->exprs)
    {
        for (int i = 0; i < p->exprCount; i++) if (p->exprs[i].ops) free(p->exprs[i].ops);
        free(p->exprs);
        p->exprs = NULL;
    }
    if (p->atls)
    {
        for (int i = 0; i < p->atlCount; i++)
            if (p->atls[i].keys)
            {
                for (int k = 0; k < p->atls[i].keyCount; k++) if (p->atls[i].keys[k].props) free(p->atls[i].keys[k].props);
                free(p->atls[i].keys);
            }
        free(p->atls);
        p->atls = NULL;
    }
    if (p->imaps)
    {
        for (int i = 0; i < p->imapCount; i++)
        {
            if (p->imaps[i].kind) free(p->imaps[i].kind);
            if (p->imaps[i].ground) free(p->imaps[i].ground);
            if (p->imaps[i].idle) free(p->imaps[i].idle);
            if (p->imaps[i].hover) free(p->imaps[i].hover);
            if (p->imaps[i].selectedIdle) free(p->imaps[i].selectedIdle);
            if (p->imaps[i].selectedHover) free(p->imaps[i].selectedHover);
            if (p->imaps[i].hotspots)
            {
                for (int h = 0; h < p->imaps[i].hotspotCount; h++) if (p->imaps[i].hotspots[h].name) free(p->imaps[i].hotspots[h].name);
                free(p->imaps[i].hotspots);
            }
        }
        free(p->imaps);
        p->imaps = NULL;
    }
    if (p->overlays)
    {
        for (int i = 0; i < p->overlayCount; i++)
        {
            if (p->overlays[i].name) free(p->overlays[i].name);
            if (p->overlays[i].widgets)
            {
                for (int w = 0; w < p->overlays[i].widgetCount; w++)
                {
                    if (p->overlays[i].widgets[w].a) free(p->overlays[i].widgets[w].a);
                    if (p->overlays[i].widgets[w].b) free(p->overlays[i].widgets[w].b);
                    if (p->overlays[i].widgets[w].action) free(p->overlays[i].widgets[w].action);
                }
                free(p->overlays[i].widgets);
            }
        }
        free(p->overlays);
        p->overlays = NULL;
    }
    if (p->labelNames)
    {
        for (int i = 0; i < p->labelCount; i++) if (p->labelNames[i]) free(p->labelNames[i]);
        free(p->labelNames);
        p->labelNames = NULL;
    }
    if (p->labelAddrs) { free(p->labelAddrs); p->labelAddrs = NULL; }
    p->instrCount = 0;
    p->stringCount = 0;
    p->exprCount = 0;
    p->atlCount = 0;
    p->imapCount = 0;
    p->overlayCount = 0;
    p->labelCount = 0;
}

int getRbcLabelAddr(const RbcProgram *p, const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < p->labelCount; i++)
        if (p->labelNames[i] && strcmp(p->labelNames[i], name) == 0) return p->labelAddrs[i];
    return -1;
}

const char *getRbcStr(const RbcProgram *p, int id)
{
    if (id < 0 || id >= p->stringCount || !p->strings[id]) return "";
    return p->strings[id];
}

const RbcExpr *getRbcExpr(const RbcProgram *p, int id)
{
    if (id < 0 || id >= p->exprCount) return NULL;
    return &p->exprs[id];
}

const RbcAtl *getRbcAtl(const RbcProgram *p, int id)
{
    if (id < 0 || id >= p->atlCount) return NULL;
    return &p->atls[id];
}

const RbcImageMap *getRbcImageMap(const RbcProgram *p, int id)
{
    if (id < 0 || id >= p->imapCount) return NULL;
    return &p->imaps[id];
}

// Find the themed imagemap for a screen ("navigation"/"load_save"/"preferences"/"yesno_prompt"/
// "main_menu"); NULL if the game doesn't provide that imagemap layout.
const RbcImageMap *getRbcImageMapByKind(const RbcProgram *p, const char *kind)
{
    if (!p || !kind) return NULL;
    for (int i = 0; i < p->imapCount; i++)
        if (p->imaps[i].kind && strcmp(p->imaps[i].kind, kind) == 0) return &p->imaps[i];
    return NULL;
}

const RbcOverlay *getRbcOverlayByName(const RbcProgram *p, const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < p->overlayCount; i++)
        if (p->overlays[i].name && strcmp(p->overlays[i].name, name) == 0) return &p->overlays[i];
    return NULL;
}
