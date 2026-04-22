#pragma once

/* Generates sdm.xml and patches category_game.xml to add "Mount Disc Image"
 * below "Package Manager" in the XMB Games column. Requires Cobra (EVILNAT). */

#include <stdint.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>
#include "dbg.h"
#include "vsh.h"

/* Paths */
static const char *pathIsoDir      = "/dev_hdd0/PS3ISO";
static const char *pathXmlHostRoot = "/dev_hdd0/xmlhost";
static const char *pathXmlHostGp   = "/dev_hdd0/xmlhost/game_plugin";
static const char *pathSdmXml      = "/dev_hdd0/xmlhost/game_plugin/sdm.xml";
static const char *pathCatFlash    = "/dev_flash/vsh/resource/explore/xmb/category_game.xml";
static const char *pathCatBlind    = "/dev_blind/vsh/resource/explore/xmb/category_game.xml";
static const char *pathCatBackup   = "/dev_hdd0/tmp/sdm_category_game.xml.bak";

/* category_game.xml inject: sits directly after the seg_package_files
 * ("Package Manager") Query in <View id="root">, indented with 3 tabs to
 * match the surrounding entries. On EVILNAT 4.75 the rendered Games column
 * is <View id="root"> (webMAN's xmb_app3 entry lives there, and we see it),
 * NOT root_for_BDU. seg_pkg_install is a different item that only exists in
 * root_for_BDU — it would never render for us. */
static const char *injectAnchor = "key=\"seg_package_files\"";
static const char *injectLine =
    "\t\t\t<Query class=\"type:x-xmb/folder-pixmap\" key=\"seg_sdm\" "
    "attr=\"seg_sdm\" src=\"xmb://localhost/dev_hdd0/xmlhost/game_plugin/sdm.xml#seg_sdm\"/>\n";

/* Working buffers. buffer holds either the generated sdm.xml or the in-flight
 * category_game.xml patch (phases never overlap). itemsBuffer collects the
 * <Item/> lines while <Table> blocks stream into buffer, since Tables must
 * precede Items inside a View. namePool holds a copy of each ISO filename so
 * we can sort them before emitting — cellFsReaddir returns entries in
 * filesystem (insertion) order, not alphabetical. */
enum {
    bufferSize   = 128 * 1024,
    itemsSize    =  32 * 1024,
    maxIsos      = 256,
    namePoolSize =  64 * 1024,   /* 256 names × ~256B worst case */
};
static char buffer[bufferSize];
static char itemsBuffer[itemsSize];
static char namePool[namePoolSize];
static int  nameOff[maxIsos];

/* patchCategoryGameXml return codes */
#define PATCH_APPLIED   1
#define PATCH_EXISTS    2
#define PATCH_FAILED   -1

/* --- small string / FS helpers --- */

