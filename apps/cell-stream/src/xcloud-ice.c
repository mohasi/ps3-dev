// xcloud-ice - see xcloud-ice.h.

#include "xcloud-ice.h"

#include "dbg.h"
#include "dtls-certificate.h"
#include "json.h"
#include "printf.h"
#include "thread.h"   // getTimeUs
#include "network.h"   // setReceiveTimeout
#include "string-utilities.h"
#include "stun.h"
#include "xcloud-api.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/random_number.h>
#include <sys/socket.h>

#define TAG "[cst] "

#define OFFER_MAX 2048
#define REMOTE_CANDIDATE_MAX 12
#define CANDIDATE_TEXT_MAX 160
#define ROUTE_PROBE_ADDRESS 0x08080808u   // a public address, only used to ask which way traffic leaves
#define UNWRAPPED_ANSWER_MAX 8192

#define NEGOTIATE_ROUNDS  40      // each round sends one check and waits briefly for anything back
#define POLL_TIMEOUT_MS   250
#define CHECK_PRIORITY    ((126u << 24) | (65535u << 8) | 255u)   // the standard's formula for a host address

// characters the standard allows in these credentials
static const char ICE_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char username[ICE_USERNAME_MAX];
static char password[ICE_PWD_MAX];
static char localAddress[ICE_ADDRESS_MAX];
static int localPort;
static int iceSocket = -1;
static int socketTimeoutMs = -1;   // what the socket's read timeout is already set to, so it is only set when it changes

// setting the timeout is a call into the kernel, and the receive loop asks for the same value on
// every packet - over a thousand a second while a game is streaming.
static void useReceiveTimeout(int milliseconds)
{
   if (milliseconds == socketTimeoutMs) return;
   setReceiveTimeout(iceSocket, milliseconds);
   socketTimeoutMs = milliseconds;
}
static struct sockaddr_in chosenAddress;   // the one address that proved reachable, for traffic after this

static char offer[OFFER_MAX];
static char remoteCandidates[REMOTE_CANDIDATE_MAX][CANDIDATE_TEXT_MAX];
static int remoteCandidateCount;
static int machineNominated;   // the machine named an address, which it only does if it is in charge
static uint64_t roundTripUs;   // how long a check took to reach the machine and come back

static char remoteUsername[ICE_USERNAME_MAX * 4];     // the machine picks its own lengths
static char remotePassword[ICE_PWD_MAX * 2];
static char remoteFingerprint[DTLS_FINGERPRINT_MAX];

// the console has a hardware random source; the SDK calls it out as old, so the bytes are only used
// to pick characters rather than as key material
static int fillCredential(char *out, int length)
{
   uint8_t raw[ICE_PWD_MAX];
   if (sys_get_random_number(raw, length) != 0) return -1;

   for (int at = 0; at < length; at++) out[at] = ICE_ALPHABET[raw[at] & 63];
   out[length] = 0;
   return 0;
}

// which of this console's addresses the machine would see. asking the network settings needs a
// library the app does not otherwise use, so instead a throwaway socket is pointed at a public
// address and asked which address it would send from. connecting a UDP socket sends nothing.
static int readLocalAddress(void)
{
   int probe = socket(AF_INET, SOCK_DGRAM, 0);
   if (probe < 0) return -1;

   struct sockaddr_in elsewhere;
   memSet(&elsewhere, 0, sizeof elsewhere);
   elsewhere.sin_family = AF_INET;
   elsewhere.sin_addr.s_addr = htonl(ROUTE_PROBE_ADDRESS);
   elsewhere.sin_port = htons(53);

   int found = -1;
   if (connect(probe, (struct sockaddr *)&elsewhere, sizeof elsewhere) == 0) {
      struct sockaddr_in mine;
      socklen_t mineLength = sizeof mine;
      if (getsockname(probe, (struct sockaddr *)&mine, &mineLength) == 0) {
         formatIpv4(localAddress, sizeof localAddress, ntohl(mine.sin_addr.s_addr));
         found = 0;
      }
   }

   socketclose(probe);
   return found;
}

