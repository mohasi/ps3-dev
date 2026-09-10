#pragma once

#include <stdint.h>

// Resolves the console's primary local IPv4 address into *ip, in network byte
// order (as in sockaddr_in.s_addr, so formatIpv4 renders it directly).
// Returns 0 on success, or a negative value if no address could be determined.
int getLocalIpv4(uint32_t *ip);

// Returns 1 if the console has a usable IPv4 network, else 0.
int isNetworkAvailable(void);

// How long a read on this socket waits before giving up, in milliseconds. Without one a socket
// with nothing to say blocks its thread for good.
void setReceiveTimeout(int socketValue, int milliseconds);
