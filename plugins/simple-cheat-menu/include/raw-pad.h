#pragma once

// raw kernel pad reads via sys_hid_manager_read (syscall 502), below libpad's
// consumption cursor — the only pad source that keeps working once input
// capture stops vsh's libpad (start_stop_vsh_pad(0) blinds cellPadGetData).
//
// SAFETY (hard-lock class, learned 2026-07-08): never poll this across a pad
// ownership transition (game launch, ps-menu close). Callers read ONLY while
// the ps menu is up (vsh owns the pad) and must stop before it closes.

#include <stdint.h>

// resolve vsh's io_pad_object (toc scan + export walk). call once, on the
// menu thread, after xmb is ready. 0 = ok, negative = unusable (logged why).
int initRawPad(void);

// one raw read of pad port 0. buttons packed like readPad: low byte word
// digital1 (SELECT/L3/R3/START/dpad), high byte word digital2 (shoulders,
// faces), plus the PS button as 0x100. returns the report length (0 = no
// new data) or a negative error.
int readRawPad(uint32_t *buttonsOut);

// input capture: 0 blinds vsh's libpad (every cellPadGetData in vsh returns
// CELL_PAD_ERROR_UNINITIALIZED, so the xmb stops reacting; raw reads keep
// working), 1 restores it. webMAN's vsh_menu mechanism: the byte at
// io_pad_object is libpad's init flag. EVERY capture path must restore —
// a leftover 0 leaves the console without input until vsh restarts.
void setVshPadEnabled(int enabled);