// the description offered to the machine. one bundle for everything, because that is what the
// machine answers with anyway, and the console receives all three.
//
// the attribute names below are the protocol's, not ours: "ufrag" here means the username field and
// must stay spelled that way on the wire whatever the variable holding it is called.
static void buildOffer(void)
{
   snprintf(offer, sizeof offer,
      "v=0\n"
      "o=- 1 2 IN IP4 127.0.0.1\n"
      "s=-\n"
      "t=0 0\n"
      "a=group:BUNDLE 0 1 2\n"
      "a=msid-semantic: WMS\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 102\n"
      "c=IN IP4 0.0.0.0\n"
      "a=rtcp:9 IN IP4 0.0.0.0\n"
      "a=ice-ufrag:%s\n"
      "a=ice-pwd:%s\n"
      "a=ice-options:trickle\n"
      "a=fingerprint:sha-256 %s\n"
      "a=setup:active\n"
      "a=mid:0\n"
      "a=recvonly\n"
      "a=rtcp-mux\n"
      "a=rtpmap:102 H264/90000\n"
      "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e02a\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 111\n"
      "c=IN IP4 0.0.0.0\n"
      "a=ice-ufrag:%s\n"
      "a=ice-pwd:%s\n"
      "a=fingerprint:sha-256 %s\n"
      "a=setup:active\n"
      "a=mid:1\n"
      "a=recvonly\n"
      "a=rtcp-mux\n"
      "a=rtpmap:111 opus/48000/2\n"
      "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\n"
      "c=IN IP4 0.0.0.0\n"
      "a=ice-ufrag:%s\n"
      "a=ice-pwd:%s\n"
      "a=fingerprint:sha-256 %s\n"
      "a=setup:active\n"
      "a=mid:2\n"
      "a=sctp-port:5000\n",
      username, password, getDtlsFingerprint(),
      username, password, getDtlsFingerprint(),
      username, password, getDtlsFingerprint());
}

int openXcloudIce(void)
{
   closeXcloudIce();

   if (fillCredential(username, ICE_USERNAME_MAX - 1) != 0 || fillCredential(password, ICE_PWD_MAX - 1) != 0) {
      logError(TAG "ice: the console would not produce randomness\n");
      return -1;
   }

   if (readLocalAddress() != 0) {
      logError(TAG "ice: could not read this console's address\n");
      return -1;
   }

   // any free port will do; the machine is told which one in the candidate below
   iceSocket = socket(AF_INET, SOCK_DGRAM, 0);
   socketTimeoutMs = -1;   // a new socket carries none of the old one's settings
   if (iceSocket < 0) {
      logError(TAG "ice: no socket to receive on\n");
      return -1;
   }

   struct sockaddr_in bound;
   memSet(&bound, 0, sizeof bound);
   bound.sin_family = AF_INET;
   bound.sin_addr.s_addr = INADDR_ANY;
   bound.sin_port = 0;
   if (bind(iceSocket, (struct sockaddr *)&bound, sizeof bound) < 0) {
      logError(TAG "ice: could not bind a port\n");
      closeXcloudIce();
      return -1;
   }

   socklen_t boundLength = sizeof bound;
   if (getsockname(iceSocket, (struct sockaddr *)&bound, &boundLength) < 0) {
      logError(TAG "ice: could not read the bound port\n");
      closeXcloudIce();
      return -1;
   }
   localPort = ntohs(bound.sin_port);

   buildOffer();
   logInfo(TAG "ice: receiving on %s:%d as %s\n", localAddress, localPort, username);
   return 0;
}

void closeXcloudIce(void)
{
   if (iceSocket >= 0) socketclose(iceSocket);
   iceSocket = -1;
   socketTimeoutMs = -1;
   remoteCandidateCount = 0;
}

const char *getXcloudOffer(void) { return offer; }

int exchangeXcloudIceCandidates(void)
{
   // where the machine can reach us
   char candidate[CANDIDATE_TEXT_MAX];
   snprintf(candidate, sizeof candidate, "candidate:1 1 udp %u %s %d typ host generation 0", CHECK_PRIORITY,
            localAddress, localPort);

   remoteCandidateCount = 0;
   int count = postXcloudIceCandidate(candidate);
   if (count < 0) return -1;

   // and where it can be reached. like the connection description, the list is wrapped: the reply is
   // an object holding a string that is itself the JSON we want.
   static char unwrapped[UNWRAPPED_ANSWER_MAX];
   const char *reply = getXcloudIceAnswer();
   if (getJsonText(reply, getStrLen(reply), "exchangeResponse", unwrapped, sizeof unwrapped) != 0) {
      logError(TAG "ice: the machine's reply held nothing to unwrap: %.200s\n", reply);
      return -1;
   }

   // what is inside the wrapper is the list itself, not an object holding one
   int answerLength = getStrLen(unwrapped);
   int listStart = 0, listEnd = 0;
   if (findJsonArray(unwrapped, answerLength, "", &listStart, &listEnd) != 0) {
      logError(TAG "ice: the machine sent no address list: %.200s\n", unwrapped);
      return -1;
   }

   int entryStart = 0, entryEnd = 0, offsetInList = listStart;
   while (remoteCandidateCount < REMOTE_CANDIDATE_MAX &&
          (offsetInList = readJsonObject(unwrapped, listEnd, offsetInList, &entryStart, &entryEnd)) != 0) {
      char line[CANDIDATE_TEXT_MAX];
      if (getJsonText(unwrapped + entryStart, entryEnd - entryStart, "candidate", line, sizeof line) != 0) continue;
      if (!line[0]) continue;   // an empty one only means the machine has finished listing

      // the machine writes these as a whole SDP line; only what follows the "a=" is the address
      const char *address = startsWith(line, "a=") ? line + 2 : line;

      // it ends the list with a marker rather than an address, and that is not somewhere to send to
      if (!startsWith(address, "candidate:")) continue;

      strCopy(remoteCandidates[remoteCandidateCount], CANDIDATE_TEXT_MAX, address);
      logInfo(TAG "ice: machine at %s\n", remoteCandidates[remoteCandidateCount]);
      remoteCandidateCount++;
   }

   return remoteCandidateCount;
}

