#pragma once

// screen-capture command: stream a w*h*4 ARGB8888 region of XMB vram
// to the host. only valid while XMB owns rsx (no game/app foreground);
// otherwise we'd wedge the gpu by pausing a fifo we don't own.

#include "cmd-common.h"
#include "capture.h"

// row sink: write straight to the client socket.
static int captureSendRow(const void *row, uint32_t bytes, void *user)
{
    return sendBytes(*(int *)user, row, (int)bytes);
}

// capture <x> <y> <w> <h>
//   streams w*h*4 bytes of ARGB8888 (vram byte order) after the OK header.
//   1x1 covers the single-pixel case. region is clipped to display bounds.
//   refused while a game/app owns rsx - pausing the fifo then would wedge
//   the gpu (we only touch vram while xmb is the foreground compositor).
static void cmdCapture(int cli, const char *args)
{
    uint64_t x = 0, y = 0, w = 0, h = 0;
    const char *r = parseUInt64(args, &x);
    if (r) r = parseUInt64(r, &y);
    if (r) r = parseUInt64(r, &w);
    if (r) r = parseUInt64(r, &h);
    if (!r) { sendReply(cli, SDB_ERR, "usage: capture <x> <y> <w> <h>"); return; }

    if (!isXmbReady()) { sendReply(cli, SDB_ERR, "capture: xmb not in foreground"); return; }

    uint32_t dw, dh, pitch, depth;
    captureDisplayInfo(&dw, &dh, &pitch, &depth);
    if (w == 0 || h == 0 || x >= dw || y >= dh) {
        sendReply(cli, SDB_ERR, "out of bounds"); return;
    }
    uint32_t cw = (x + w > dw) ? (dw - (uint32_t)x) : (uint32_t)w;
    uint32_t ch = (y + h > dh) ? (dh - (uint32_t)y) : (uint32_t)h;

    if (sendFrameHeader(cli, SDB_OK, cw * ch * 4) < 0) return;
    captureRegion((uint32_t)x, (uint32_t)y, cw, ch, captureSendRow, &cli);
}
