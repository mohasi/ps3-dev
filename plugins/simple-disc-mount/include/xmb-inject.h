#pragma once

// Generates sdm.xml and patches category_game.xml to add "Mount Disc Image"
// below "Package Manager" in the XMB Games column. Requires Cobra (EVILNAT).

#include "dbg.h"
#include "vsh.h"
#include "vfs.h"
#include "syscall.h"   // sysMemAllocate for the menu scratch
#include "string-utilities.h"

// Paths
static const char *pathIsoDir      = "/dev_hdd0/PS3ISO";
static const char *pathXmlHostRoot = "/dev_hdd0/xmlhost";
static const char *pathXmlHostGp   = "/dev_hdd0/xmlhost/game_plugin";
static const char *pathSdmXml      = "/dev_hdd0/xmlhost/game_plugin/sdm.xml";
static const char *pathCatFlash    = "/dev_flash/vsh/resource/explore/xmb/category_game.xml";
static const char *pathCatBlind    = "/dev_blind/vsh/resource/explore/xmb/category_game.xml";
static const char *pathCatBackup   = "/dev_hdd0/tmp/sdm_category_game.xml.bak";

// category_game.xml inject: sits directly after the seg_package_files
// ("Package Manager") Query in <View id="root">, indented with 3 tabs to
// match the surrounding entries. On EVILNAT 4.75 the rendered Games column
// is <View id="root"> (webMAN's xmb_app3 entry lives there, and we see it),
// NOT root_for_BDU. seg_pkg_install is a different item that only exists in
// root_for_BDU — it would never render for us.
static const char *injectAnchor = "key=\"seg_package_files\"";
static const char *injectLine =
   "\t\t\t<Query class=\"type:x-xmb/folder-pixmap\" key=\"seg_sdm\" "
   "attr=\"seg_sdm\" src=\"xmb://localhost/dev_hdd0/xmlhost/game_plugin/sdm.xml#seg_sdm\"/>\n";

// Working buffers. xmlBuf holds either the generated sdm.xml or the in-flight
// category_game.xml patch (phases never overlap). itemsBuf collects the
// <Item/> lines while <Table> blocks stream into xmlBuf, since Tables must
// precede Items inside a View. namePool holds a copy of each ISO filename so
// we can sort them before emitting — cellFsReaddir returns entries in
// filesystem (insertion) order, not alphabetical.
//
// A quarter of a megabyte cannot sit in a vsh plugin's fixed memory, and all of
// it is dead once the menu is written, so it lives in one on-demand allocation
// held only for the duration of that work. The three sizes sum to exactly the
// 64KB-aligned total lv2 requires.
enum {
   XML_BUF_SIZE    = 160 * 1024,
   ITEMS_BUF_SIZE  =  32 * 1024,
   NAME_POOL_SIZE  =  64 * 1024,  // 256 names × ~256B worst case
   SCRATCH_SIZE    = XML_BUF_SIZE + ITEMS_BUF_SIZE + NAME_POOL_SIZE,
   MAX_ISOS        = 256,
};
static char *xmlBuf;
static char *itemsBuf;
static char *namePool;
static uint32_t scratchAddr;
static int  nameOff[MAX_ISOS];   // 1KB, small enough to keep resident

static int openXmlScratch(void)
{
   if (sysMemAllocate(SCRATCH_SIZE, SYS_PAGE_64K, &scratchAddr) != 0 || scratchAddr == 0) {
      logError("[sdm] could not allocate %dKB of menu scratch\n", SCRATCH_SIZE / 1024);
      return -1;
   }

   xmlBuf   = (char *)(uintptr_t)scratchAddr;
   itemsBuf = xmlBuf + XML_BUF_SIZE;
   namePool = itemsBuf + ITEMS_BUF_SIZE;
   return 0;
}

static void closeXmlScratch(void)
{
   sysMemFree(scratchAddr);
   scratchAddr = 0;
   xmlBuf = itemsBuf = namePool = 0;
}

