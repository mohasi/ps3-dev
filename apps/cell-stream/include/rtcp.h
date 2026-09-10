#pragma once

// rtcp - the report sent back saying what arrived.
//
// A sender decides how much picture to send from what its receiver tells it. Told nothing, it
// assumes the least, which is what was happening here: the machine sent about a tenth of the
// bitrate it was asked for. This counts what arrives and says so, about once a second.
//
// Only the receiver's side is here. Nothing on the console sends picture or sound, so nothing
// here builds a sender's report.

#include <stdint.h>

void openRtcp(void);

// Counts one arriving media packet. The packet is the plain one, after its protection is off.
void noteRtcpPacket(const void *packet, int length, uint64_t arrivedAtUs);

// Takes one arriving report from the machine, unwrapped. It carries the machine's own clock, which
// goes back in the next report so it can work out how long the round trip took.
void noteRtcpSenderReport(const void *packet, int length, uint64_t arrivedAtUs);

// Builds the report, wrapped and ready to send, if one is due. Returns its length, or 0 if it is
// not time yet or nothing has arrived to report on.
int getRtcpReport(uint64_t nowUs, uint8_t *out, int capacity);
