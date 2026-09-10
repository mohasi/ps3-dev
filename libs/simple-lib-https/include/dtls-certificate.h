#pragma once

// dtls-certificate - the certificate the console presents when it encrypts the stream.
//
// Both ends of a WebRTC connection present one, and neither is signed by anybody: what makes it
// mean something is that its fingerprint was named in the connection description, which travelled
// over a channel already trusted. So this is generated fresh here rather than obtained from
// anywhere, and its only job is to be the same certificate the description promised.

#include <stdint.h>

#define DTLS_FINGERPRINT_MAX 100   // 32 bytes as "AA:" pairs, plus room for the terminator

// Makes a key and a certificate for it. Takes a moment, so it is done once and kept. 0 or -1.
int createDtlsCertificate(void);

// The certificate as the far end will receive it. NULL until created.
const uint8_t *getDtlsCertificate(int *length);

// Its fingerprint, in the upper-case colon-separated form the description carries.
const char *getDtlsFingerprint(void);

// Writes any certificate's fingerprint into out, which must hold DTLS_FINGERPRINT_MAX bytes. Used
// on the far end's certificate as well as on ours, so both are measured the same way.
void writeCertificateFingerprint(const uint8_t *der, int length, char *out);

// Signs a SHA-256 digest with the certificate's key, writing the signature into out. Returns its
// length, or 0. The key itself never leaves this file.
int signWithDtlsKey(const uint8_t *digest, uint8_t *out, int capacity);
