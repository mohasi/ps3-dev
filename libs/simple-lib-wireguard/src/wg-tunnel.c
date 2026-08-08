#include "wg-tunnel.h"

#include <sys/sys_time.h>

#include "dbg.h"
#include "string-utilities.h"
#include "wg-bytes.h"
#include "wg-ip.h"   // answering a ping is host behaviour, so the tunnel does it

#define TAG "[wg] "

// The timing rules of the protocol, whitepaper section 6.5. All in milliseconds.
#define REKEY_AFTER_MS     120000   // start a new handshake once a session reaches this age
#define REJECT_AFTER_MS    180000   // never use keys older than this
#define REKEY_TIMEOUT_MS     5000   // resend an unanswered initiation after this long
#define REKEY_GIVE_UP_MS    90000   // stop retrying a handshake after this long
#define KEEPALIVE_TIMEOUT_MS 10000  // the longest either side may stay silent, even with nothing to say
#define PREVIOUS_SESSION_MS  15000  // how long the keys we just replaced still open arriving packets

#define RECEIVE_BUFFER_LENGTH 2048

static uint64_t getMillisecondsNow(void)
{
   return sys_time_get_system_time() / 1000;   // the system clock counts microseconds since boot
}

WgTunnelState getWgTunnelState(const WgTunnel *tunnel)
{
   return tunnel->state;
}

const char *getWgTunnelStateName(const WgTunnel *tunnel)
{
   if (tunnel->state == WG_TUNNEL_UP) return "up";
   if (tunnel->state == WG_TUNNEL_CONNECTING) return "connecting";
   return "down";
}

int openWgTunnel(WgTunnel *tunnel, const WgConfig *config)
{
   memSet(tunnel, 0, sizeof *tunnel);
   tunnel->config = *config;

   uint32_t serverAddress = 0;
   if (parseIpv4Text(config->endpointHost, &serverAddress) != 0) {
      logError(TAG "tunnel: the endpoint is a name rather than an address, and looking names up is not built yet\n");
      return -1;
   }

   if (openWgTransport(&tunnel->transport, serverAddress, config->endpointPort) != 0) return -1;

   tunnel->state = WG_TUNNEL_DOWN;
   return 0;
}

void closeWgTunnel(WgTunnel *tunnel)
{
   closeWgTransport(&tunnel->transport);
   memSet(&tunnel->session, 0, sizeof tunnel->session);
   memSet(&tunnel->pending, 0, sizeof tunnel->pending);
   memSet(&tunnel->config, 0, sizeof tunnel->config);   // it holds the private key
   tunnel->state = WG_TUNNEL_DOWN;
}

// build and send a fresh initiation. the handshake state it produces replaces any earlier attempt.
static int sendInitiation(WgTunnel *tunnel, uint64_t now)
{
   uint8_t initiation[WG_INITIATION_LENGTH];
   if (createWgInitiation(&tunnel->pending, &tunnel->config, initiation) != 0) return -1;
   if (sendWgDatagram(&tunnel->transport, initiation, sizeof initiation) < 0) return -1;

   tunnel->initiationSentMs = now;
   tunnel->handshakeAttempts++;
   if (tunnel->state == WG_TUNNEL_DOWN) tunnel->state = WG_TUNNEL_CONNECTING;
   return 0;
}

static int sendKeepalive(WgTunnel *tunnel, uint64_t now)
{
   // a keepalive is a data message carrying no packet at all
   uint8_t message[WG_DATA_OVERHEAD];
   int length = sealWgPacket(&tunnel->session, message, 0, message, sizeof message);
   if (length < 0 || sendWgDatagram(&tunnel->transport, message, length) < 0) return -1;

   tunnel->lastSentMs = now;
   return 0;
}

