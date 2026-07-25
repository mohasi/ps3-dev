#pragma once

// virtual controller. registers a fake pad with lv2 and feeds it button
// frames so a host can drive the xmb - and a running game - over the bridge.
//
//   pad <button>[+<button>...] [holdMs]   press, wait holdMs (default 80), release
//   pad hold <button>[+<button>...]       press and keep held
//   pad release                           release every button, pad stays registered
//   pad off                               unregister the fake pad
//
// mechanism follows webMAN-MOD's vpad.h: syscall 574 registers an ldd
// ("logical debug device") controller, syscall 573 switches it to insert
// mode so presses reach a running game and not only the xmb, then the
// sys_io export pushes one button frame at a time.

#include <cell/pad/pad_codes.h>

#include "dbg.h"
#include "thread.h"
#include "syscall.h"
#include "string-utilities.h"
#include "cmd-common.h"

// lv2 debug-pad syscalls (psdevwiki: sys_io_pad_dbg_ldd_*)
enum {
   SYSCALL_PAD_SET_INSERT_MODE   = 573,
   SYSCALL_PAD_REGISTER_LDD      = 574
};

enum {
   PAD_REGISTER_DATA_SIZE = 0x114,  // lv2 fills this scratch during registration
   PAD_DEFAULT_HOLD_MS    = 80,     // one comfortable press for the xmb
   PAD_MAX_HOLD_MS        = 5000,
   PAD_NAME_MAX           = 32,
   PAD_NO_PRESSURE        = 0xFF    // marker: button has no pressure byte
};

// sys_io exports - resolvable only inside vsh.self, linked via libsys_io_export_stub.a
extern int32_t sys_io_BAFD6409(int32_t handle, CellPadData *data);  // cellPadLddDataInsert
extern int32_t sys_io_E442FAA8(int32_t handle);                     // cellPadLddUnregisterController
extern int32_t sys_io_8B8231E5(int32_t handle);                     // cellPadLddGetPortNo
extern int32_t sys_io_578E3C98(uint32_t portNumber, uint32_t setting);  // cellPadSetPortSetting

typedef struct {
   const char *name;
   uint8_t     digitalOffset;   // index into CellPadData.button holding the bit
   uint16_t    mask;
   uint8_t     pressureOffset;  // analogue "how hard", or PAD_NO_PRESSURE
} PadButton;

static const PadButton padButtons[] = {
   { "up",       CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_UP,       CELL_PAD_BTN_OFFSET_PRESS_UP       },
   { "down",     CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_DOWN,     CELL_PAD_BTN_OFFSET_PRESS_DOWN     },
   { "left",     CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_LEFT,     CELL_PAD_BTN_OFFSET_PRESS_LEFT     },
   { "right",    CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_RIGHT,    CELL_PAD_BTN_OFFSET_PRESS_RIGHT    },
   { "start",    CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_START,    PAD_NO_PRESSURE                    },
   { "select",   CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_SELECT,   PAD_NO_PRESSURE                    },
   { "l3",       CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_L3,       PAD_NO_PRESSURE                    },
   { "r3",       CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_R3,       PAD_NO_PRESSURE                    },
   { "cross",    CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CROSS,    CELL_PAD_BTN_OFFSET_PRESS_CROSS    },
   { "circle",   CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CIRCLE,   CELL_PAD_BTN_OFFSET_PRESS_CIRCLE   },
   { "square",   CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_SQUARE,   CELL_PAD_BTN_OFFSET_PRESS_SQUARE   },
   { "triangle", CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_TRIANGLE, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE },
   { "l1",       CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L1,       CELL_PAD_BTN_OFFSET_PRESS_L1       },
   { "l2",       CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L2,       CELL_PAD_BTN_OFFSET_PRESS_L2       },
   { "r1",       CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R1,       CELL_PAD_BTN_OFFSET_PRESS_R1       },
   { "r2",       CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R2,       CELL_PAD_BTN_OFFSET_PRESS_R2       },
   { "ps",       0,                            CELL_PAD_CTRL_LDD_PS,   PAD_NO_PRESSURE                    }
};

static int32_t virtualPadHandle = -1;

// a frame with sticks centred and no button held - what "nothing pressed"
// looks like on the wire. every press starts from this.
static void clearPadFrame(CellPadData *frame)
{
   memSet(frame, 0, sizeof *frame);
   frame->len = CELL_PAD_MAX_CODES;
   frame->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = 0x80;
   frame->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = 0x80;
   frame->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = 0x80;
   frame->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = 0x80;
   frame->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 0x200;
   frame->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 0x200;
   frame->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 0x200;
   frame->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 0x200;
}

