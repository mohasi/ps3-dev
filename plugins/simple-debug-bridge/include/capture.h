#pragma once

// framebuffer capture primitives for simple-debug-bridge.
//
// xmb-only by construction: every piece below is tied to vsh's compositor.
//   - paf_* are vsh-resident display getters (resolved from libpaf_export_stub
//     and only present inside vsh.self).
//   - CAP_FRONTBUF_OFF_ADDR reads vsh's *current* front-buffer offset within
//     rsx local memory; a game runs in its own rsx context with its own
//     buffers, so this address is meaningless when xmb isn't compositing.
//   - rsxFifoPause() drives the *global* rsx fifo via syscall 674. pausing
//     it while a game owns rsx wedges the gpu.
// callers must gate on isXmbReady() — cmdCapture() does.
//
// single primitive: captureRegion(x, y, w, h, sink). 1x1 = single pixel.
// pitch-padded rows are repacked into width*4 ARGB and streamed out one
// row at a time so no large buffer is needed; the rsx fifo stays paused
// for the whole capture (same pattern as webman saveBMP).
//
// references:
//   webman-mod include/feat/xmb_savebmp.h  (fifo pause + framebuffer layout)
//   webman-mod vsh/paf.h                   (paf display-getter nids)
//
// link requirements: libpaf_export_stub.a (paf_*).

#include <stdint.h>
#include "syscall.h"

// rsx local-memory base address (mapped read-only into vsh on cobra/evilnat).
// the current front-buffer offset within local memory lives at 0x60201104.
// reference: webman-mod include/feat/xmb_savebmp.h, BASE/OFFSET macros.
#define CAP_RSX_BASE          0xC0000000UL
#define CAP_FRONTBUF_OFF_ADDR 0x60201104UL

// lv2 syscall 674 (sys_rsx_context_attribute). passing 0x55555555 as the
// context id with arg2=2 pauses the rsx fifo, arg2=3 resumes it. confirmed
// by webman-mod's rsx_fifo_pause(). MUST be balanced: a pause without a
// resume will hang the gpu and eventually freeze the system.
static inline void rsxFifoPause(int pause)
{
    (void)scCall6(0x2A2, 0x55555555ULL, (uint64_t)(pause ? 2 : 3), 0, 0, 0, 0);
}

// paf display getters (vsh-only). resolved by the linker via libpaf_export_stub.
extern uint32_t paf_F476E8AA(void);                                  // display width
extern uint32_t paf_AC984A12(void);                                  // display height
extern int32_t  paf_FFE0FBC9(uint32_t *pitchBytes, uint32_t *depth); // pitch + format tag

// query the current display geometry. fills width, height, pitch (bytes per
// row in the framebuffer - typically padded beyond width*4), and depth
// (color format tag, 0x12 = argb8888). always succeeds.
static inline void captureDisplayInfo(uint32_t *outW, uint32_t *outH,
                                      uint32_t *outPitch, uint32_t *outDepth)
{
    *outW = paf_F476E8AA();
    *outH = paf_AC984A12();
    paf_FFE0FBC9(outPitch, outDepth);
}

// resolve the address of pixel (x,y) in current front buffer's local memory.
static inline volatile uint32_t *captureFramebufferPixel(uint32_t x, uint32_t y, uint32_t pitchBytes)
{
    uint32_t bufOff = *(volatile uint32_t *)(uintptr_t)CAP_FRONTBUF_OFF_ADDR;
    uintptr_t addr  = (uintptr_t)CAP_RSX_BASE + bufOff + (uintptr_t)y * pitchBytes + (uintptr_t)x * 4;
    return (volatile uint32_t *)addr;
}

// per-row sink callback. return < 0 to abort the capture (fifo is resumed
// before the function returns). row is width*4 bytes of ARGB8888.
typedef int (*CaptureRowSink)(const void *row, uint32_t bytes, void *user);

// capture an arbitrary region of the current front buffer. clipped to the
// display bounds; out-of-bounds origin or zero-area returns -1 without
// touching vram. rsx fifo is paused for the whole copy, then resumed even
// on sink error. returns 0 on success, -1 on bad args, -2 on sink abort.
static inline int captureRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                CaptureRowSink sink, void *user)
{
    uint32_t dw, dh, pitch, depth;
    captureDisplayInfo(&dw, &dh, &pitch, &depth);
    if (w == 0 || h == 0 || x >= dw || y >= dh) return -1;
    if (x + w > dw) w = dw - x;
    if (y + h > dh) h = dh - y;

    uint32_t rowBytes = w * 4;
    int rc = 0;

    rsxFifoPause(1);
    for (uint32_t r = 0; r < h; r++) {
        volatile uint32_t *src = captureFramebufferPixel(x, y + r, pitch);
        if (sink((const void *)src, rowBytes, user) < 0) { rc = -2; break; }
    }
    rsxFifoPause(0);
    return rc;
}
