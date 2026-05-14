#pragma once

// vsh-only nid stub exports. these are only resolvable when the prx is
// loaded into vsh.self (i.e. a vsh plugin). non-vsh code should not
// include this header — use syscall.h for generic lv2 wrappers instead.

#include <stdint.h>

// resolved at load time by libvshtask_export_stub.a / libvshmain_export_stub.a
extern int32_t  vshtask_A02D46E7(int32_t, const char *);
extern uint32_t vshmain_EB757101(void);

#define vshNotify(msg)  vshtask_A02D46E7(0, (msg))
#define isXmbReady()    (vshmain_EB757101() == 0)
