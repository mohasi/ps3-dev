#pragma once

// xcloud-auth - signing in to the Microsoft account that owns the Xbox Cloud subscription.
//
// Microsoft's device-code flow: we ask for a code, the user types it into microsoft.com/link on a
// phone, and we poll until they have. What comes back is a refresh token, saved so the console only
// has to be signed in once.

#include <stdint.h>

#define XCLOUD_USER_CODE_MAX 32
#define XCLOUD_STREAM_TOKEN_MAX 8192   // Xbox Live tokens are signed blobs of several KB

typedef enum {
   XCLOUD_AUTH_IDLE,
   XCLOUD_AUTH_WAITING_FOR_USER,   // the code is on screen, the user has not finished yet
   XCLOUD_AUTH_SIGNED_IN,
   XCLOUD_AUTH_FAILED
} XcloudAuthState;

// starts the flow and fills userCode with what the user must type. 0 on success, -1 when the
// request failed. A saved refresh token short-circuits this and reports signed in straight away.
int startXcloudSignIn(void);

// call about once a second while WAITING_FOR_USER; it does the polling and moves the state on.
void pollXcloudSignIn(void);

XcloudAuthState getXcloudAuthState(void);
const char *getXcloudUserCode(void);      // what the user types at microsoft.com/link
const char *getXcloudAuthError(void);     // why it failed, for the screen

// Trades the Microsoft sign-in for the pass the Xbox streaming servers want, through Xbox Live. Only
// valid once signed in. 0 on success, -1 with the reason in getXcloudAuthError().
//
// It also tells us which regional server to talk to, which is not known until this point: the answer
// names it, and every later request in the session goes there.
int fetchXcloudStreamingToken(void);

const char *getXcloudStreamHost(void);    // e.g. https://uks.core.gssv-play-prod.xboxlive.com
const char *getXcloudStreamToken(void);   // the pass, sent with every request to that host

// A separate short-lived token the streaming server asks for when a session is ready to connect.
// It comes from a different Microsoft endpoint to the sign-in one. 0 on success, -1 on failure.
int fetchXcloudPassportToken(char *out, int capacity);
