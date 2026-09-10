#pragma once

// stun - the small message format two ends of a connection use to check they can actually reach
// each other, RFC 5389 with the extra fields RFC 8445 adds for that check.
//
// Nothing here knows about Xbox or streaming. It builds one request and reads one reply.

#include <stdint.h>

#define STUN_TRANSACTION_LENGTH 12
#define STUN_MESSAGE_MAX 512

typedef struct {
   uint8_t transaction[STUN_TRANSACTION_LENGTH];   // what ties a reply to its request
} StunPendingRequest;

// Builds a check addressed to the far end. `username` is the far end's name followed by ours, and
// `password` is the far end's, because the far end is who has to believe the message. `priority`
// and `tiebreaker` are what the standard uses to agree which address wins and who decides.
//
// `nominate` marks the check as choosing this address for the connection. Only the side that
// declared itself in charge may do that, and only once an address has proved it works.
//
// Returns the message length, or -1 if it would not fit.
int buildStunRequest(uint8_t *out, int capacity, const char *username, const char *password, uint32_t priority,
                     uint64_t tiebreaker, int nominate, StunPendingRequest *request);

typedef enum {
   STUN_REPLY_NOT_STUN,      // not one of these messages at all
   STUN_REPLY_OTHER,         // a valid message, but not the answer to this request
   STUN_REPLY_SUCCESS,
   STUN_REPLY_ERROR          // the far end refused the check
} StunReplyKind;

// What the far end said when it refused, so a refusal can be acted on rather than just retried.
typedef struct {
   int code;              // 400, 401, 487 and so on; 0 when the reply carried none
   char reason[64];
} StunError;

// Reads a reply and says whether it answers `request`. When it does and the far end reported the
// address it saw us on, that is written to `seenAddress` and `seenPort`. When it refused, `error`
// is filled in. Any of the outputs may be NULL.
StunReplyKind readStunReply(const uint8_t *message, int length, const StunPendingRequest *request,
                            uint32_t *seenAddress, uint16_t *seenPort, StunError *error);

// section: answering a check the far end sent us

#define STUN_USERNAME_MAX 128

typedef struct {
   uint8_t transaction[STUN_TRANSACTION_LENGTH];
   char username[STUN_USERNAME_MAX];   // the far end's name followed by ours
   int nominated;                      // the far end wants this address to be the one used
} StunIncomingRequest;

// Recognises a check addressed to us and pulls out what is needed to answer it. Returns 0 when the
// message is one, -1 otherwise.
int readStunRequest(const uint8_t *message, int length, StunIncomingRequest *request);

// The answer that says we are here, signed with our own password, telling the far end which
// address we received it from. Returns the message length, or -1 if it would not fit.
int buildStunResponse(uint8_t *out, int capacity, const StunIncomingRequest *request, const char *password,
                      uint32_t theirAddress, uint16_t theirPort);

// Runs the published test vectors for the two things a check depends on: the keyed hash that proves
// who sent it, and the checksum that proves it arrived whole. Logs one line per failure and returns
// how many there were, so 0 means both match the documents they came from.
int runStunSelfTest(void);