// a completed handshake becomes the live session, and the one it replaces stays usable for a
// while so packets already in flight under the old keys still open
static void adoptHandshake(WgTunnel *tunnel, uint64_t now)
{
   if (tunnel->state == WG_TUNNEL_UP) {
      tunnel->previousSession = tunnel->session;
      tunnel->hasPreviousSession = 1;
      tunnel->previousExpiresMs = now + PREVIOUS_SESSION_MS;
   }

   startWgSession(&tunnel->session, &tunnel->pending);
   tunnel->state = WG_TUNNEL_UP;
   tunnel->isRekeying = 0;
   tunnel->handshakeAttempts = 0;
   tunnel->sessionStartedMs = now;
   tunnel->lastSentMs = now;
   tunnel->handshakesCompleted++;
   logTrace(TAG "tunnel: session %llu established, index 0x%x\n", (unsigned long long)tunnel->handshakesCompleted,
           tunnel->session.senderIndex);
   resetWgReplayWindow(&tunnel->session.received);
}

// decide what has to happen before we wait for traffic: expiry, rekey, retry, keepalive
static int runTimers(WgTunnel *tunnel, uint64_t now)
{
   // the replaced keys stop being kept once nothing can still be in flight under them
   if (tunnel->hasPreviousSession && now >= tunnel->previousExpiresMs) {
      memSet(&tunnel->previousSession, 0, sizeof tunnel->previousSession);
      tunnel->hasPreviousSession = 0;
   }

   // keys past their limit stop being used, whatever else is going on
   if (tunnel->state == WG_TUNNEL_UP && now - tunnel->sessionStartedMs >= REJECT_AFTER_MS) {
      logWarn(TAG "tunnel: session expired before a new one was ready, traffic stops until it reconnects\n");
      memSet(&tunnel->session, 0, sizeof tunnel->session);
      tunnel->state = WG_TUNNEL_DOWN;
   }

   if (tunnel->state == WG_TUNNEL_DOWN) return sendInitiation(tunnel, now);

   // a handshake in flight is resent until it is answered, then abandoned
   int handshakeInFlight = tunnel->state == WG_TUNNEL_CONNECTING || tunnel->isRekeying;
   if (handshakeInFlight && now - tunnel->initiationSentMs >= REKEY_TIMEOUT_MS) {
      // A run of unanswered attempts means the network went away rather than that the config is
      // wrong, and it can come back, so the keys are dropped and the count starts again instead of
      // the tunnel being abandoned. Nothing is sent while it is down, so this cannot leak traffic.
      if ((uint64_t)tunnel->handshakeAttempts * REKEY_TIMEOUT_MS >= REKEY_GIVE_UP_MS) {
         logWarn(TAG "tunnel: no answer in %ds, dropping the session and starting again\n", REKEY_GIVE_UP_MS / 1000);
         memSet(&tunnel->session, 0, sizeof tunnel->session);
         tunnel->handshakeAttempts = 0;
         tunnel->isRekeying = 0;
         tunnel->state = WG_TUNNEL_DOWN;
      }
      logWarn(TAG "tunnel: no answer in %dms, sending another initiation\n", REKEY_TIMEOUT_MS);
      return sendInitiation(tunnel, now);
   }

   if (tunnel->state != WG_TUNNEL_UP) return 0;

   // start the replacement early, while the current keys still work
   if (!tunnel->isRekeying && now - tunnel->sessionStartedMs >= REKEY_AFTER_MS) {
      logTrace(TAG "tunnel: session is %ds old, starting a new handshake\n", REKEY_AFTER_MS / 1000);
      tunnel->isRekeying = 1;
      tunnel->handshakeAttempts = 0;
      return sendInitiation(tunnel, now);
   }

   // Never be silent for more than ten seconds. The other end gives up on a session fifteen seconds
   // after it last sent with nothing coming back, and rebuilds it, so silence in either direction
   // has to be broken before then. The configured interval may only make this shorter: measured
   // against the provider's twenty five seconds, the server rebuilt the session after nine of the
   // ten exchanges in a ten minute run.
   uint64_t keepaliveMs = (uint64_t)tunnel->config.keepaliveSeconds * 1000;
   if (keepaliveMs == 0 || keepaliveMs > KEEPALIVE_TIMEOUT_MS) keepaliveMs = KEEPALIVE_TIMEOUT_MS;
   if (now - tunnel->lastSentMs >= keepaliveMs) return sendKeepalive(tunnel, now);

   return 0;
}