// section: proving the two ends can reach each other

// the value of an "a=name:value" line in the machine's answer.
//
// the value ends at a space as well as at a line break, because the description reached us through
// the JSON reader and that turns a newline into a space. reading only to a line break swallowed the
// whole rest of the description into the credential, and the machine refused every check we signed
// with it.
static int readSdpAttribute(const char *sdp, const char *name, char *out, int capacity)
{
   int sdpLength = getStrLen(sdp), nameLength = getStrLen(name);
   int at = findBytes(sdp, sdpLength, name, nameLength);
   if (at < 0) return -1;

   at += nameLength;
   int written = 0;
   while (at < sdpLength && sdp[at] != '\r' && sdp[at] != '\n' && sdp[at] != ' ' && written < capacity - 1)
      out[written++] = sdp[at++];
   out[written] = 0;
   return written > 0 ? 0 : -1;
}

int readXcloudRemoteCredentials(const char *answerSdp)
{
   if (readSdpAttribute(answerSdp, "a=ice-ufrag:", remoteUsername, sizeof remoteUsername) != 0 ||
       readSdpAttribute(answerSdp, "a=ice-pwd:", remotePassword, sizeof remotePassword) != 0) {
      logError(TAG "ice: the machine's answer carried no credentials\n");
      return -1;
   }

   // this is what the encrypted connection will be checked against, so an answer without it is
   // one we cannot safely use
   if (readSdpAttribute(answerSdp, "a=fingerprint:sha-256 ", remoteFingerprint, sizeof remoteFingerprint) != 0) {
      logError(TAG "ice: the machine's answer did not say what its certificate would be\n");
      return -1;
   }

   // a credential that came out too long means the answer was not split where it was expected to be
   logInfo(TAG "ice: the machine calls itself %s, with a %d character password\n", remoteUsername,
           getStrLen(remotePassword));
   return 0;
}

// the address and port out of "candidate:1 1 UDP 100 1.2.3.4 5678 typ host"
static int readCandidateAddress(const char *candidate, uint32_t *address, uint16_t *port)
{
   char text[CANDIDATE_TEXT_MAX];
   strCopy(text, sizeof text, candidate);

   // the address is the fifth space-separated field and the port the sixth
   char *fields[8];
   int fieldCount = 0;
   for (char *at = text; *at && fieldCount < 8;) {
      while (*at == ' ') at++;
      if (!*at) break;
      fields[fieldCount++] = at;
      while (*at && *at != ' ') at++;
      if (*at) *at++ = 0;
   }
   if (fieldCount < 6) return -1;

   struct in_addr parsed;
   if (inet_pton(AF_INET, fields[4], &parsed) != 1) return -1;   // an IPv6 address is not one we can use
   *address = ntohl(parsed.s_addr);

   int value = 0;
   for (const char *digit = fields[5]; *digit >= '0' && *digit <= '9'; digit++) value = value * 10 + (*digit - '0');
   if (value <= 0 || value > 65535) return -1;
   *port = (uint16_t)value;
   return 0;
}

// answers one check the machine sent us, signed with our own password so it can tell it was us
static void answerCheck(const uint8_t *message, int length, const struct sockaddr_in *from)
{
   StunIncomingRequest incoming;
   if (readStunRequest(message, length, &incoming) != 0) return;

   uint8_t reply[STUN_MESSAGE_MAX];
   int replyLength = buildStunResponse(reply, sizeof reply, &incoming, password, ntohl(from->sin_addr.s_addr),
                                       ntohs(from->sin_port));
   if (replyLength < 0) return;

   sendto(iceSocket, reply, replyLength, 0, (const struct sockaddr *)from, sizeof *from);
   if (incoming.nominated && !machineNominated) {
      machineNominated = 1;
      logInfo(TAG "ice: the machine picked this address to use\n");
   }
}