static int strLen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static int strCmpICase(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int endsWithICase(const char *s, const char *suf)
{
    int ls = strLen(s), lsuf = strLen(suf);
    if (lsuf > ls) return 0;
    return strCmpICase(s + ls - lsuf, suf) == 0;
}

static int findBytes(const char *hay, int hLen, const char *needle, int nLen)
{
    if (nLen == 0 || nLen > hLen) return -1;
    for (int i = 0; i <= hLen - nLen; i++) {
        int j = 0;
        while (j < nLen && hay[i + j] == needle[j]) j++;
        if (j == nLen) return i;
    }
    return -1;
}

static int makeDir(const char *path)
{
    int r = cellFsMkdir(path, CELL_FS_S_IFDIR | 0777);
    return (r == CELL_FS_SUCCEEDED || r == (int)CELL_FS_EEXIST) ? 0 : r;
}

static int writeFile(const char *path, const char *data, uint64_t len)
{
    int fd;
    if (cellFsOpen(path, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED) return -1;
    uint64_t written = 0;
    int r = cellFsWrite(fd, data, len, &written);
    cellFsClose(fd);
    return (r == CELL_FS_SUCCEEDED && written == len) ? 0 : -1;
}

static int readFile(const char *path, char *buf, int cap)
{
    int fd;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) return -1;
    uint64_t got = 0;
    int r = cellFsRead(fd, buf, (uint64_t)(cap - 1), &got);
    cellFsClose(fd);
    if (r != CELL_FS_SUCCEEDED) return -1;
    buf[got] = '\0';
    return (int)got;
}

static int fileExists(const char *path)
{
    CellFsStat st;
    return cellFsStat(path, &st) == CELL_FS_SUCCEEDED;
}

static void appendStr(char *dst, int cap, int *off, const char *src)
{
    int o = *off;
    for (int i = 0; src[i] && o < cap - 1; i++) dst[o++] = src[i];
    *off = o;
}

static void appendXmlEscaped(char *dst, int cap, int *off, const char *src)
{
    int o = *off;
    for (int i = 0; src[i] && o < cap - 8; i++) {
        unsigned char c = (unsigned char)src[i];
        const char *e = NULL;
        if      (c == '&')  e = "&amp;";
        else if (c == '<')  e = "&lt;";
        else if (c == '>')  e = "&gt;";
        else if (c == '"')  e = "&quot;";
        else if (c == '\'') e = "&apos;";
        if (e) while (*e && o < cap - 1) dst[o++] = *e++;
        else   dst[o++] = (char)c;
    }
    *off = o;
}

/* Writes non-negative v as decimal into out. Returns chars written. */
static int intToDec(int v, char *out)
{
    char tmp[12]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    else while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
    int n = t;
    while (t--) *out++ = tmp[t];
    return n;
}

/* --- sdm.xml generation --- */

static int buildSdmXml(char *buf, int cap)
{
    int off = 0;
    appendStr(buf, cap, &off,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<XMBML version=\"1.0\">\n"
        " <View id=\"seg_sdm\">\n"
        "  <Attributes>\n"
        "   <Table key=\"seg_sdm\">\n"
        "    <Pair key=\"title\"><String>Mount Disc Image</String></Pair>\n"
        "    <Pair key=\"info\"><String>Boot a game ISO from /dev_hdd0/PS3ISO as a disc</String></Pair>\n"
        "    <Pair key=\"icon_rsc\"><String>tex_disc</String></Pair>\n"
        "    <Pair key=\"ingame\"><String>disable</String></Pair>\n"
        "   </Table>\n"
        "  </Attributes>\n"
        "  <Items>\n"
        "   <Query class=\"type:x-xmb/folder-pixmap\" key=\"sdm_items\" attr=\"seg_sdm\""
        " src=\"xmb://localhost/dev_hdd0/xmlhost/game_plugin/sdm.xml#seg_sdm_list\"/>\n"
        "  </Items>\n"
        " </View>\n"
        " <View id=\"seg_sdm_list\">\n"
        "  <Attributes>\n");

    /* Phase 1: collect ISO filenames into namePool. cellFsReaddir returns
     * entries in filesystem order (creation time), not alphabetical, so we
     * can't emit during the walk if we want a sorted menu. */
    int poolOff = 0;
    int count = 0;
    int fd;
    if (cellFsOpendir(pathIsoDir, &fd) == CELL_FS_SUCCEEDED) {
        CellFsDirent ent; uint64_t entSz;
        while (count < maxIsos &&
               cellFsReaddir(fd, &ent, &entSz) == CELL_FS_SUCCEEDED && entSz > 0)
        {
            if (ent.d_name[0] == '.' && (ent.d_name[1] == 0 ||
                (ent.d_name[1] == '.' && ent.d_name[2] == 0))) continue;
            if (!endsWithICase(ent.d_name, ".iso")) continue;

            int nlen = strLen(ent.d_name);
            if (poolOff + nlen + 1 > namePoolSize) break;
            nameOff[count] = poolOff;
            for (int i = 0; i <= nlen; i++) namePool[poolOff + i] = ent.d_name[i];
            poolOff += nlen + 1;
            count++;
        }
        cellFsClosedir(fd);
    }

    /* Phase 2: insertion sort by case-insensitive name. n <= 256, n² fine. */
    for (int i = 1; i < count; i++) {
        int k = nameOff[i];
        int j = i - 1;
        while (j >= 0 && strCmpICase(namePool + nameOff[j], namePool + k) > 0) {
            nameOff[j + 1] = nameOff[j];
            j--;
        }
        nameOff[j + 1] = k;
    }

    /* Phase 3: emit Tables and Items in sorted order.
     * Match Sony's seg_dummy_items pattern so items render properly and
     * don't hang on X-press: explore_plugin + NotifyErrorNoExecute +
     * bar_action=none. Replace with our own module once the
     * webrender_plugin Action() hook is wired. */
    int itemsOff = 0;
    for (int i = 0; i < count; i++) {
        const char *name = namePool + nameOff[i];

        char key[24];
        int k = 0;
        key[k++] = 'i'; key[k++] = 's'; key[k++] = 'o'; key[k++] = '_';
        k += intToDec(i, key + k);
        key[k] = 0;

        appendStr(buf, cap, &off, "   <Table key=\"");
        appendStr(buf, cap, &off, key);
        appendStr(buf, cap, &off, "\">\n    <Pair key=\"title\"><String>");
        appendXmlEscaped(buf, cap, &off, name);
        appendStr(buf, cap, &off,
            "</String></Pair>\n"
            "    <Pair key=\"icon_rsc\"><String>tex_disc</String></Pair>\n"
            "    <Pair key=\"module_name\"><String>explore_plugin</String></Pair>\n"
            "    <Pair key=\"module_action\"><String>NotifyErrorNoExecute</String></Pair>\n"
            "    <Pair key=\"bar_action\"><String>none</String></Pair>\n"
            "   </Table>\n");

        appendStr(itemsBuffer, itemsSize, &itemsOff,
            "   <Item class=\"type:x-xmb/module-action\" key=\"");
        appendStr(itemsBuffer, itemsSize, &itemsOff, key);
        appendStr(itemsBuffer, itemsSize, &itemsOff, "\" attr=\"");
        appendStr(itemsBuffer, itemsSize, &itemsOff, key);
        appendStr(itemsBuffer, itemsSize, &itemsOff, "\"/>\n");
    }

    if (count == 0) {
        appendStr(buf, cap, &off,
            "   <Table key=\"iso_none\">\n"
            "    <Pair key=\"title\"><String>(no .iso files in /dev_hdd0/PS3ISO)</String></Pair>\n"
            "    <Pair key=\"icon_rsc\"><String>tex_disc</String></Pair>\n"
            "    <Pair key=\"module_name\"><String>explore_plugin</String></Pair>\n"
            "    <Pair key=\"module_action\"><String>NotifyErrorNoExecute</String></Pair>\n"
            "    <Pair key=\"bar_action\"><String>none</String></Pair>\n"
            "   </Table>\n");
        appendStr(itemsBuffer, itemsSize, &itemsOff,
            "   <Item class=\"type:x-xmb/module-action\" key=\"iso_none\" attr=\"iso_none\"/>\n");
    }

    if (itemsOff >= itemsSize - 1) return -1;
    itemsBuffer[itemsOff] = '\0';

    appendStr(buf, cap, &off, "  </Attributes>\n  <Items>\n");
    appendStr(buf, cap, &off, itemsBuffer);
    appendStr(buf, cap, &off, "  </Items>\n </View>\n</XMBML>\n");

    if (off >= cap - 1) return -1;
    buf[off] = '\0';
    return off;
}

static int writeSdmXml(void)
{
    int n = buildSdmXml(buffer, bufferSize);
    if (n < 0) { dbgLog("[sdm] build sdm.xml overflow\n"); return -1; }
    if (writeFile(pathSdmXml, buffer, (uint64_t)n) != 0) {
        dbgLog("[sdm] write sdm.xml failed\n");
        return -1;
    }
    return 0;
}

/* --- category_game.xml patching --- */

/* Splice injectLine after the Query element containing injectAnchor, inside
 * the View whose opener is viewOpen. Returns new length. Both "view not
 * found" and "anchor not found in this view" are treated as no-ops (returns
 * curLen unchanged). Only structural/overflow errors return -1.
 *
 * EVILNAT 4.75's category_game.xml uses multi-line Query elements, e.g.:
 *     <Query
 *         class="..."
 *         key="seg_package_files"
 *         src="#seg_package_files"
 *         />
 * so the anchor (key="...") is NOT on the closing-tag line. We must scan
 * forward from the anchor to the next "/>" (the element terminator), then
 * past the following newline, before splicing. */
static int spliceAfterAnchor(const char *viewOpen, int curLen)
{
    int vLen   = strLen(viewOpen);
    int aLen   = strLen(injectAnchor);
    int injLen = strLen(injectLine);

    int vPos = findBytes(buffer, curLen, viewOpen, vLen);
    if (vPos < 0) return curLen;

    /* Bound the anchor search inside this View. XMBML does not nest <View>
     * inside <View>, so the first </View> after the opener terminates it. */
    int afterOpen = vPos + vLen;
    int closeRel = findBytes(buffer + afterOpen, curLen - afterOpen, "</View>", 7);
    if (closeRel < 0) return curLen;
    int vEnd = afterOpen + closeRel;

    int aRel = findBytes(buffer + afterOpen, vEnd - afterOpen, injectAnchor, aLen);
    if (aRel < 0) return curLen;  /* Anchor not in this view; skip. */
    int aPos = afterOpen + aRel;

    /* Find "/>" — end of the current Query element — after the anchor. */
    int qCloseRel = findBytes(buffer + aPos, vEnd - aPos, "/>", 2);
    if (qCloseRel < 0) return -1;
    int insertAt = aPos + qCloseRel + 2;

    /* Skip trailing whitespace/newline so the spliced line lands on its own row. */
    while (insertAt < vEnd &&
           (buffer[insertAt] == ' ' || buffer[insertAt] == '\t'))
        insertAt++;
    if (insertAt < vEnd && buffer[insertAt] == '\r') insertAt++;
    if (insertAt < vEnd && buffer[insertAt] == '\n') insertAt++;

    if (curLen + injLen >= bufferSize - 1) return -1;
    for (int i = curLen; i >= insertAt; i--) buffer[i + injLen] = buffer[i];
    for (int i = 0; i < injLen; i++)  buffer[insertAt + i] = injectLine[i];
    return curLen + injLen;
}

static int patchCategoryGameXml(void)
{
    int len = readFile(pathCatFlash, buffer, bufferSize);
    if (len <= 0) { dbgLog("[sdm] read category_game.xml failed\n"); return PATCH_FAILED; }

    /* Already up to date? Verbatim match on the exact injectLine. */
    int injLen = strLen(injectLine);
    if (findBytes(buffer, len, injectLine, injLen) >= 0) {
        dbgLog("[sdm] category_game.xml already up to date\n");
        return PATCH_EXISTS;
    }

    /* One-time backup of the pristine file. */
    if (!fileExists(pathCatBackup))
        writeFile(pathCatBackup, buffer, (uint64_t)len);

    int n = spliceAfterAnchor("<View id=\"root\">", len);
    if (n < 0) { dbgLog("[sdm] splice root failed\n"); return PATCH_FAILED; }

    if (writeFile(pathCatBlind, buffer, (uint64_t)n) != 0) {
        dbgLog("[sdm] write /dev_blind failed\n");
        return PATCH_FAILED;
    }
    dbgLog("[sdm] category_game.xml patched\n");
    return PATCH_APPLIED;
}
