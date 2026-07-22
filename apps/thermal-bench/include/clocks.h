#pragma once

// clocks - reads and changes the graphics chip's core and memory clocks live,
// no reboot. the mechanism is a hypervisor register holding a multiplier:
// core MHz = multiplier * 50, memory MHz = multiplier * 25. Taken from the
// working webMAN-MOD implementation (include/feat/clock.h), which credits
// Chattrapat Sangmanee.
//
// This writes below the operating system. A silly value can hang the console,
// so the steps are fixed and the range is clamped.
//
// A clock change is global and outlives the app: the XMB and every game
// launched afterwards run at whatever was left here, until the console is
// rebooted. That is the point of the tool, so nothing is put back on exit -
// but the clocks the app started with are remembered so the screen can show
// them and the safety cutoff can put them back.

// remembers the clocks the console booted with. call once at startup.
void initClocks(void);

int getRsxCoreClockMhz(void);
int getRsxMemoryClockMhz(void);

int getBootRsxCoreClockMhz(void);
int getBootRsxMemoryClockMhz(void);

// puts both clocks back to what they were at startup. the memory clock ramps
// one step at a time, so this can take a second - it is only used when the
// safety cutoff fires.
void restoreBootRsxClocks(void);

// one step up or down (direction +1 / -1). returns the new value in MHz, or 0
// if the clock could not be read (no cfw support). memory moves in single
// steps on purpose - it must not jump.
int stepRsxCoreClock(int direction);
int stepRsxMemoryClock(int direction);