// sends one check to the machine's address at `index`
static int sendCheck(int index, const char *checkName, uint64_t tiebreaker, int nominate, StunPendingRequest *request)
{
   uint32_t address = 0;
   uint16_t port = 0;
   if (readCandidateAddress(remoteCandidates[index], &address, &port) != 0) return -1;

   uint8_t message[STUN_MESSAGE_MAX];
   int length = buildStunRequest(message, sizeof message, checkName, remotePassword, CHECK_PRIORITY, tiebreaker,
                                 nominate, request);
   if (length < 0) return -1;

   struct sockaddr_in target;
   memSet(&target, 0, sizeof target);
   target.sin_family = AF_INET;
   target.sin_addr.s_addr = htonl(address);
   target.sin_port = htons(port);
   return sendto(iceSocket, message, length, 0, (struct sockaddr *)&target, sizeof target) < 0 ? -1 : 0;
}

int negotiateXcloudConnection(void)
{
   if (iceSocket < 0 || !remoteUsername[0]) return -1;

   // the far end checks this name against what it told us, so its own comes first
   char checkName[ICE_USERNAME_MAX * 2 + 4];
   snprintf(checkName, sizeof checkName, "%s:%s", remoteUsername, username);

   uint64_t tiebreaker = 0;
   sys_get_random_number(&tiebreaker, sizeof tiebreaker);

   useReceiveTimeout(POLL_TIMEOUT_MS);
   machineNominated = 0;

   int theyAnswered = 0, weAnswered = 0, weNominated = 0, workingIndex = -1;
   StunPendingRequest request;
   int haveRequest = 0;

   // both directions have to work, so this keeps sending ours and answering theirs until each has
   // happened at least once. the machine checks us as much as we check it.
   for (int round = 0; round < NEGOTIATE_ROUNDS && !(theyAnswered && weAnswered && weNominated); round++) {
      // before an address is known to work, try each in turn. after that, keep checking the one
      // that did, marking it as the address to use: we declared ourselves in charge of choosing.
      int target = workingIndex >= 0 ? workingIndex
                                     : round % (remoteCandidateCount > 0 ? remoteCandidateCount : 1);
      int nominating = workingIndex >= 0;
      haveRequest = sendCheck(target, checkName, tiebreaker, nominating, &request) == 0;
      if (nominating && haveRequest) weNominated = 1;
      uint64_t sentAt = getTimeUs();

      uint8_t incoming[STUN_MESSAGE_MAX];
      struct sockaddr_in from;
      socklen_t fromLength = sizeof from;
      int length = recvfrom(iceSocket, incoming, sizeof incoming, 0, (struct sockaddr *)&from, &fromLength);
      if (length <= 0) continue;

      // one socket carries both, so what arrived decides which it is
      if (haveRequest) {
         uint32_t seenAddress = 0;
         uint16_t seenPort = 0;
         StunError error;
         StunReplyKind kind = readStunReply(incoming, length, &request, &seenAddress, &seenPort, &error);
         if (kind == STUN_REPLY_SUCCESS) {
            chosenAddress = from;
            if (!theyAnswered) {
               char seenText[ICE_ADDRESS_MAX];
               formatIpv4(seenText, sizeof seenText, seenAddress);
               logInfo(TAG "ice: the machine answered us, it sees us at %s:%d\n", seenText, seenPort);
            }
            // the time to the machine and back, which is the part of the delay that is the
            // network rather than either end
            roundTripUs = getTimeUs() - sentAt;
            theyAnswered = 1;
            workingIndex = target;
            continue;
         }
         if (kind == STUN_REPLY_ERROR) {
            logWarn(TAG "ice: the machine refused our check: %d %s\n", error.code, error.reason);
            continue;
         }
      }

      answerCheck(incoming, length, &from);
      if (!weAnswered) {
         weAnswered = 1;
         logInfo(TAG "ice: answered the machine's first check\n");
      }
   }

   if (roundTripUs > 0) logInfo(TAG "ice: the machine is %d ms away, there and back\n", (int)(roundTripUs / 1000));
   logInfo(TAG "ice: we reached it %s, it reached us %s, we chose the address %s\n", theyAnswered ? "yes" : "no",
           weAnswered ? "yes" : "no", weNominated ? "yes" : "no");
   return theyAnswered && weAnswered && weNominated ? 0 : -1;
}

const char *getXcloudRemoteFingerprint(void) { return remoteFingerprint; }

void answerXcloudCheck(const void *packet, int length)
{
   answerCheck((const uint8_t *)packet, length, &chosenAddress);
}

int sendXcloudDatagram(const void *data, int length)
{
   if (iceSocket < 0) return -1;
   return sendto(iceSocket, data, length, 0, (const struct sockaddr *)&chosenAddress, sizeof chosenAddress);
}

int receiveXcloudDatagram(void *buffer, int capacity, int timeoutMs)
{
   if (iceSocket < 0) return -1;
   useReceiveTimeout(timeoutMs);
   return recvfrom(iceSocket, buffer, capacity, 0, 0, 0);
}
