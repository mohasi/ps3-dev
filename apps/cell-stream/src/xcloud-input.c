// xcloud-input - see xcloud-input.h.
//
// Everything in these packets is written least significant byte first, which is the opposite of
// this console, so every number goes out a byte at a time. The one exception is the last field of
// a controller, which the machine reads the other way round; it is written that way here because
// that is what it expects, not because it makes sense.

#include "xcloud-input.h"

#include "thread.h"   // getTimeUs
#include "pad.h"
#include "sctp.h"

// what a packet is about. they combine, so one packet can carry several kinds.
#define REPORT_GAMEPAD  2
#define REPORT_METADATA 8

#define HEADER_SIZE   14   // what it is about, its number, and how long the session has been going
#define GAMEPAD_SIZE  23

// which bit stands for which button, from the order the machine reads them in
#define BUTTON_MENU      0x0004
#define BUTTON_VIEW      0x0008
#define BUTTON_A         0x0010
#define BUTTON_B         0x0020
#define BUTTON_X         0x0040
#define BUTTON_Y         0x0080
#define BUTTON_UP        0x0100
#define BUTTON_DOWN      0x0200
#define BUTTON_LEFT      0x0400
#define BUTTON_RIGHT     0x0800
#define BUTTON_SHOULDER_LEFT  0x1000
#define BUTTON_SHOULDER_RIGHT 0x2000
#define BUTTON_THUMB_LEFT     0x4000
#define BUTTON_THUMB_RIGHT    0x8000

#define STICK_FULL   32767   // what the machine reads as a stick pushed all the way
#define STICK_RANGE  127     // what this console reports for the same
#define TRIGGER_FULL 65535

static uint32_t sequence;
static uint64_t startedAt;

static int write16(uint8_t *out, int at, uint16_t value)
{
   out[at] = (uint8_t)value;
   out[at + 1] = (uint8_t)(value >> 8);
   return at + 2;
}

static int write32(uint8_t *out, int at, uint32_t value)
{
   out[at] = (uint8_t)value;
   out[at + 1] = (uint8_t)(value >> 8);
   out[at + 2] = (uint8_t)(value >> 16);
   out[at + 3] = (uint8_t)(value >> 24);
   return at + 4;
}

// how long this has been going, as a fraction, in the layout the machine reads numbers with a
// decimal point in
static int writeElapsed(uint8_t *out, int at, double milliseconds)
{
   union { double asNumber; uint64_t asBytes; } value;
   value.asNumber = milliseconds;

   for (int byte = 0; byte < 8; byte++) out[at + byte] = (uint8_t)(value.asBytes >> (byte * 8));
   return at + 8;
}

static int writeHeader(uint8_t *out, int reportType)
{
   int at = write16(out, 0, (uint16_t)reportType);
   at = write32(out, at, sequence++);
   return writeElapsed(out, at, (double)(getTimeUs() - startedAt) / 1000.0);
}

void sendXcloudInputStart(int channel)
{
   sequence = 0;
   startedAt = getTimeUs();

   uint8_t packet[HEADER_SIZE + 1];
   int at = writeHeader(packet, REPORT_METADATA);
   packet[at++] = 0;   // no touch screen on this console

   sendSctpMessage(channel, SCTP_PAYLOAD_BINARY, packet, at);
}

static uint16_t readButtons(void)
{
   uint16_t mask = 0;
   if (isPadButtonDown(PAD_BTN_CROSS))    mask |= BUTTON_A;
   if (isPadButtonDown(PAD_BTN_CIRCLE))   mask |= BUTTON_B;
   if (isPadButtonDown(PAD_BTN_SQUARE))   mask |= BUTTON_X;
   if (isPadButtonDown(PAD_BTN_TRIANGLE)) mask |= BUTTON_Y;
   if (isPadButtonDown(PAD_BTN_UP))       mask |= BUTTON_UP;
   if (isPadButtonDown(PAD_BTN_DOWN))     mask |= BUTTON_DOWN;
   if (isPadButtonDown(PAD_BTN_LEFT))     mask |= BUTTON_LEFT;
   if (isPadButtonDown(PAD_BTN_RIGHT))    mask |= BUTTON_RIGHT;
   if (isPadButtonDown(PAD_BTN_L1))       mask |= BUTTON_SHOULDER_LEFT;
   if (isPadButtonDown(PAD_BTN_R1))       mask |= BUTTON_SHOULDER_RIGHT;
   if (isPadButtonDown(PAD_BTN_L3))       mask |= BUTTON_THUMB_LEFT;
   if (isPadButtonDown(PAD_BTN_R3))       mask |= BUTTON_THUMB_RIGHT;
   if (isPadButtonDown(PAD_BTN_START))    mask |= BUTTON_MENU;
   if (isPadButtonDown(PAD_BTN_SELECT))   mask |= BUTTON_VIEW;
   return mask;
}

// this console reports how hard a shoulder button is held as 0 to 255, the machine over the full
// range of an unsigned number
static uint16_t scaleTrigger(int pressure)
{
   return (uint16_t)(pressure * TRIGGER_FULL / PAD_PRESSURE_MAX);
}

// this console reports a stick as -128 to 127 and the machine wants the full range of a signed
// number, so it is scaled up
static int16_t scaleStick(int value)
{
   int scaled = value * STICK_FULL / STICK_RANGE;
   if (scaled > STICK_FULL) return STICK_FULL;
   if (scaled < -STICK_FULL) return -STICK_FULL;
   return (int16_t)scaled;
}

void sendXcloudGamepad(int channel)
{
   uint8_t packet[HEADER_SIZE + 1 + GAMEPAD_SIZE];
   int at = writeHeader(packet, REPORT_GAMEPAD);

   packet[at++] = 1;   // one controller
   packet[at++] = 0;   // and it is the first

   uint16_t buttons = readButtons();
   at = write16(packet, at, buttons);

   // up on this console's sticks is a smaller number, and on the machine's a larger one
   Stick left = getPadLeftStick(), right = getPadRightStick();
   at = write16(packet, at, (uint16_t)scaleStick(left.x));
   at = write16(packet, at, (uint16_t)scaleStick(-left.y));
   at = write16(packet, at, (uint16_t)scaleStick(right.x));
   at = write16(packet, at, (uint16_t)scaleStick(-right.y));

   // the shoulder buttons underneath are pressure sensitive on this console and the machine reads
   // them over a range, so how hard they are held is carried through rather than being flattened
   // to on or off. a racing game needs that for the throttle.
   at = write16(packet, at, scaleTrigger(getPadButtonPressure(PAD_BTN_L2)));
   at = write16(packet, at, scaleTrigger(getPadButtonPressure(PAD_BTN_R2)));

   at = write32(packet, at, 1);

   // the machine reads this last field the other way round from every other number in the packet
   packet[at++] = 0;
   packet[at++] = 0;
   packet[at++] = 0;
   packet[at++] = 1;

   sendSctpMessage(channel, SCTP_PAYLOAD_BINARY, packet, at);
}
