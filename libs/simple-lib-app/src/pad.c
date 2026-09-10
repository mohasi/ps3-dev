// pad - implementation
#include "pad.h"
#include "dbg.h"
#include <cell/pad.h>
#include <string.h>

#define PAD_BUTTON_COUNT 16

// which digital word (1 or 2) each button lives in, its control bit, and where its pressure is
// reported - indexed by PadButton so one table drives the edge derivation, the live down-mask and
// the pressure lookup. START, SELECT and the stick clicks have no sensor, marked 0.
static const struct { int word; uint16_t bit; int press; } padControlBits[PAD_BUTTON_COUNT] = {
   { 1, CELL_PAD_CTRL_UP,       CELL_PAD_BTN_OFFSET_PRESS_UP },
   { 1, CELL_PAD_CTRL_DOWN,     CELL_PAD_BTN_OFFSET_PRESS_DOWN },
   { 1, CELL_PAD_CTRL_LEFT,     CELL_PAD_BTN_OFFSET_PRESS_LEFT },
   { 1, CELL_PAD_CTRL_RIGHT,    CELL_PAD_BTN_OFFSET_PRESS_RIGHT },
   { 2, CELL_PAD_CTRL_CROSS,    CELL_PAD_BTN_OFFSET_PRESS_CROSS },
   { 2, CELL_PAD_CTRL_CIRCLE,   CELL_PAD_BTN_OFFSET_PRESS_CIRCLE },
   { 2, CELL_PAD_CTRL_SQUARE,   CELL_PAD_BTN_OFFSET_PRESS_SQUARE },
   { 2, CELL_PAD_CTRL_TRIANGLE, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE },
   { 2, CELL_PAD_CTRL_L1,       CELL_PAD_BTN_OFFSET_PRESS_L1 },
   { 2, CELL_PAD_CTRL_R1,       CELL_PAD_BTN_OFFSET_PRESS_R1 },
   { 2, CELL_PAD_CTRL_L2,       CELL_PAD_BTN_OFFSET_PRESS_L2 },
   { 2, CELL_PAD_CTRL_R2,       CELL_PAD_BTN_OFFSET_PRESS_R2 },
   { 1, CELL_PAD_CTRL_START,    0 },
   { 1, CELL_PAD_CTRL_SELECT,   0 },
   { 1, CELL_PAD_CTRL_L3,       0 },
   { 1, CELL_PAD_CTRL_R3,       0 },
};

// BD / TV (HDMI-CEC) remote: cellPad reports one key code at CELL_PAD_BTN_OFFSET_BD_CODE instead of the
// digital bitmasks a controller uses. map each code onto the same button bits so the remote drives the app
// through the existing pipeline. only the keys a Bravia CEC remote reliably sends are mapped: dpad for
// navigation, enter/return for confirm/back, play/pause, and fast-forward/rewind for seek.
static const struct { uint16_t code; int word; uint16_t bit; } remoteCodeBits[] = {
   { CELL_PAD_BTN_CODE_BD_UP,        1, CELL_PAD_CTRL_UP },
   { CELL_PAD_BTN_CODE_BD_DOWN,      1, CELL_PAD_CTRL_DOWN },
   { CELL_PAD_BTN_CODE_BD_LEFT,      1, CELL_PAD_CTRL_LEFT },
   { CELL_PAD_BTN_CODE_BD_RIGHT,     1, CELL_PAD_CTRL_RIGHT },
   { CELL_PAD_BTN_CODE_BD_ENTER,     2, CELL_PAD_CTRL_CROSS },    // confirm
   { CELL_PAD_BTN_CODE_BD_CROSS,     2, CELL_PAD_CTRL_CROSS },
   { CELL_PAD_BTN_CODE_BD_RETURN,    2, CELL_PAD_CTRL_CIRCLE },   // back
   { CELL_PAD_BTN_CODE_BD_CIRCLE,    2, CELL_PAD_CTRL_CIRCLE },
   { CELL_PAD_BTN_CODE_BD_PLAY,      2, CELL_PAD_CTRL_CROSS },    // cross toggles pause during playback
   { CELL_PAD_BTN_CODE_BD_PAUSE,     2, CELL_PAD_CTRL_CROSS },
   { CELL_PAD_BTN_CODE_BD_SCAN_FWD,  1, CELL_PAD_CTRL_RIGHT },    // seek: left/right nudge in playback
   { CELL_PAD_BTN_CODE_BD_SCAN_REV,  1, CELL_PAD_CTRL_LEFT },
   { CELL_PAD_BTN_CODE_BD_NEXT,      1, CELL_PAD_CTRL_RIGHT },
   { CELL_PAD_BTN_CODE_BD_PREV,      1, CELL_PAD_CTRL_LEFT },
   { CELL_PAD_BTN_CODE_BD_FLASH_FWD, 1, CELL_PAD_CTRL_RIGHT },
   { CELL_PAD_BTN_CODE_BD_FLASH_REV, 1, CELL_PAD_CTRL_LEFT },
};

