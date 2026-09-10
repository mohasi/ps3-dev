// xcloud-screen - see xcloud-screen.h.
//
// Signing in and starting a session are done one after another on this thread, so the screen
// holds still for as long as a request takes. That is visible but harmless while there is nothing
// on screen but a line of text.
//
// Once there is a connection the work moves to a thread of its own, because from then on the
// screen has a picture to keep up with. See runConnection.

#include "xcloud-screen.h"

#include "app.h"
#include "colors.h"
#include "dbg.h"
#include "dtls.h"
#include "dtls-certificate.h"
#include "gfx.h"
#include "pad.h"
#include "printf.h"
#include "rtcp.h"
#include "sctp.h"
#include "screen-chrome.h"
#include "srtp.h"
#include "stun.h"
#include "thread.h"
#include "title-select.h"
#include "ui/label.h"
#include "xcloud-api.h"
#include "xcloud-auth.h"
#include "xcloud-catalog.h"
#include "xcloud-channels.h"
#include "xcloud-ice.h"
#include "xcloud-media.h"

#define CODE_SIZE    72
#define MESSAGE_SIZE 26
#define LINE_GAP     42

#define POLL_INTERVAL_US 5000000   // Microsoft asks for 5 seconds between polls
#define STATUS_MAX       192
#define DTLS_DATAGRAM_MAX 1500   // one datagram on an ordinary network
#define DTLS_WAIT_MS      500
#define DTLS_ATTEMPTS     40     // 20 seconds at the wait above, well past any reasonable exchange
#define CONNECTION_WAIT_MS 4   // short, so the controller is read and sent often enough to play with
#define SESSION_POLL_INTERVAL_US 1000000   // the session's own state changes faster than a sign-in

static const char *TITLE = "Xbox Cloud";

static const char *getStateMessage(XcloudAuthState state)
{
   switch (state) {
   case XCLOUD_AUTH_IDLE:             return "Signing in...";
   case XCLOUD_AUTH_WAITING_FOR_USER: return "On a phone or PC, open microsoft.com/link and enter:";
   case XCLOUD_AUTH_SIGNED_IN:        return "Signed in. Asking for a streaming pass...";
   case XCLOUD_AUTH_FAILED:           return getXcloudAuthError();
   }
   return "";
}

static const char *getSessionMessage(XcloudSessionState state)
{
   switch (state) {
   case XCLOUD_SESSION_PROVISIONING: return "Getting a machine ready...";
   case XCLOUD_SESSION_WAITING:      return "Queued, waiting for a free machine...";
   case XCLOUD_SESSION_AUTHORIZING:  return "Proving who we are...";
   case XCLOUD_SESSION_READY:        return "The machine is ready, connecting...";
   case XCLOUD_SESSION_NONE:
   case XCLOUD_SESSION_FAILED:       break;
   }
   return "";
}

// what the account can play, fetched once: it does not change while the app is open, and coming
// back from a game should show the list at once rather than asking for all of it again
static int loadLibrary(Label *message)
{
   if (fetchXcloudStreamingToken() != 0) { setLabelText(message, getXcloudAuthError()); return -1; }
   if (fetchXcloudTitles() < 0) { setLabelText(message, getXcloudApiError()); return -1; }

   // names and art are a nicety: the list still works from identifiers if the store is unreachable
   setLabelText(message, "Looking up the games...");
   fetchXcloudCatalog();
   return 0;
}

// picks what to play and asks for it; the message says how far it got. returns 0 once a session
// is on its way, or -1 if there is nothing to wait for.
static int startSession(Label *message, Font *font)
{
   int picked = runTitleSelectScreen(font);
   if (picked < 0) { setLabelText(message, "Nothing chosen"); return -1; }

   const XcloudTitle *chosen = getXcloudTitle(picked);
   if (!chosen) { setLabelText(message, "This account has nothing it can stream"); return -1; }

   if (startXcloudSession(chosen->id) != 0) { setLabelText(message, getXcloudApiError()); return -1; }

   char line[STATUS_MAX];
   snprintf(line, sizeof line, "Starting %s", chosen->id);
   setLabelText(message, line);
   return 0;
}

