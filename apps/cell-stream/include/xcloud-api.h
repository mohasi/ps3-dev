#pragma once

// xcloud-api - talking to the regional streaming server the sign-in pointed us at. Everything here
// needs fetchXcloudStreamingToken() to have succeeded first.

#define XCLOUD_TITLE_ID_MAX  64
#define XCLOUD_PRODUCT_ID_MAX 32
#define TITLE_LIST_MAX 512   // a Game Pass account lists a few hundred playable ones

// One entry from the account's title list. The list carries no human-readable name: that lives in a
// separate Microsoft catalog service, looked up by productId.
typedef struct {
   char id[XCLOUD_TITLE_ID_MAX];             // what a play request names
   char productId[XCLOUD_PRODUCT_ID_MAX];    // what the store's catalog is keyed by
} XcloudTitle;

// Loads the titles this account can actually start (owned, or covered by its subscription).
// Returns how many, or -1 on failure with the reason in getXcloudApiError().
int fetchXcloudTitles(void);

int getXcloudTitleCount(void);
const XcloudTitle *getXcloudTitle(int index);   // NULL when index is out of range

const char *getXcloudApiError(void);

// A play session. Asking for one puts a machine in the region to work; it is only worth connecting
// to once it reports ready, which takes a few seconds and sometimes a queue.
typedef enum {
   XCLOUD_SESSION_NONE,
   XCLOUD_SESSION_PROVISIONING,   // a machine is being made ready
   XCLOUD_SESSION_WAITING,        // queued behind other players
   XCLOUD_SESSION_AUTHORIZING,    // the server is asking us to prove who we are before it goes further
   XCLOUD_SESSION_READY,          // the machine is up and waiting to be connected to
   XCLOUD_SESSION_FAILED
} XcloudSessionState;

// Asks for a machine to play titleId on. 0 when the request was accepted, -1 otherwise; being
// accepted only means the session exists, not that it is ready.
int startXcloudSession(const char *titleId);

// Call about once a second after starting one. It asks how the session is doing and does the
// handover the server expects when it first reports ready.
XcloudSessionState pollXcloudSession(void);

XcloudSessionState getXcloudSessionState(void);

// Gives the machine back. Sessions cost the account money and time, so one is never left running.
void stopXcloudSession(void);

// Offers the machine a connection description and waits for its reply. Only valid once the session
// is READY. 0 on success, -1 otherwise.
//
// This is the step that decides how the picture will actually arrive, so what comes back is written
// to /dev_hdd0/tmp/cell-stream/sdp-answer.txt to be read afterwards: it names the addresses to talk
// to, the codecs chosen and the encryption fingerprint, none of which can be known in advance.
int exchangeXcloudSdp(const char *offer);

// Tells the machine one address it can reach us on, then waits for its own list. Only valid after
// the description has been exchanged. 0 on success, -1 otherwise.
int postXcloudIceCandidate(const char *candidate);

// The machine's reply to the above, as it arrived. Valid until the next call.
const char *getXcloudIceAnswer(void);

// The connection description the machine answered with, unwrapped. Valid after exchangeXcloudSdp.
const char *getXcloudSdpAnswer(void);