// patchCategoryGameXml return codes
#define PATCH_APPLIED   1
#define PATCH_EXISTS    2
#define PATCH_FAILED   -1

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

   // Phase 1: collect ISO filenames into namePool. readDir returns entries in
   // filesystem order (creation time), not alphabetical, so we can't emit
   // during the walk if we want a sorted menu. Routed through the VFS like every
   // other filesystem access ("." / ".." are already filtered by the backend).
   int poolOff = 0;
   int count = 0;
   int dropped = 0;
   VfsDir dir;
   if (openDir(pathIsoDir, &dir) == 0) {
      char entName[256];
      while (readDir(&dir, entName, sizeof entName, NULL) == 1)
      {
         if (!endsWithICase(entName, ".iso")) continue;

         int nlen = getStrLen(entName);
         if (count >= MAX_ISOS || poolOff + nlen + 1 > NAME_POOL_SIZE) { dropped++; continue; }

         nameOff[count] = poolOff;
         for (int i = 0; i <= nlen; i++) namePool[poolOff + i] = entName[i];
         poolOff += nlen + 1;
         count++;
      }
      closeDir(&dir);
   }
   if (dropped > 0) logWarn("[sdm] menu is full, %d iso(s) left out\n", dropped);

   // Phase 2: insertion sort by case-insensitive name. n <= 256, n² fine.
   for (int i = 1; i < count; i++) {
      int k = nameOff[i];
      int j = i - 1;
      while (j >= 0 && strCmpICase(namePool + nameOff[j], namePool + k) > 0) {
         nameOff[j + 1] = nameOff[j];
         j--;
      }
      nameOff[j + 1] = k;
   }

   // Phase 3: emit Tables and Items in sorted order.
   //
   // Each ISO becomes a module-action that, when X is pressed, wakes Sony's
   // built-in webrender_plugin with the configured URL. webrender fires an
   // HTTP GET at 127.0.0.1:8947 — our http.h listener catches that, parses
   // the filename out of the path, and calls cobraMountIso(). Filename
   // travels in the URL (percent-encoded) so the handler is stateless and
   // the last mount can be persisted as a plain filename for auto-mount on
   // reboot. Port 8947 is derived from fnv1a32("simple-disc-mount") — see
   // http.h for the formula.
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
          "    <Pair key=\"module_name\"><String>webrender_plugin</String></Pair>\n"
          "    <Pair key=\"module_action\"><String>http://0:8947/mount/");
      appendUrlEnc(buf, cap, &off, pathIsoDir);
      appendUrlEnc(buf, cap, &off, "/");
      appendUrlEnc(buf, cap, &off, name);
      appendStr(buf, cap, &off,
          "</String></Pair>\n"
          "   </Table>\n");

      appendStr(itemsBuf, ITEMS_BUF_SIZE, &itemsOff,
          "   <Item class=\"type:x-xmb/module-action\" key=\"");
      appendStr(itemsBuf, ITEMS_BUF_SIZE, &itemsOff, key);
      appendStr(itemsBuf, ITEMS_BUF_SIZE, &itemsOff, "\" attr=\"");
      appendStr(itemsBuf, ITEMS_BUF_SIZE, &itemsOff, key);
      appendStr(itemsBuf, ITEMS_BUF_SIZE, &itemsOff, "\"/>\n");
   }

   if (count == 0) {
      // Empty-state item: keep Sony's stock "do nothing" pattern so X-press
      // shows "Cannot operate" instead of trying to fire an HTTP action we
      // don't want to handle.
      appendStr(buf, cap, &off,
          "   <Table key=\"iso_none\">\n"
          "    <Pair key=\"title\"><String>(no .iso files in /dev_hdd0/PS3ISO)</String></Pair>\n"
          "    <Pair key=\"icon_rsc\"><String>tex_disc</String></Pair>\n"
          "    <Pair key=\"module_name\"><String>explore_plugin</String></Pair>\n"
          "    <Pair key=\"module_action\"><String>NotifyErrorNoExecute</String></Pair>\n"
          "    <Pair key=\"bar_action\"><String>none</String></Pair>\n"
          "   </Table>\n");
      appendStr(itemsBuf, ITEMS_BUF_SIZE, &itemsOff,
          "   <Item class=\"type:x-xmb/module-action\" key=\"iso_none\" attr=\"iso_none\"/>\n");
   }

   if (itemsOff >= ITEMS_BUF_SIZE - 1) return -1;
   itemsBuf[itemsOff] = '\0';

   appendStr(buf, cap, &off, "  </Attributes>\n  <Items>\n");
   appendStr(buf, cap, &off, itemsBuf);
   appendStr(buf, cap, &off, "  </Items>\n </View>\n</XMBML>\n");

   if (off >= cap - 1) return -1;
   buf[off] = '\0';
   return off;
}

static int writeSdmXml(void)
{
   int n = buildSdmXml(xmlBuf, XML_BUF_SIZE);
   if (n < 0) { logError("[sdm] build sdm.xml overflow\n"); return -1; }
   if (writeFile(pathSdmXml, xmlBuf, (uint64_t)n) != 0) {
      logError("[sdm] write sdm.xml failed\n");
      return -1;
   }
   return 0;
}