// runs the encrypted setup to its end. datagrams go missing, so anything unanswered goes again.
static int startEncryption(void)
{
   if (startDtls(sendXcloudDatagram, getXcloudRemoteFingerprint()) != 0) return -1;

   for (int attempt = 0; attempt < DTLS_ATTEMPTS; attempt++) {
      uint8_t incoming[DTLS_DATAGRAM_MAX];
      int length = receiveXcloudDatagram(incoming, sizeof incoming, DTLS_WAIT_MS);
      if (length <= 0) { resendDtls(); continue; }
      if (!isDtlsDatagram(incoming, length)) continue;

      DtlsProgress progress = feedDtls(incoming, length);
      if (progress == DTLS_HANDSHAKE_DONE) return 0;
      if (progress == DTLS_HANDSHAKE_FAILED) return -1;
   }
   return -1;
}

// Everything on the connection happens on its own thread. It has to: putting a picture on screen
// waits for the display, and the socket cannot go unread for that long without losing packets.
// So this thread only ever reads the socket and decodes, and the screen thread only ever draws.
//
// Sending is all done here too. The counters underneath the encrypted connection and the message
// channel are not safe to advance from two threads, so the controller goes out from here as well,
// reading the pad directly rather than through the screen thread.
static volatile int connectionStopping;
static volatile int connectionRunning;

static void runConnectionThread(uint64_t unused)
{
   (void)unused;

   int media = 0, refused = 0, reports = 0, reportsSent = 0;
   int channelsAsked = 0;
   int64_t mediaBytes = 0;
   uint64_t firstMediaUs = 0;

   // The keys exist the moment the encrypted setup finishes, so they are taken now. Opening them
   // on the first message of the association instead meant anything the machine sent in between
   // was checked against nothing and refused, and a failure part way through left them being
   // opened a second time, which starts the report numbering again under a key already used.
   const uint8_t *keys = getDtlsMediaKeys();
   openSrtp(keys + DTLS_MEDIA_KEY_SIZE, keys + 2 * DTLS_MEDIA_KEY_SIZE + DTLS_MEDIA_SALT_SIZE);
   openSrtcpSend(keys, keys + 2 * DTLS_MEDIA_KEY_SIZE);
   openSrtcpRead(keys + DTLS_MEDIA_KEY_SIZE, keys + 2 * DTLS_MEDIA_KEY_SIZE + DTLS_MEDIA_SALT_SIZE);
   openRtcp();

   while (!connectionStopping) {
      sendXcloudControllerState();

      // On the clock, not on arrival. What the machine sends depends on being told what arrived,
      // so going quiet exactly while nothing arrives leaves it guessing at the worst moment.
      uint8_t report[128];
      int reportLength = getRtcpReport(getTimeUs(), report, sizeof report);
      if (reportLength > 0) { sendXcloudDatagram(report, reportLength); reportsSent++; }

      uint8_t incoming[DTLS_DATAGRAM_MAX];
      int length = receiveXcloudDatagram(incoming, sizeof incoming, CONNECTION_WAIT_MS);
      if (length <= 0) continue;

      if (isXcloudCheck(incoming, length)) {
         answerXcloudCheck(incoming, length);
         continue;
      }

      if (isDtlsDatagram(incoming, length)) {
         uint8_t content[DTLS_DATAGRAM_MAX];
         int held = readDtlsData(incoming, length, content, sizeof content);
         if (held <= 0) continue;

         SctpMessage messages[SCTP_MESSAGES_MAX];
         int found = feedSctp(content, held, messages, SCTP_MESSAGES_MAX);
         for (int index = 0; index < found; index++) advanceXcloudChannels(&messages[index]);

         // the machine opens the association, and the channels are ours to ask for once it has
         if (isSctpOpen() && !channelsAsked) channelsAsked = openXcloudChannels() == 0;
         pollXcloudChannels();
         continue;
      }

      // section: the picture and sound, which travel beside the connection rather than inside it

      // the machine's own reports come the same way. what they say about its clock goes back in
      // ours, which is how it works out how long the round trip takes.
      if (!isSrtpMedia(incoming, length)) {
         reports++;
         int reportLength = readSrtcpPacket(incoming, length);
         if (reportLength > 0) noteRtcpSenderReport(incoming, reportLength, getTimeUs());
         continue;
      }

      int plainLength = readSrtpPacket(incoming, length);
      if (plainLength < 0) { refused++; continue; }

      media++;
      mediaBytes += length;

      uint64_t arrivedAt = getTimeUs();
      if (!firstMediaUs) firstMediaUs = arrivedAt;
      noteRtcpPacket(incoming, plainLength, arrivedAt);

      feedXcloudMedia(incoming, plainLength);
   }

   logInfo("[cst] connection: %d media read, %d refused, %d reports in, %d out\n", media, refused, reports,
           reportsSent);

   // what the machine actually sent, which is what the reports above are there to raise
   uint64_t mediaUs = firstMediaUs ? getTimeUs() - firstMediaUs : 0;
   if (mediaUs > 0)
      logInfo("[cst] media rate: %d kbps over %d seconds\n", (int)(mediaBytes * 8 / (int64_t)(mediaUs / 1000)),
              (int)(mediaUs / 1000000));
   reportXcloudMedia();

   connectionRunning = 0;
   exitThread();
}