// register the fake pad with lv2 and make its presses reach games too.
// every step logs its own result so a failure names itself.
static int openVirtualPad(void)
{
   if (virtualPadHandle >= 0) return 0;

   // register
   uint8_t registerScratch[PAD_REGISTER_DATA_SIZE];
   int32_t handle = -1;
   const uint32_t capability = 0xFFFF;   // conformity + press + analog stick + actuator
   scCall4(SYSCALL_PAD_REGISTER_LDD, (uint64_t)(uintptr_t)registerScratch,
           (uint64_t)(uintptr_t)&handle, 5, (uint64_t)capability << 1);
   sleepMs(500);   // lv2 needs a moment before the new pad is usable
   if (handle < 0) {
      logError("[sdb] virtual pad register failed, handle=%d\n", (int)handle);
      return -1;
   }
   virtualPadHandle = handle;

   // let presses through to a running game, not just the xmb. lv2 wants the
   // mode by address here - passing it by value (as webMAN's vpad.h does)
   // comes back 0x8001000d, a bad-address error.
   uint32_t insertMode = 1;
   int32_t insertRc = (int32_t)scCall4(SYSCALL_PAD_SET_INSERT_MODE, (uint64_t)handle, 0x100,
                                       (uint64_t)(uintptr_t)&insertMode, 4);
   if (insertRc != 0) logWarn("[sdb] virtual pad insert mode rc=0x%x (xmb only)\n", insertRc);

   // pressure + motion reporting, so games that read them see sane values
   int32_t port = sys_io_8B8231E5(handle);
   if (port < 0) {
      logWarn("[sdb] virtual pad port lookup rc=%d\n", (int)port);
   } else {
      int32_t settingRc = sys_io_578E3C98((uint32_t)port, CELL_PAD_SETTING_PRESS_ON | CELL_PAD_SETTING_SENSOR_ON);
      if (settingRc != 0) logWarn("[sdb] virtual pad port setting rc=0x%x\n", settingRc);
   }

   logInfo("[sdb] virtual pad registered, handle=%d port=%d\n", (int)handle, (int)port);
   return 0;
}

static void closeVirtualPad(void)
{
   if (virtualPadHandle < 0) return;
   int32_t rc = sys_io_E442FAA8(virtualPadHandle);
   logInfo("[sdb] virtual pad unregistered, rc=0x%x\n", rc);
   virtualPadHandle = -1;
}

static int sendPadFrame(const CellPadData *frame)
{
   int32_t rc = sys_io_BAFD6409(virtualPadHandle, (CellPadData *)frame);
   if (rc != 0) logError("[sdb] pad insert rc=0x%x\n", rc);
   return rc == 0 ? 0 : -1;
}

static const PadButton *findPadButton(const char *name, int length)
{
   for (unsigned i = 0; i < sizeof padButtons / sizeof padButtons[0]; i++) {
      if (getStrLen(padButtons[i].name) != length) continue;
      int same = 1;
      for (int c = 0; c < length; c++) {
         if (padButtons[i].name[c] != name[c]) { same = 0; break; }
      }
      if (same) return &padButtons[i];
   }
   return 0;
}

// "cross+start" -> set those bits in frame. stops at the first space (the
// optional hold time follows there). returns the unknown name, or 0 on
// success. the returned name lives in a shared buffer, which is safe because
// the server runs one command at a time under serverHostLock.
static const char *setPadButtons(CellPadData *frame, const char *names)
{
   static char unknown[PAD_NAME_MAX];
   while (*names && *names != ' ') {
      int length = 0;
      while (names[length] && names[length] != '+' && names[length] != ' ') length++;

      const PadButton *match = findPadButton(names, length);
      if (!match) {
         int take = length < PAD_NAME_MAX - 1 ? length : PAD_NAME_MAX - 1;
         memCopy(unknown, names, take);
         unknown[take] = '\0';
         return unknown;
      }

      frame->button[match->digitalOffset] |= match->mask;
      if (match->pressureOffset != PAD_NO_PRESSURE) frame->button[match->pressureOffset] = 0xFF;

      names += length;
      while (*names == '+') names++;
   }
   return 0;
}

static void cmdPad(int cli, const char *args)
{
   char reply[128];

   if (matchCommand(args, "off")) {
      closeVirtualPad();
      sendReply(cli, SDB_OK, "pad off");
      return;
   }
   if (openVirtualPad() < 0) {
      sendReply(cli, SDB_ERR, "could not register virtual pad");
      return;
   }

   CellPadData frame;
   clearPadFrame(&frame);

   if (matchCommand(args, "release")) {
      sendPadFrame(&frame);
      sendReply(cli, SDB_OK, "released");
      return;
   }

   // "hold <buttons>" keeps them down; plain "<buttons> [ms]" presses and lets go
   int isHold = 0;
   const char *buttons = matchCommand(args, "hold");
   if (buttons) isHold = 1; else buttons = args;
   if (!*buttons) {
      sendReply(cli, SDB_ERR, "usage: pad <button>[+<button>...] [holdMs] | pad hold <buttons> | pad release | pad off");
      return;
   }

   // which buttons
   const char *unknown = setPadButtons(&frame, buttons);
   if (unknown) {
      snprintf(reply, sizeof reply, "unknown button: %s", unknown);
      sendReply(cli, SDB_ERR, reply);
      return;
   }

   // how long, if the host said
   char pressed[PAD_NAME_MAX];
   const char *tail = buttons;
   while (*tail && *tail != ' ') tail++;
   int take = (int)(tail - buttons);
   if (take > PAD_NAME_MAX - 1) take = PAD_NAME_MAX - 1;
   memCopy(pressed, buttons, take);
   pressed[take] = '\0';

   uint64_t holdMs = PAD_DEFAULT_HOLD_MS;
   while (*tail == ' ') tail++;
   if (*tail && parseUInt64(tail, &holdMs) == 0) holdMs = PAD_DEFAULT_HOLD_MS;
   if (holdMs > PAD_MAX_HOLD_MS) holdMs = PAD_MAX_HOLD_MS;

   // press, and let go again unless the host asked to keep it held
   if (sendPadFrame(&frame) < 0) {
      sendReply(cli, SDB_ERR, "pad insert failed");
      return;
   }
   if (isHold) {
      sendReply(cli, SDB_OK, "held");
      return;
   }
   sleepMs((unsigned)holdMs);
   clearPadFrame(&frame);
   sendPadFrame(&frame);

   snprintf(reply, sizeof reply, "pressed %s for %ums", pressed, (unsigned)holdMs);
   sendReply(cli, SDB_OK, reply);
}