#define REMOTE_CODE_NONE 0xFFFF   // no valid BD code equals this (real codes reach 0x0153)

static uint32_t lastConnectedMask = 0xFFFFFFFF;   // bring-up: force a connectivity log on the first poll
static CellPadData current;             // last standard controller data, for the digital words and sticks
static uint16_t remoteWord1, remoteWord2;   // remote key mapped into the DIGITAL1/DIGITAL2 bit layout
static uint16_t lastRemoteCode = REMOTE_CODE_NONE;   // for the bring-up log, so a held key logs once
static PadButtonState buttonStates[PAD_BUTTON_COUNT];
static Stick leftStick;
static Stick rightStick;
static volatile uint16_t rawDigital1;   // latest DIGITAL1 bits from pollPad (dpad, start, select, L3/R3)
static volatile uint16_t rawDigital2;   // latest DIGITAL2 bits from pollPad (face buttons, L1/R1/L2/R2)
static uint16_t prev1 = 0;              // DIGITAL1 bits at the previous updatePadEdges, for edge detection
static uint16_t prev2 = 0;

static PadButtonState getState(int held, int wasHeld)
{
   if (held && !wasHeld) return PAD_BUTTON_STATE_PRESSED;
   if (held && wasHeld) return PAD_BUTTON_STATE_HELD;
   if (!held && wasHeld) return PAD_BUTTON_STATE_RELEASED;
   return PAD_BUTTON_STATE_UP;
}

static int buttonHeld(int button, uint16_t digital1, uint16_t digital2)
{
   uint16_t word = padControlBits[button].word == 1 ? digital1 : digital2;
   return (word & padControlBits[button].bit) != 0;
}

// translate a fresh remote key code into remoteWord1/remoteWord2. a release (or an unmapped key) clears
// them. the log fires once per code change so a held key does not spam, and an unmapped code is visible in
// dbg.txt (CEC remotes vary by TV, so this is how we learn which codes a given remote sends).
static void setRemoteCode(uint16_t code)
{
   remoteWord1 = remoteWord2 = 0;
   int mapped = 0;
   if (code != CELL_PAD_BTN_CODE_BD_RELEASE) {
      for (int i = 0; i < (int)(sizeof remoteCodeBits / sizeof *remoteCodeBits); i++)
         if (remoteCodeBits[i].code == code) {
            if (remoteCodeBits[i].word == 1) remoteWord1 |= remoteCodeBits[i].bit;
            else                             remoteWord2 |= remoteCodeBits[i].bit;
            mapped = 1;
            break;
         }
   }
   if (code != lastRemoteCode) {
      if (code != CELL_PAD_BTN_CODE_BD_RELEASE)
         logInfo("[pad] remote code 0x%x%s\n", code, mapped ? "" : " (unmapped)");
      lastRemoteCode = code;
   }
}

void initPad(void)
{
   cellPadInit(CELL_PAD_MAX_PORT_NUM);   // enumerate every port so a remote is seen alongside a controller
   logInfo("[pad] init with remote support (%d ports)\n", CELL_PAD_MAX_PORT_NUM);
   lastConnectedMask = 0xFFFFFFFF;
   memset(&current, 0, sizeof(current));
   memset(buttonStates, 0, sizeof(buttonStates));
   memset(&leftStick, 0, sizeof(leftStick));
   memset(&rightStick, 0, sizeof(rightStick));
   remoteWord1 = remoteWord2 = 0;
   lastRemoteCode = REMOTE_CODE_NONE;
   rawDigital1 = 0;
   rawDigital2 = 0;
   prev1 = 0;
   prev2 = 0;
}