// the screen's half: draw what has been decoded, and read the controller for the other thread
static void runConnection(void)
{
   if (openSctp(sendDtlsData) != 0) return;
   openXcloudMedia();

   connectionStopping = 0;
   connectionRunning = 1;

   sys_ppu_thread_t worker;
   if (spawnThread(&worker, runConnectionThread, 0, THREAD_PRIORITY_HIGH, THREAD_STACK_SIZE_64KB,
                   "xcloud-net") != 0) {
      logError("[cst] could not start the connection thread\n");
      connectionRunning = 0;
      closeXcloudMedia();
      return;
   }

   while (!appExitRequested) {
      appPoll();
      updatePad();

      // every button now belongs to the game, so leaving takes a pair no game asks for at once
      if (isPadButtonDown(PAD_BTN_START) && isPadButtonDown(PAD_BTN_SELECT)) break;

      beginGfxFrame();
      clearGfx(COLOR_BLACK);
      drawXcloudMedia();
      endGfxFrame();
   }

   // the thread is holding the socket and the decoders, so nothing is torn down until it is out
   connectionStopping = 1;
   while (connectionRunning) sleepMs(4);

   closeXcloudMedia();
}

// offers the machine a connection and finds out where it can be reached
static void connectToMachine(Label *message)
{
   if (openXcloudIce() != 0) { setLabelText(message, "Could not open a connection here"); return; }
   if (exchangeXcloudSdp(getXcloudOffer()) != 0) { setLabelText(message, getXcloudApiError()); return; }

   if (readXcloudRemoteCredentials(getXcloudSdpAnswer()) != 0) {
      setLabelText(message, "The machine's answer was missing something");
      return;
   }

   int found = exchangeXcloudIceCandidates();
   if (found < 0) { setLabelText(message, getXcloudApiError()); return; }

   setLabelText(message, "Agreeing a connection...");
   if (negotiateXcloudConnection() != 0) { setLabelText(message, "Could not agree a connection"); return; }

   setLabelText(message, "Setting up encryption...");
   if (startEncryption() != 0) { setLabelText(message, "Could not set up encryption"); return; }

   setLabelText(message, "Encrypted.");
   runConnection();
}

