#pragma once

// The network the app lends this library. Nothing here opens a socket itself: Swarm hands over calls
// that go through the WireGuard tunnel, which is what keeps the console's own connection out of it.
//
// An app binds one of these before anything that talks to a tracker or a peer is called.

#include <stdint.h>

typedef struct {
   int  (*resolve)(const char *hostName, uint32_t *address, int timeoutMs);
   int  (*openUdp)(void);                                                        // a handle, or -1
   // sendUdp answers 0 when the whole datagram went, or -1; the byte count is not what is read
   int  (*sendUdp)(int handle, uint32_t address, uint16_t port, const void *data, int length);
   // receiveUdp gives back the length, or 0 when nothing is waiting, and says who sent it
   int  (*receiveUdp)(int handle, uint32_t *fromAddress, uint16_t *fromPort, void *buffer, int capacity);
   void (*closeUdp)(int handle);

   // a stream to one peer. openTcp returns before the other end has answered, so isTcpConnecting is
   // what says when it is usable and isTcpFailed what says it never will be.
   int  (*openTcp)(uint32_t address, uint16_t port);
   int  (*isTcpConnecting)(int handle);
   int  (*isTcpFailed)(int handle);
   // sendTcp answers how many bytes went, which the caller checks against what it asked for
   int  (*sendTcp)(int handle, const void *data, int length, int timeoutMs);
   int  (*receiveTcp)(int handle, void *buffer, int capacity);   // 0 when nothing has arrived yet
   void (*closeTcp)(int handle);

   void     (*serviceNetwork)(int waitMs);   // let packets arrive while we wait for a reply
   int      (*getRandom)(uint8_t *out, int length);
   uint64_t (*getNowMs)(void);   // any clock that only goes forward; timeouts and speeds come from it
} TorrentNetwork;

void bindTorrentNetwork(const TorrentNetwork *network);
const TorrentNetwork *getTorrentNetwork(void);   // 0 when the app has not bound one