void pollPad(void)
{
   // scan every connected port: a standard controller feeds the digital words and sticks, a BD/CEC remote
   // feeds a key code. both merge into the same digital words so either device drives the app.
   CellPadInfo2 info;
   int remotePresent = 0;
   if (cellPadGetInfo2(&info) == CELL_OK) {
      // bring-up diagnostic: log which ports are connected and their device type, only when it changes,
      // so a remote appearing (type 4 = BD/CEC) is visible without spamming per frame.
      uint32_t connectedMask = 0;
      for (uint32_t port = 0; port < CELL_PAD_MAX_PORT_NUM; port++)
         if (info.port_status[port] & CELL_PAD_STATUS_CONNECTED) connectedMask |= 1u << port;
      if (connectedMask != lastConnectedMask) {
         logInfo("[pad] ports changed: now_connect=%d mask=0x%x\n", info.now_connect, connectedMask);
         for (uint32_t port = 0; port < CELL_PAD_MAX_PORT_NUM; port++) {
            if (!(connectedMask & (1u << port))) continue;
            logInfo("[pad]  port %d type %d (4=remote) capability 0x%x\n", port, info.device_type[port], info.device_capability[port]);

            // how hard a button is held is only reported when this is asked for, and it is asked
            // for per port, so it goes here rather than at startup where no port exists yet
            if (info.device_capability[port] & CELL_PAD_CAPABILITY_PRESS_MODE)
               cellPadSetPortSetting(port, CELL_PAD_SETTING_PRESS_ON);
         }
         lastConnectedMask = connectedMask;
      }

      for (uint32_t port = 0; port < CELL_PAD_MAX_PORT_NUM; port++) {
         if (!(info.port_status[port] & CELL_PAD_STATUS_CONNECTED)) continue;

         uint32_t deviceType = CELL_PAD_DEV_TYPE_STANDARD;
         CellPadData data;
         if (cellPadGetDataExtra(port, &deviceType, &data) != CELL_OK) continue;

         if (deviceType == CELL_PAD_DEV_TYPE_BD_REMOCON) {
            remotePresent = 1;
            if (data.len > 0) setRemoteCode(data.button[CELL_PAD_BTN_OFFSET_BD_CODE]);   // else keep the held key
         } else if (data.len > 0) {
            current = data;   // keep the last good controller data when a poll reports no change
         }
      }
   }
   if (!remotePresent) { remoteWord1 = remoteWord2 = 0; lastRemoteCode = REMOTE_CODE_NONE; }

   rawDigital1 = remoteWord1 | current.button[CELL_PAD_BTN_OFFSET_DIGITAL1];
   rawDigital2 = remoteWord2 | current.button[CELL_PAD_BTN_OFFSET_DIGITAL2];

   // sticks carry no edges, so publish them straight from the poll. SDK reports unsigned 0..255,
   // centered at 128; shift to signed -128..127.
   leftStick.x = current.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] - 128;
   leftStick.y = current.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y] - 128;
   rightStick.x = current.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] - 128;
   rightStick.y = current.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] - 128;
}

void updatePadEdges(void)
{
   uint16_t digital1 = rawDigital1, digital2 = rawDigital2;
   for (int button = 0; button < PAD_BUTTON_COUNT; button++)
      buttonStates[button] = getState(buttonHeld(button, digital1, digital2), buttonHeld(button, prev1, prev2));
   prev1 = digital1;
   prev2 = digital2;
}

void updatePad(void)
{
   pollPad();
   updatePadEdges();
}

unsigned getPadDownButtons(void)
{
   uint16_t digital1 = rawDigital1, digital2 = rawDigital2;
   unsigned mask = 0;
   for (int button = 0; button < PAD_BUTTON_COUNT; button++)
      if (buttonHeld(button, digital1, digital2)) mask |= 1u << button;
   return mask;
}

PadButtonState getPadButtonState(PadButton button)
{
   return buttonStates[button];
}

int isPadButtonPressed(PadButton button)
{
   return getPadButtonState(button) == PAD_BUTTON_STATE_PRESSED;
}

int isPadButtonHeld(PadButton button)
{
   return getPadButtonState(button) == PAD_BUTTON_STATE_HELD;
}

int isPadButtonDown(PadButton button)
{
   PadButtonState state = getPadButtonState(button);
   return state == PAD_BUTTON_STATE_PRESSED || state == PAD_BUTTON_STATE_HELD;
}

int isPadButtonReleased(PadButton button)
{
   return getPadButtonState(button) == PAD_BUTTON_STATE_RELEASED;
}

int getPadButtonPressure(PadButton button)
{
   if (button < 0 || button >= PAD_BUTTON_COUNT || !isPadButtonDown(button)) return 0;

   // a button with no sensor, and one whose sensor says nothing because the pad or the port does
   // not report pressure, both count as held all the way: the caller gets a usable number either way
   int offset = padControlBits[button].press;
   if (!offset) return PAD_PRESSURE_MAX;

   int pressure = current.button[offset];
   return pressure > 0 ? pressure : PAD_PRESSURE_MAX;
}

Stick getPadLeftStick(void)
{
   return leftStick;
}

Stick getPadRightStick(void)
{
   return rightStick;
}