void runXcloudScreen(Font *font)
{
   Label title, message, code;
   initLabelRaw(&title, font, 0, 0, AUTO, AUTO, TITLE_SIZE, COLOR_WHITE, TEXT_NOWRAP, TITLE);
   initLabelRaw(&message, font, 0, 0, AUTO, AUTO, MESSAGE_SIZE, TEXT_DIM, TEXT_NOWRAP, "Signing in...");
   initLabelRaw(&code, font, 0, 0, AUTO, AUTO, CODE_SIZE, COLOR_WHITE, TEXT_NOWRAP, "");

   // the first frame goes up before the request, so the screen is never blank while it runs
   beginGfxFrame();
   clearGfx(COLOR_BLACK);
   drawLabelCentered(&title, getGfxScreenWidth() / 2, TITLE_Y);
   drawLabelCentered(&message, getGfxScreenWidth() / 2, getGfxScreenHeight() / 2);
   endGfxFrame();

   runStunSelfTest();   // the checks below are worthless if the hash behind them is wrong
   runSctpSelfTest();
   runSrtpSelfTest();

   // the certificate belongs to the console, not to any one session, and the offer names its
   // fingerprint. making it here means it is ready before anything needs it, and that it can be
   // looked at without waiting for a machine to be free.
   createDtlsCertificate();

   loadXcloudStreamHeight();   // the size chosen last time, before the list can show it
   startXcloudSignIn();

   int shownAuthState = -1;    // nothing drawn yet, so the first pass always sets the text
   int shownSessionState = -1;
   int sessionRequested = 0;
   int connectStarted = 0;
   int libraryLoaded = 0;
   uint64_t nextPollAt = getTimeUs() + POLL_INTERVAL_US;
   int leaving = 0;

   while (!leaving && !appExitRequested) {
      appPoll();
      updatePad();
      if (isPadButtonPressed(PAD_BTN_START) || isPadButtonPressed(PAD_BTN_CIRCLE)) leaving = 1;

      XcloudAuthState authState = getXcloudAuthState();

      // section: sign in, then ask for a machine to play on
      if (authState != XCLOUD_AUTH_SIGNED_IN && getTimeUs() >= nextPollAt) {
         pollXcloudSignIn();
         nextPollAt = getTimeUs() + POLL_INTERVAL_US;
      }

      if ((int)authState != shownAuthState) {
         setLabelText(&message, getStateMessage(authState));
         setLabelText(&code, authState == XCLOUD_AUTH_WAITING_FOR_USER ? getXcloudUserCode() : "");
         shownAuthState = (int)authState;
      }

      // backing out of the game list leaves this screen too: there is nothing else here to do
      if (authState == XCLOUD_AUTH_SIGNED_IN && !sessionRequested) {
         if (!libraryLoaded && loadLibrary(&message) != 0) leaving = 1;
         else libraryLoaded = 1;

         if (libraryLoaded) {
            sessionRequested = 1;
            if (startSession(&message, font) != 0) leaving = 1;
         }
      }

      // section: wait for that machine to be ready
      //
      // asking stops once the connection is up: the state endpoint has nothing left to say, and a
      // request a second to a remote service for an answer nobody reads is waste. what the session
      // will need instead is an occasional keepalive, once there is a connection worth keeping.
      if (!connectStarted && getXcloudSessionState() != XCLOUD_SESSION_NONE && getTimeUs() >= nextPollAt) {
         pollXcloudSession();
         nextPollAt = getTimeUs() + SESSION_POLL_INTERVAL_US;
      }

      XcloudSessionState sessionState = getXcloudSessionState();

      // section: once there is a machine, work out how to reach it
      if (sessionState == XCLOUD_SESSION_READY && !connectStarted) {
         connectStarted = 1;
         connectToMachine(&message);   // does not return until the game is left or it could not connect

         // The game is over. Give the machine back and offer the list again, so another can be
         // started without signing in from the beginning. The list is already in hand, so this
         // comes straight back up.
         closeXcloudIce();
         stopXcloudSession();
         sessionRequested = 0;
         connectStarted = 0;
         shownSessionState = -1;
         nextPollAt = getTimeUs();
         continue;
      }

      if ((int)sessionState != shownSessionState) {
         if (sessionState != XCLOUD_SESSION_NONE)
            setLabelText(&message, sessionState == XCLOUD_SESSION_FAILED ? getXcloudApiError()
                                                                        : getSessionMessage(sessionState));
         shownSessionState = (int)sessionState;
      }

      beginGfxFrame();
      clearGfx(COLOR_BLACK);
      int centerX = getGfxScreenWidth() / 2;
      drawLabelCentered(&title, centerX, TITLE_Y);

      int y = (getGfxScreenHeight() - MESSAGE_SIZE - LINE_GAP - CODE_SIZE) / 2;
      drawLabelCentered(&message, centerX, y);
      if (authState == XCLOUD_AUTH_WAITING_FOR_USER) drawLabelCentered(&code, centerX, y + MESSAGE_SIZE + LINE_GAP);
      endGfxFrame();

      sleepMs(16);
   }

   closeXcloudIce();
   stopXcloudSession();   // a session left running keeps a machine busy and the account's clock ticking

   freeLabel(&title);
   freeLabel(&message);
   freeLabel(&code);
}
