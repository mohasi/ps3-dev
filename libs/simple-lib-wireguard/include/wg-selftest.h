#pragma once

// Runs the published test vectors for every primitive and logs one line per case.
// Returns the number of failures, so 0 means everything matched.
//
// This is the gate the rest of the library sits on: nothing that depends on the crypto should be
// trusted until this returns 0 on the console itself.

int runWgSelfTest(void);
