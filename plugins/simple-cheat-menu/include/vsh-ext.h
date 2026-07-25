#pragma once

// extra vsh-only nid imports beyond simple-lib-plugin/vsh.h. resolvable
// only inside vsh.self (i.e. a vsh plugin). link libvshmain_export_stub.a.

#include <stdint.h>

extern int32_t  sys_io_3733EA3C(void);    // vsh cellPadGetData export; its OPD leads to io_pad_object
extern void     vshmain_5C3E01A1(void);   // end the in-game XMB (ps menu), resume the game

#define endInGameXmb()      vshmain_5C3E01A1()