// Splice injectLine after the Query element containing injectAnchor, inside
// the View whose opener is viewOpen. Returns new length. Both "view not
// found" and "anchor not found in this view" are treated as no-ops (returns
// curLen unchanged). Only structural/overflow errors return -1.
//
// EVILNAT 4.75's category_game.xml uses multi-line Query elements, e.g.:
//     <Query
//         class="..."
//         key="seg_package_files"
//         src="#seg_package_files"
//         />
// so the anchor (key="...") is NOT on the closing-tag line. We must scan
// forward from the anchor to the next "/>" (the element terminator), then
// past the following newline, before splicing.
static int spliceAfterAnchor(const char *viewOpen, int curLen)
{
   int vLen   = getStrLen(viewOpen);
   int aLen   = getStrLen(injectAnchor);
   int injLen = getStrLen(injectLine);

   int vPos = findBytes(xmlBuf, curLen, viewOpen, vLen);
   if (vPos < 0) return curLen;

   // Bound the anchor search inside this View. XMBML does not nest <View>
   // inside <View>, so the first </View> after the opener terminates it.
   int afterOpen = vPos + vLen;
   int closeRel = findBytes(xmlBuf + afterOpen, curLen - afterOpen, "</View>", 7);
   if (closeRel < 0) return curLen;
   int vEnd = afterOpen + closeRel;

   int aRel = findBytes(xmlBuf + afterOpen, vEnd - afterOpen, injectAnchor, aLen);
   if (aRel < 0) return curLen;  // Anchor not in this view; skip.
   int aPos = afterOpen + aRel;

   // Find "/>" — end of the current Query element — after the anchor.
   int qCloseRel = findBytes(xmlBuf + aPos, vEnd - aPos, "/>", 2);
   if (qCloseRel < 0) return -1;
   int insertAt = aPos + qCloseRel + 2;

   // Skip trailing whitespace/newline so the spliced line lands on its own row.
   while (insertAt < vEnd &&
          (xmlBuf[insertAt] == ' ' || xmlBuf[insertAt] == '\t'))
      insertAt++;
   if (insertAt < vEnd && xmlBuf[insertAt] == '\r') insertAt++;
   if (insertAt < vEnd && xmlBuf[insertAt] == '\n') insertAt++;

   if (curLen + injLen >= XML_BUF_SIZE - 1) return -1;
   for (int i = curLen; i >= insertAt; i--) xmlBuf[i + injLen] = xmlBuf[i];
   for (int i = 0; i < injLen; i++)  xmlBuf[insertAt + i] = injectLine[i];
   return curLen + injLen;
}

static int patchCategoryGameXml(void)
{
   int len = readFile(pathCatFlash, xmlBuf, XML_BUF_SIZE);
   if (len <= 0) { logError("[sdm] read category_game.xml failed\n"); return PATCH_FAILED; }

   // readFile cannot report truncation, so a file that exactly fills the buffer
   // may be a short read. Writing that back would put a cut-off system XML into
   // flash and take the pristine backup from the same cut-off bytes.
   if (len >= XML_BUF_SIZE - 1) {
      logError("[sdm] category_game.xml is too big for our buffer, leaving it alone\n");
      return PATCH_FAILED;
   }

   // Already up to date? Verbatim match on the exact injectLine.
   int injLen = getStrLen(injectLine);
   if (findBytes(xmlBuf, len, injectLine, injLen) >= 0) {
      logInfo("[sdm] category_game.xml already up to date\n");
      return PATCH_EXISTS;
   }

   // One-time backup of the pristine file.
   if (!fileExists(pathCatBackup))
      writeFile(pathCatBackup, xmlBuf, (uint64_t)len);

   int n = spliceAfterAnchor("<View id=\"root\">", len);
   if (n < 0) { logError("[sdm] splice root failed\n"); return PATCH_FAILED; }

   // A splice always grows the file, so an unchanged length means the View or the
   // anchor wasn't there. Writing the file back unchanged and calling it patched
   // would claim success, and re-toast "installed successfully", on every boot.
   if (n == len) {
      logError("[sdm] anchor %s not found, menu not injected\n", injectAnchor);
      return PATCH_FAILED;
   }

   if (writeFile(pathCatBlind, xmlBuf, (uint64_t)n) != 0) {
      logError("[sdm] write /dev_blind failed\n");
      return PATCH_FAILED;
   }
   logInfo("[sdm] category_game.xml patched\n");
   return PATCH_APPLIED;
}
