#pragma once

// clocks - what this app adds on top of the shared RSX clock control in
// simple-lib-core's rsx-clocks.h, which owns the registers, the multipliers,
// the safe ranges and the memory clock's settling period.
//
// A clock change is global and outlives the app: the XMB and every game
// launched afterwards run at whatever was left here, until the console is
// rebooted. That is the point of the tool, so nothing is put back on exit -
// but the clocks the app started with are remembered so the screen can show
// them and the safety cutoff can put them back.

#include "rsx-clocks.h"   // getRsxCoreClockMhz / getRsxMemoryClockMhz / stepRsx*Clock

// remembers the clocks the console booted with. call once at startup.
void initClocks(void);

int getBootRsxCoreClockMhz(void);
int getBootRsxMemoryClockMhz(void);

// puts both clocks back to what they were at startup. the memory clock ramps
// one step at a time, so this can take a second - it is only used when the
// safety cutoff fires.
void restoreBootRsxClocks(void);
