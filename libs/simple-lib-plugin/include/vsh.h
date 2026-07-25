#pragma once

// vsh-only nid imports. these are only resolvable when the prx is loaded
// into vsh.self (i.e. a vsh plugin). non-vsh code should not include this
// header — use syscall.h for generic lv2 wrappers instead.
//
// link requirements (per-symbol):
//   vshtask_*  -> libvshtask_export_stub.a
//   vshmain_*  -> libvshmain_export_stub.a
// only link the archives whose symbols you actually reference.

#include <stdint.h>

extern int32_t  vshtask_A02D46E7(int32_t, const char *);
extern uint32_t vshmain_EB757101(void);
extern uint32_t vshmain_0624D3AE(void);   // running game's process id (0 = none)

#define vshNotify(msg)         vshtask_A02D46E7(0, (msg))
#define isXmbReady()           (vshmain_EB757101() == 0)
#define getGameProcessId()     vshmain_0624D3AE()
