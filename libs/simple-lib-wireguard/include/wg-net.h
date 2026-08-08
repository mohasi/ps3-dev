#pragma once

// What an app actually calls. One VPN connection per app, so this owns it: there is no handle to
// pass around and nothing to construct.
//
// Every call here goes through the tunnel or fails. There is no way to ask for a packet to be sent
// outside it, and no fallback when it is down, so there is no path traffic can leak through. An app
// that wants the ordinary network connection does not use this.

#include <stdint.h>

#include "wg-tunnel.h"

#define WG_SOCKET_MAX  32     // how many ports an app can listen on at once
#define WG_PACKET_MAX  1536   // the largest packet held for a caller

typedef int WgSocket;         // 0 and above is a socket, -1 is a failure

typedef struct {
   uint32_t address;
   uint16_t port;
} WgEndpoint;

// load the config and bring the tunnel up. returns 0, or -1 with the reason logged.
int startWgNetwork(const char *configPath);
void stopWgNetwork(void);

// Do one turn of work: keep the tunnel alive and hand any arriving packets to the sockets waiting
// for them. An app calls this in its main loop. waitMs is how long to wait for traffic before
// returning. Returns 0, or -1 when the tunnel has failed.
int serviceWgNetwork(int waitMs);

int isWgNetworkReady(void);                 // 1 when traffic can flow right now
const WgTunnel *getWgNetworkTunnel(void);   // counters and state, for a diagnostics display

// look a host name up through the tunnel. 0 and fills address, or -1. never falls back to the
// console's own lookup, because that would go out in the clear.
int resolveWgHost(const char *hostName, uint32_t *address, int timeoutMs);

// Find the largest packet that gets through the tunnel, by pinging the gateway with increasing
// sizes. Returns that size in bytes, or -1 if nothing came back at all.
//
// Read this as "what arrives", not "what fits". A packet too large for some link along the way is
// usually chopped into pieces and reassembled rather than refused, so a large size can succeed
// while being slower and more fragile than a smaller one. Finding the size that avoids that
// entirely needs the outer packet marked as unsplittable, which the console's own network stack
// builds and does not let us mark. What this does rule out is a path that silently swallows large
// packets, which is the failure that looks like a bug in whatever was sending them.
//
// The result is also used: streams opened afterwards keep their segments inside what got through,
// which is how a server whose path carries less than the one this was tested against is handled.
// It can only lower the segment size, never raise it above the tested maximum.
//
// Run it while nothing else is using the tunnel: it reads packets directly, so anything arriving
// for a socket during the measurement is lost.
int measureWgPacketLimit(void);

// localPort 0 picks an unused one. returns the socket, or -1 if none are free.
WgSocket openWgUdp(uint16_t localPort);
void closeWgUdp(WgSocket socket);

// returns 0, or -1 when the tunnel is down or the packet will not fit
int sendWgTo(WgSocket socket, const WgEndpoint *to, const void *data, int length);

// takes the next packet waiting for this socket. returns its length, 0 when nothing is waiting, or
// -1 for a bad socket. does not wait: call serviceWgNetwork to let packets arrive.
int recvWgFrom(WgSocket socket, WgEndpoint *from, void *buffer, int capacity);

// TCP through the tunnel: a stream to one host, for web requests and torrent peers.
//
// A stream takes 384 KB of buffer while it is open, which is what lets one run at several megabytes
// a second. The memory is asked for when the stream opens and given back when it closes, so an app
// that holds two streams pays for two.
#define WG_TCP_MAX 28   // how many streams an app can hold open at once

typedef int WgTcpSocket;

// Start a connection and return at once, before the other end has answered. The stream is not
// usable until isWgTcpConnecting returns 0; serviceWgNetwork drives it, sends the hello again if it
// goes unanswered, and marks it failed after about fifteen seconds of silence.
//
// This is the one to use for many peers at once: open them all, then service the network while they
// answer, rather than waiting out one timeout before starting the next.
WgTcpSocket openWgTcp(uint32_t address, uint16_t port);

int isWgTcpConnecting(WgTcpSocket socket);
int isWgTcpFailed(WgTcpSocket socket);

// Open one and wait for the other end to agree, for callers with nothing else to do meanwhile.
// Returns the socket, or -1 with the reason logged.
WgTcpSocket connectWgTcp(uint32_t address, uint16_t port, int timeoutMs);

// Hand data over to be sent, and return how much was taken. It does not wait for the other end to
// confirm it: several packets are kept in flight at once, which is what makes bulk transfer fast.
// It may take less than asked, so call it again with the rest. It only waits, up to timeoutMs, when
// there is no room at all, and returns -1 if none frees up or the stream fails.
int sendWgTcp(WgTcpSocket socket, const void *data, int length, int timeoutMs);

// take whatever has arrived. returns the number of bytes, 0 when nothing is waiting, or -1 if the
// connection has failed. does not wait: call serviceWgNetwork to let data arrive.
int recvWgTcp(WgTcpSocket socket, void *buffer, int capacity);

// 1 once the other end has finished sending and everything it sent has been read
int isWgTcpFinished(WgTcpSocket socket);

void closeWgTcp(WgTcpSocket socket);
