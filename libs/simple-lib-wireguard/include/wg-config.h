#pragma once

// Reads the ordinary WireGuard .conf file that a VPN provider hands out, the same format the
// desktop clients take. Only the keys a client needs are recognised; anything else in the file is
// skipped rather than treated as an error, because providers add comment lines and extra settings.
//
// The file holds a private key, so it is a secret: never log its contents, and never bake one into
// a package.

#include <stdint.h>

#define WG_KEY_LENGTH          32
#define WG_ENDPOINT_MAX_LENGTH 64

typedef struct {
   uint8_t  privateKey[WG_KEY_LENGTH];      // [Interface] PrivateKey
   uint32_t tunnelAddress;                  // [Interface] Address, our address inside the tunnel
   int      tunnelPrefixLength;             // the /32 part of that address
   uint32_t dnsAddress;                     // [Interface] DNS, 0 when absent

   uint8_t  peerPublicKey[WG_KEY_LENGTH];   // [Peer] PublicKey
   uint8_t  presharedKey[WG_KEY_LENGTH];    // [Peer] PresharedKey, optional
   int      hasPresharedKey;
   int      routesAllTraffic;               // [Peer] AllowedIPs covers 0.0.0.0/0
   char     endpointHost[WG_ENDPOINT_MAX_LENGTH];
   uint16_t endpointPort;
   int      keepaliveSeconds;               // [Peer] PersistentKeepalive, 0 when absent
} WgConfig;

// both return 0 on success, -1 when a required field is missing or malformed.
int parseWgConfig(WgConfig *config, const char *text, int length);
int loadWgConfig(WgConfig *config, const char *path);

// "203.0.113.7" into an IPv4 address in host order. returns 0 / -1. a config endpoint given as
// a name rather than an address fails here, and has to be looked up first.
int parseIpv4Text(const char *text, uint32_t *address);
