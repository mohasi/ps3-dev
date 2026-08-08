#pragma once

// Random bytes for key material.
//
// The console has a proper source. The SDK manual page for sys_get_random_number (Lv2 System Call
// Reference, "Other System Calls") says it is the FIPS 186-2 Appendix 3.1 generator seeded from the
// hardware true random number generator built into the console, that the output differs on every
// call, and that it costs about 15 microseconds per 20 bytes, plus roughly 20 milliseconds on the
// first call after the console starts.
//
// That generator is old and no longer recommended on its own, so the output is hashed before use.
// Hashing cannot reduce the randomness and it keeps our key material away from the structure of a
// deprecated generator.

#include <stdint.h>

// returns 0 on success, -1 if the console refused to produce randomness. a failure must abort
// whatever key was being generated; there is no fallback, because a predictable key would work
// perfectly while offering no protection at all.
int getRandomBytes(uint8_t *out, int length);
