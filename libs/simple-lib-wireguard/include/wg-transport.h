#pragma once

// The one real network connection the library makes: a UDP socket to the VPN server.
//
// It is pinned to the server address, so the console will refuse to send anything from it to any
// other address. Everything else the app does travels inside this socket, encrypted.

#include <stdint.h>

typedef struct {
   int socketHandle;
   int timeoutMs;   // what the socket is currently set to wait, so it is only changed when it changes
} WgTransport;

// address is IPv4 in host order, as parsed from the config. returns 0 / -1.
int openWgTransport(WgTransport *transport, uint32_t address, uint16_t port);

int sendWgDatagram(WgTransport *transport, const void *data, int length);

// returns the number of bytes received, 0 if nothing arrived before the timeout, -1 on error.
// timeoutMs 0 takes whatever is already waiting and returns straight away.
int receiveWgDatagram(WgTransport *transport, void *buffer, int capacity, int timeoutMs);

void closeWgTransport(WgTransport *transport);