int serviceWgTunnel(WgTunnel *tunnel, uint8_t *packet, int capacity, int waitMs, int *receivedSomething)
{
   if (receivedSomething) *receivedSomething = 0;

   uint64_t now = getMillisecondsNow();
   if (runTimers(tunnel, now) != 0) return -1;

   uint8_t message[RECEIVE_BUFFER_LENGTH];
   int received = receiveWgDatagram(&tunnel->transport, message, sizeof message, waitMs);
   if (received <= 0) return received;   // 0 is an ordinary quiet moment, -1 a socket failure

   if (receivedSomething) *receivedSomething = 1;
   now = getMillisecondsNow();

   // a handshake answer completes whichever handshake is outstanding
   if (message[0] == WG_MESSAGE_RESPONSE) {
      if (processWgResponse(&tunnel->pending, message, received) == 0) adoptHandshake(tunnel, now);
      return 0;
   }

   // the server can start a handshake of its own, and a peer that ignores those cannot receive
   // anything the server wants to start sending
   if (message[0] == WG_MESSAGE_INITIATION) {
      WgHandshake answered;
      if (processWgInitiation(&answered, &tunnel->config, message, received, tunnel->lastPeerTimestamp) != 0)
         return 0;

      uint8_t response[WG_RESPONSE_LENGTH];
      if (createWgResponse(&answered, response) != 0) return 0;
      if (sendWgDatagram(&tunnel->transport, response, sizeof response) < 0) return 0;

      tunnel->pending = answered;
      adoptHandshake(tunnel, now);
      tunnel->handshakesAnswered++;
      return 0;
   }

   if (message[0] == WG_MESSAGE_COOKIE) {
      logWarn(TAG "tunnel: server sent a cookie reply, it is under load or rejected our authenticator\n");
      return 0;
   }

   if (tunnel->state != WG_TUNNEL_UP) {
      logWarn(TAG "tunnel: data arrived with no session to open it, dropped\n");
      return 0;
   }

   // pick the keys the sender used: every session carries the index it was given, and a packet
   // says which one it belongs to
   WgSession *session = &tunnel->session;
   uint32_t wantedIndex = load32le(message + 4);
   if (wantedIndex != tunnel->session.senderIndex) {
      if (!tunnel->hasPreviousSession || wantedIndex != tunnel->previousSession.senderIndex) {
         logWarn(TAG "tunnel: a packet arrived for a session we no longer have, dropped\n");
         return 0;
      }
      session = &tunnel->previousSession;
   }

   int length = openWgPacket(session, message, received, packet, capacity);
   if (length < 0) return 0;   // the reason was already reported

   // a keepalive still counts as hearing from the server, and still has to be answered
   tunnel->lastReceivedMs = now;
   if (length == 0) return 0;

   tunnel->packetsReceived++;

   // a ping is answered here rather than passed up: the gateway pings us to check we are alive,
   // and that is the tunnel's business, not the app's
   int replyLength = buildPingReply(packet, length);
   if (replyLength > 0) {
      sendThroughWgTunnel(tunnel, packet, replyLength);
      tunnel->pingsAnswered++;
      return 0;
   }

   return length;
}

int sendThroughWgTunnel(WgTunnel *tunnel, const uint8_t *packet, int length)
{
   if (tunnel->state != WG_TUNNEL_UP) return -1;

   uint8_t message[RECEIVE_BUFFER_LENGTH];
   int sealedLength = sealWgPacket(&tunnel->session, packet, length, message, sizeof message);
   if (sealedLength < 0 || sendWgDatagram(&tunnel->transport, message, sealedLength) < 0) return -1;

   tunnel->lastSentMs = getMillisecondsNow();
   tunnel->packetsSent++;
   return 0;
}
