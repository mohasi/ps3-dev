#pragma once

// net-common - socket plumbing for the video stream:
// server discovery via beacon, timeouts, draining, tiny ASCII parsing, wall-clock us.

#include <stdint.h>
#include <netinet/in.h>

#define SERVER_PORT 38310   // cell-stream-server listens here
#define CLIENT_PORT 38311   // we bind here; server beacons broadcast to this port
#define PACKET_MAX  1500

uint64_t getTimeUs(void);
void setReceiveTimeout(int socketValue, int milliseconds);
void drainSocket(int socketValue);   // discard queued packets so an earlier test can't pollute the next

// creates a UDP socket bound to CLIENT_PORT with a large receive buffer. -1 on failure.
int openClientSocket(void);

// waits for the server's CELLSTREAM beacon; fills serverAddress. 1 on success, 0 on timeout.
int discoverServer(int socketValue, struct sockaddr_in *serverAddress, int timeoutMs);

// parses the unsigned integer that follows `prefix` in `text`; -1 when the prefix doesn't match
long parseNumberAfter(const char *text, const char *prefix);

// same for values that overflow a long (32 bits here) - e.g. the server's clock, in microseconds since 2020
long long parseBigNumberAfter(const char *text, const char *prefix);
