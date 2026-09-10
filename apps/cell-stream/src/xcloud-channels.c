// xcloud-channels - see xcloud-channels.h.
//
// The messages are JSON, written out as text rather than built by a library: there are a fixed
// handful of them and none is nested deeply enough to be worth the machinery.
//
// The channel numbers are ours to choose. Even numbers belong to the side that started the
// encryption, which is this console.

#include "xcloud-channels.h"

#include "cell-stream-settings.h"
#include "dbg.h"
#include "json.h"
#include "printf.h"
#include "thread.h"   // getTimeUs
#include "settings-file.h"
#include "xcloud-input.h"
#include "string-utilities.h"

#define TAG "[cst] "

#define CHANNEL_CONTROL 0
#define CHANNEL_INPUT   2
#define CHANNEL_MESSAGE 4
#define CHANNEL_CHAT    6

#define MESSAGE_MAX 640

// the machine checks this against a list of clients it knows, so it is the value a browser sends
// rather than anything of ours
#define ACCESS_KEY "4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E"

// What this console can show.
//
// The frame rate asked for is ignored: the machine sends sixty a second whatever this says. The
// size is honoured.
//
// The bitrate is what a game looks like rather than whether it plays. Twelve megabits is what
// Microsoft's own client asks for at this size, and this console is on a wired network where that
// is nothing. It was two, carried over from a handheld with a small screen, and at that figure a
// busy scene visibly falls apart.
#define STREAM_FPS     30
#define STREAM_KBPS    12000

// Asked for at whichever size is chosen on the game list. The console outputs 1080p, so 720
// is stretched to fill it; the larger size is sharper but far more for the decoder to keep up
// with, which is why it is a choice rather than a fixed value.
static int streamHeight = 720;

#define CONTROLLER_INTERVAL_US 16000   // about sixty times a second, the same as the picture

static int greetingSent;
static int ready;
static int inputStarted;
static uint64_t lastControllerSentAt;

static int sendText(int channel, const char *text)
{
   return sendSctpMessage(channel, SCTP_PAYLOAD_TEXT, text, getStrLen(text));
}

// every message on the message channel is wrapped the same way, with the real content as a string
// inside it rather than as nested JSON
static int sendWrapped(const char *target, const char *content)
{
   char message[MESSAGE_MAX];
   snprintf(message, sizeof message,
            "{\"type\":\"Message\",\"content\":\"%s\",\"id\":\"%s\",\"target\":\"%s\",\"cv\":\"\"}",
            content, "be0bfc6d-1e83-4c8a-90ed-fa8601c5a180", target);
   return sendText(CHANNEL_MESSAGE, message);
}

// the JSON above puts content inside a string, so the quotes within it have to be marked as part
// of the text rather than as the string ending
static void escapeQuotes(char *out, int capacity, const char *text)
{
   int at = 0;
   for (const char *from = text; *from && at < capacity - 3; from++) {
      if (*from == '"') out[at++] = '\\';
      out[at++] = *from;
   }
   out[at] = 0;
}

static int sendDescription(const char *target, const char *content)
{
   char escaped[MESSAGE_MAX];
   escapeQuotes(escaped, sizeof escaped, content);
   return sendWrapped(target, escaped);
}

// tells the machine what this console can display, so it does not send a picture too large or too
// fast to decode
static void sendClientDescription(void)
{
   char content[MESSAGE_MAX];
   int width = getXcloudStreamHeight() * 16 / 9;

   snprintf(content, sizeof content,
            "{\"supportsCustomResolution\":true,\"supportsHevc\":false,\"supportsHdr\":false,"
            "\"supportsFps\":%d,\"maxWidth\":%d,\"maxHeight\":%d,\"maxBitrateKbps\":%d,"
            "\"video\":{\"width\":%d,\"height\":%d,\"maxWidth\":%d,\"maxHeight\":%d,\"maxBitrateKbps\":%d}}",
            STREAM_FPS, width, streamHeight, STREAM_KBPS, width, streamHeight, width, streamHeight,
            STREAM_KBPS);
   sendDescription("/streaming/characteristics/clientdevicecapabilities", content);

   snprintf(content, sizeof content,
            "{\"horizontal\":%d,\"vertical\":%d,\"preferredWidth\":%d,\"preferredHeight\":%d,"
            "\"safeAreaLeft\":0,\"safeAreaTop\":0,\"safeAreaRight\":%d,\"safeAreaBottom\":%d,"
            "\"supportsCustomResolution\":true}",
            width, streamHeight, width, streamHeight, width, streamHeight);
   sendDescription("/streaming/characteristics/dimensionschanged", content);

   sendDescription("/streaming/characteristics/touchinputenabledchanged", "{\"touchInputEnabled\":false}");
   sendDescription("/streaming/characteristics/orientationchanged", "{\"orientation\":0}");
}

