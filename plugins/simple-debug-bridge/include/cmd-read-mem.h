#pragma once

// read-mem <hexAddr> <decLen>
//   reply: OK <len>\n<len raw bytes>  or  ERR <msg>
//   reads [addr, addr+len) from this process's address space, but ONLY
//   if the whole window lies inside a known loadable segment of some
//   loaded prx OR the main vsh executable. otherwise rejects with ERR
//   "out of range" - blind reads at random VAs hard-crash vsh.
//
// addr is hex (0x prefix optional); len is decimal, capped at READ_MEM_MAX.
// host caller is the analyzer / trace follow-up flow: a slot or arg
// pointer captured during trace-on can be resolved to its bytes here
// without needing a fresh arm pass.

#include "cmd-common.h"
#include "module-inspect.h"

#define READ_MEM_MAX  (32u * 1024u)

// hex parser; accepts optional "0x" prefix. on success fills *out, returns
// pointer to remainder past trailing spaces. on malformed input returns 0.
static const char *parseHex32(const char *args, uint32_t *out)
{
    if (args[0] == '0' && (args[1] == 'x' || args[1] == 'X')) args += 2;
    uint32_t v = 0;
    int digits = 0;
    while (*args) {
        char c = *args;
        uint32_t d;
        if      (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
        args++;
        digits++;
    }
    if (digits == 0 || digits > 8) return 0;
    *out = v;
    while (*args == ' ') args++;
    return args;
}

// returns 1 if [addr, addr+len) fits entirely inside a loadable segment
// of any loaded prx, or inside the main vsh executable mapped at 0x10000.
static int addrInLoadedImage(uint32_t addr, uint32_t len)
{
    // main exe first - low addresses (0x10000+) are by far the common case
    // for the analysis flow (e.g. vshmain callback bodies).
    PrxSegment segs[PRX_SEGMENTS_MAX];
    const volatile uint8_t *elf = (const volatile uint8_t *)(uintptr_t)0x10000;
    uint32_t segCount = loadElfSegments(elf, segs, PRX_SEGMENTS_MAX);
    if (segCount && addrInSegments(addr, len, segs, segCount)) return 1;

    uint32_t count = 0;
    if (prxList(moduleIds, MODULE_IDS_MAX, &count) < 0) return 0;
    char name[PRX_NAME_MAX];
    char file[PRX_FILENAME_MAX];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t sc = 0;
        if (prxInfo((int32_t)moduleIds[i], name, file, segs, PRX_SEGMENTS_MAX, &sc) < 0) continue;
        if (sc > PRX_SEGMENTS_MAX) sc = PRX_SEGMENTS_MAX;
        if (addrInSegments(addr, len, segs, sc)) return 1;
    }
    return 0;
}

static void cmdReadMem(int cli, const char *args)
{
    uint32_t addr = 0;
    uint64_t lenU = 0;
    const char *rest = args ? parseHex32(args, &addr) : 0;
    if (rest) rest = parseUInt64(rest, &lenU);
    if (!rest || lenU == 0) {
        sendReply(cli, SDB_ERR, "usage: read-mem <hexAddr> <decLen>");
        return;
    }
    if (lenU > READ_MEM_MAX) {
        sendReply(cli, SDB_ERR, "len too large");
        return;
    }
    uint32_t len = (uint32_t)lenU;
    if (!addrInLoadedImage(addr, len)) {
        sendReply(cli, SDB_ERR, "out of range");
        return;
    }
    if (sendFrameHeader(cli, SDB_OK, len) < 0) return;
    sendBytes(cli, (const char *)(uintptr_t)addr, (int)len);
}
