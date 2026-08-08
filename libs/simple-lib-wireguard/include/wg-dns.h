#pragma once

// Turning a host name into an address, over the tunnel.
//
// This is the part that stops a VPN leaking. The console's own name lookup goes out of the network
// card whatever the tunnel is doing, so it would tell your internet provider every address the app
// is about to visit, even though the traffic itself is encrypted. Asking the VPN's own name server
// through the tunnel is the only way that does not happen.

#include <stdint.h>

#define DNS_PORT 53

// write a query for the address of hostName. returns its length, or -1 if the name will not fit.
int buildDnsQuery(uint8_t *query, int capacity, uint16_t transactionId, const char *hostName);

// read the answer and take the first address out of it. returns 0 and fills address on success,
// -1 when the answer is malformed, belongs to another question, reports an error, or holds no
// address at all.
int readDnsAnswer(const uint8_t *answer, int length, uint16_t expectedTransactionId, uint32_t *address);
