#pragma once

// A tunnel that stays up: it connects, keeps the connection alive, and replaces the keys before
// they expire, all driven from one function the caller pumps.
//
// WireGuard puts a clock on every session. Keys may be used for three minutes, and a new handshake
// starts after two, which leaves a minute of slack for it to complete while the old keys still
// carry traffic. A tunnel that ignores these rules works for two minutes and then goes quiet,
// which is why this exists.

#include <stdint.h>

#include "wg-config.h"
#include "wg-handshake.h"
#include "wg-session.h"
#include "wg-transport.h"

typedef enum {
   WG_TUNNEL_DOWN,        // no usable keys
   WG_TUNNEL_CONNECTING,  // an initiation is out, waiting for the answer
   WG_TUNNEL_UP           // keys are good; traffic may flow
} WgTunnelState;

typedef struct {
   WgConfig      config;
   WgTransport   transport;
   WgSession     session;
   WgHandshake   pending;        // the handshake being attempted, if any
   WgTunnelState state;
   int           isRekeying;     // a handshake is running while the current session still works

   // When the keys change, packets already on their way still carry the old ones. Keeping the
   // previous session usable for a while is what stops those being thrown away, which shows up as
   // a request that goes unanswered every time the keys are replaced.
   WgSession previousSession;
   int       hasPreviousSession;
   uint64_t  previousExpiresMs;

   uint64_t sessionStartedMs;
   uint64_t initiationSentMs;
   uint64_t lastSentMs;
   uint64_t lastReceivedMs;
   int      handshakeAttempts;

   // the newest timestamp seen in a handshake the server started, so an old one cannot be replayed
   uint8_t lastPeerTimestamp[12];

   // counters for the caller to display
   uint64_t packetsSent, packetsReceived, handshakesCompleted, handshakesAnswered, pingsAnswered;
} WgTunnel;

int  openWgTunnel(WgTunnel *tunnel, const WgConfig *config);
void closeWgTunnel(WgTunnel *tunnel);

// Do one turn of work: retry or refresh the handshake, send a keepalive if one is due, and wait up
// to waitMs for something to arrive. Returns the length of a received packet written to packet,
// 0 when there was nothing for the caller, or -1 when the tunnel failed and gave up.
//
// receivedSomething, when given, is set to 1 if a datagram arrived at all, including one dealt with
// here rather than passed up. A caller emptying the socket keeps going while it stays 1.
int serviceWgTunnel(WgTunnel *tunnel, uint8_t *packet, int capacity, int waitMs, int *receivedSomething);

// send one IP packet through the tunnel. returns 0, or -1 when the tunnel is not up, which is what
// makes traffic stop dead rather than leak out of the tunnel.
int sendThroughWgTunnel(WgTunnel *tunnel, const uint8_t *packet, int length);

WgTunnelState getWgTunnelState(const WgTunnel *tunnel);
const char *getWgTunnelStateName(const WgTunnel *tunnel);