int openXcloudChannels(void)
{
   greetingSent = 0;
   ready = 0;
   inputStarted = 0;
   lastControllerSentAt = 0;

   // button presses are sent out of order and never repeated: one that arrives late is already
   // wrong, so sending it again is worse than dropping it
   if (openSctpChannel(CHANNEL_CONTROL, "control", "controlV1", 1) != 0) return -1;
   if (openSctpChannel(CHANNEL_INPUT, "input", "1.0", 0) != 0) return -1;
   if (openSctpChannel(CHANNEL_MESSAGE, "message", "messageV1", 1) != 0) return -1;
   if (openSctpChannel(CHANNEL_CHAT, "chat", "chatV1", 1) != 0) return -1;

   logInfo(TAG "channels: asked for control, input, message and chat\n");
   return 0;
}

void sendXcloudControllerState(void)
{
   if (!ready || !isSctpChannelOpen(CHANNEL_INPUT)) return;

   // the loop runs as fast as packets arrive, which is far more often than a controller can
   // change. sending at that rate would be noise, so it goes at about the rate of the picture.
   uint64_t now = getTimeUs();
   if (now - lastControllerSentAt < CONTROLLER_INTERVAL_US) return;
   lastControllerSentAt = now;

   sendXcloudGamepad(CHANNEL_INPUT);
}

void pollXcloudChannels(void)
{
   // the machine wants to be told what kind of controller it is dealing with before any presses
   if (isSctpChannelOpen(CHANNEL_INPUT) && !inputStarted) {
      sendXcloudInputStart(CHANNEL_INPUT);
      inputStarted = 1;
   }

   if (greetingSent || !isSctpChannelOpen(CHANNEL_MESSAGE)) return;

   sendText(CHANNEL_MESSAGE, "{\"type\":\"Handshake\",\"version\":\"messageV1\","
                             "\"id\":\"be0bfc6d-1e83-4c8a-90ed-fa8601c5a179\",\"cv\":\"0\"}");
   greetingSent = 1;
   logInfo(TAG "channels: greeting sent\n");
}

void advanceXcloudChannels(const SctpMessage *message)
{
   if (message->channel != CHANNEL_MESSAGE || ready) return;

   char type[32];
   if (getJsonText((const char *)message->data, message->length, "type", type, sizeof type) != 0) return;
   if (strCmpICase(type, "HandshakeAck") != 0) return;

   // the machine has accepted us, so now it is told who is playing and what it should send
   char request[MESSAGE_MAX];
   snprintf(request, sizeof request, "{\"message\":\"authorizationRequest\",\"accessKey\":\"%s\"}", ACCESS_KEY);
   sendText(CHANNEL_CONTROL, request);
   sendText(CHANNEL_CONTROL, "{\"message\":\"gamepadChanged\",\"gamepadIndex\":0,\"wasAdded\":true}");

   sendDescription("/streaming/systemUi/configuration", "{\"version\":[0,2,0],\"systemUis\":[]}");
   sendClientDescription();

   // The stream is already running when we join it, so what arrives first only says what changed
   // since a picture we never saw. Asking for a complete one gives the decoder somewhere to start
   // from; without it the parts of the screen that move stay wrong.
   sendText(CHANNEL_CONTROL, "{\"message\":\"videoKeyframeRequested\",\"ifrRequested\":true}");

   ready = 1;
   logInfo(TAG "channels: the machine accepted us, asked it to start\n");
}

// the chosen size outlives the session, so it is kept beside the app's other settings rather than
// being asked for again every time
#define KEY_STREAM_HEIGHT "saved-xbox-size"
static const char *TALL_TOKEN = "1080p";

void loadXcloudStreamHeight(void)
{
   char text[SETTINGS_FILE_CAP];
   int length = readFile(CELL_STREAM_SETTINGS_PATH, text, sizeof text - 1);
   if (length <= 0) return;
   text[length] = 0;

   const char *value = findSettingValue(text, KEY_STREAM_HEIGHT);
   streamHeight = value && settingValueEquals(value, TALL_TOKEN) ? 1080 : 720;
}

int getXcloudStreamHeight(void) { return streamHeight; }

void setXcloudStreamHeight(int height)
{
   streamHeight = height;
   upsertSettingValue(CELL_STREAM_SETTINGS_PATH, KEY_STREAM_HEIGHT, height == 1080 ? TALL_TOKEN : "720p");
}
