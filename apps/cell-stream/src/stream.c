// stream - video path: PLAY request -> SINFO (stream parameters) -> fragmented H.264 access
// units over UDP -> reassemble -> cellVdec (via decode-h264) -> publish the newest picture for
// the render loop. render-on-arrival: no preroll, no clock, the draw thread always shows the
// latest decoded picture.
//
// two threads, deliberately: the receive thread must NEVER wait for the decoder. when both ran
// on one thread, a decode stall stopped us reading the socket, incoming packets overflowed and
// were lost, and every loss froze the picture until the next keyframe. the receive thread now
// only reassembles frames into a queue; the decode thread drains it.
//
// fragment packet layout (20-byte header, big-endian, must match StreamSender.cs):
//   [0]='V' [1]='F' [2..5]=frameId [6..7]=fragIndex [8..9]=fragCount [10]=flags(bit0 keyframe)
//   [11]=version [12..19]=encoder-exit time (server clock, microseconds)
// every fragment except a frame's last carries exactly FRAGMENT_PAYLOAD_BYTES of data.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "thread.h"
#include "dbg.h"
#include "gfx.h"
#include "audio.h"
#include "decode-h264.h"
#include "h264.h"
#include "net-common.h"
#include "stream.h"

#define BEACON_WAIT_MS         2000    // one look for a server before going round again
#define RECONNECT_DELAY_MS     500
#define SERVER_TIMEOUT_MS      2000    // once video has flowed, no video for this long = the server is gone
#define FIRST_FRAME_GRACE_MS   10000   // but the FIRST frame gets much longer: the server's encoder cold-starts
                                       // in ~1-2s, and giving up early made us drop it and reconnect, which
                                       // restarted the cold-start and never let the first frame land
#define BUFFER_RELEASE_WAIT_MS 2000    // how long a new session waits for the draw thread to free the old one's pictures
#define CLOCK_RESYNC_INTERVAL_MS 30000 // the two clocks drift apart; re-measure the offset this often
#define CLOCK_PROBE_TIMEOUT_MS 500
#define PLAY_TRIES             5
#define TIME_SYNC_SAMPLES      10
#define FRAGMENT_HEADER_BYTES  20
#define FRAGMENT_PAYLOAD_BYTES 1300
#define FRAME_MAX_BYTES        (1024 * 1024)   // a keyframe at a high bitrate is far bigger than a normal frame; generous headroom
#define FRAGMENT_MAX_COUNT     (FRAME_MAX_BYTES / FRAGMENT_PAYLOAD_BYTES + 1)
#define FEED_TIME_RING         32
#define DECODE_BUSY_TRIES      200   // 1ms apart; give up feeding an AU after this
#define YUV_BUFFER_COUNT       5     // enough that a write target is never published, previously published, or recently drawn
#define AU_SLOT_COUNT          6     // queue between the receive and decode threads (also the decoder's no-touch window)

// section: state shared with the UI thread

static StreamStats stats;
static sys_lwmutex_t statsLock;
static volatile int streamRunning;   // the connect loop is alive
static volatile int streamLive;      // ... and a server is actually sending us video
static volatile int stopRequested;    // the app is exiting: end the connect loop for good
static volatile int sessionAbort;     // THIS session has failed: drop it and reconnect
static volatile int buffersInUse;     // pictures allocated; only the draw thread may free them

// latest decoded picture. the RSX reads a drawn buffer asynchronously (up to ~2 flips later),
// so the decoder must never write into the published, previously published, or two most
// recently DRAWN buffers - if it decodes several frames between two draws, published moves on
// while the RSX is still scanning an older buffer (reusing it showed as screen tearing).
static void *yuvBuffers[YUV_BUFFER_COUNT];
static int publishedIndex = -1, previousPublishedIndex = -1;
static int drawnIndex = -1, previousDrawnIndex = -1;
static int publishedWidth, publishedHeight;
static uint64_t bufferCaptureUs[YUV_BUFFER_COUNT], bufferDecodedUs[YUV_BUFFER_COUNT];   // per buffer, for present/total timings
static sys_lwmutex_t frameLock;

// one-frame buffer mode: instead of showing the newest picture the instant it decodes, present on the
// display's steady refresh one frame behind, keeping a single decoded frame in reserve. a frame that
// decodes a little late is then already the reserve when its refresh comes, so the cadence stays even -
// it rides out the common small hitches for the price of ~one frame of delay. bigger stalls (a frame
// more than a whole refresh late) still show as a single repeat. publishSeq counts decoded frames;
// presentSeq is the one currently shown; we only keep the newest two buffers, so present tracks within one.
static int bufferOneFrame;
static uint64_t publishSeq, presentSeq;

// section: decode bookkeeping (stream thread only)

static H264Decoder *decoder;
static int fedCount, decodedCount;

// server clock offset: serverUs ~= localUs + clockOffsetUs, so a frame's encoder-exit stamp
// converts to our clock as (captureUs - clockOffsetUs). measured once before streaming.
static int64_t clockOffsetUs;

// per-frame timings, ring-buffered by feed order so a picture coming out can be matched
// back to when its frame was captured, arrived and was fed
typedef struct {
   uint64_t captureUs;    // encoder exit (server clock, converted to ours)
   uint64_t completeUs;
   uint64_t fedUs;
} FrameTiming;
static FrameTiming feedTiming[FEED_TIME_RING];

// per-second stat window
static uint64_t windowStartUs, windowBytes;
static uint64_t windowNetworkUs, windowDecodeUs;
static int windowDecodedFrames;   // all decoded frames (the fps count)
static int windowTimedFrames;     // ... of those, the ones whose server timestamps made sense
static int clockOutOfSync;        // the server's clock disagreed with ours: latency figures are not to be trusted
static int clockOutOfSyncWarned;  // ... and it has been logged once for this episode

// after a session that never received video, the reconnect loop goes quiet: an idle server answering the
// handshake every few seconds would otherwise fill the log with the same connect lines forever
static int logSessionDetail = 1;
static uint64_t windowPresentUs, windowTotalUs;   // written by the draw thread under frameLock
static int windowPresentedFrames;

static uint64_t windowFlipWaitUs;   // time spent waiting for the display (see noteStreamFlipWait)
static int windowFlipWaits;


// section: the queue between the receive thread and the decode thread
//
// the receive thread reassembles into the slot at auWriteSlot and publishes it by advancing
// auWriteSlot; the decode thread consumes from auReadSlot. a fed slot must also stay untouched
// until the decoder has consumed it, which the queue depth naturally guarantees.

typedef struct {
   int bytes;
   int keyframe;
   uint64_t captureUs, completeUs;
} AuSlot;

static uint8_t auBuffers[AU_SLOT_COUNT][FRAME_MAX_BYTES];
static AuSlot auSlots[AU_SLOT_COUNT];
static volatile int auWriteSlot, auReadSlot;
static sys_lwmutex_t queueLock;

static int getQueuedAuCount(void)
{
   lock(&queueLock);
   int count = (auWriteSlot - auReadSlot + AU_SLOT_COUNT) % AU_SLOT_COUNT;
   unlock(&queueLock);
   return count;
}

// frame reassembly (receive thread only); assembles straight into the slot being written
static long assemblyFrameId = -1;
static int assemblyFragCount, assemblyFragsReceived, assemblyLastFragBytes;
static uint8_t assemblyFragSeen[FRAGMENT_MAX_COUNT];

// the message the UI shows for the current state ("waiting for server ...", "connecting ...", an error)
static void setStreamStatusLocked(StreamState state, const char *message)
{
   stats.state = state;
   strncpy(stats.error, message, sizeof stats.error - 1);
   stats.error[sizeof stats.error - 1] = 0;
}

static void setStreamStatus(StreamState state, const char *message)
{
   lock(&statsLock);
   setStreamStatusLocked(state, message);
   unlock(&statsLock);
}

static void setStreamError(const char *message)
{
   setStreamStatus(STREAM_STATE_ERROR, message);
   logError("[cst] stream: %s\n", message);
}

// section: decoded-frame publishing

static int getNextWriteIndex(void)
{
   lock(&frameLock);
   int i;
   for (i = 0; i < YUV_BUFFER_COUNT; i++) {
      if (i == publishedIndex || i == previousPublishedIndex || i == drawnIndex || i == previousDrawnIndex) continue;
      break;
   }
   unlock(&frameLock);
   return i < YUV_BUFFER_COUNT ? i : 0;   // 5 buffers vs at most 4 exclusions: always found
}

static void drainDecodedFrames(void)
{
   if (!decoder) return;   // the decoder only exists once the first keyframe has described the stream
   for (;;) {
      int writeIndex = getNextWriteIndex();
      int width, height;
      uint64_t pts;
      int rc = getFrameH264(decoder, yuvBuffers[writeIndex], &width, &height, &pts);
      if (rc != 1) break;

      // pictures come out in feed order, so this frame's timings are the decodedCount'th we fed
      uint64_t now = getTimeUs();
      const FrameTiming *timing = &feedTiming[decodedCount % FEED_TIME_RING];
      uint64_t decodeUs = now - timing->fedUs;
      windowDecodeUs += decodeUs;

      // a frame cannot arrive before it was captured: if it claims to, our clock and the server's have come
      // apart (a restarted server used to do this) and the timings are meaningless, so leave them out of the
      // averages rather than reporting a negative latency
      if (timing->completeUs >= timing->captureUs) {
         windowNetworkUs += timing->completeUs - timing->captureUs;
         windowTimedFrames++;
      } else {
         clockOutOfSync = 1;
      }
      windowDecodedFrames++;
      decodedCount++;

      lock(&frameLock);
      previousPublishedIndex = publishedIndex;
      publishedIndex = writeIndex;
      publishedWidth = width;
      publishedHeight = height;
      bufferCaptureUs[writeIndex] = timing->captureUs;
      bufferDecodedUs[writeIndex] = now;
      publishSeq++;
      unlock(&frameLock);
   }
}

static volatile int waitingForKeyframe = 1;   // set on start, and after any loss unless the stream self-heals

// set from SINFO. an intra-refresh stream redraws a strip of the picture every frame, so a loss's damage is
// swept away within one sweep: decode straight through it. a keyframe stream cannot repair itself, so
// decoding on after a loss paints corruption that lingers and spreads - hold the last good picture instead.
static int streamSelfHeals;

// brings up the decoder from the stream's own SPS, on the first keyframe. the size cellVdec is given
// must be the CODED size - never the display size, and never a size someone else claimed: too small
// and it silently decodes nothing (black), too large and it locks the console. 0 on success.
static int openDecoderForStream(const uint8_t *keyframe, int bytes)
{
   H264StreamInfo info;
   if (readH264StreamInfo(keyframe, bytes, &info) != 0) { setStreamError("no SPS in the keyframe"); return -1; }
   logInfo("[cst] stream codes %dx%d, level %d, %d ref frames\n", info.codedWidth, info.codedHeight,
           info.level, info.maxRefFrames);

   size_t yuvBytes = ((size_t)info.codedWidth * info.codedHeight * 3 / 2 + 127) & ~(size_t)127;
   int i;
   for (i = 0; i < YUV_BUFFER_COUNT; i++) {
      if (yuvBuffers[i]) { setStreamError("previous buffers not yet released"); return -1; }
      yuvBuffers[i] = allocGfxVideoBuffer(yuvBytes);
      if (!yuvBuffers[i]) {
         // free the ones we just got before bailing - they were never published, so the RSX never saw
         // them. leaving them behind would leak forever (buffersInUse is still 0, so releaseStreamBuffers
         // skips them) and wedge every future session on the "previous buffers not yet released" guard.
         setStreamError("video buffer alloc failed");
         while (i-- > 0) { freeGfxVideoBuffer(yuvBuffers[i]); yuvBuffers[i] = NULL; }
         return -1;
      }
   }
   buffersInUse = 1;

   decoder = createH264Decoder(info.codedWidth, info.codedHeight, info.level, info.maxRefFrames);
   if (!decoder) { setStreamError("decoder create failed"); return -1; }

   lock(&statsLock);
   stats.width = info.codedWidth;
   stats.height = info.codedHeight;
   unlock(&statsLock);
   return 0;
}

// decode thread: feeds one queued access unit into the decoder. blocking here is safe -
// the receive thread keeps draining the socket regardless.
static void feedAu(const AuSlot *slot, const uint8_t *data)
{
   // the first feed must be a keyframe regardless: it carries the stream's SPS/PPS setup data, and the
   // decoder is built from it. after that, see streamSelfHeals.
   if (waitingForKeyframe) {
      if (!slot->keyframe) return;
      waitingForKeyframe = 0;
   }

   // the very first keyframe describes the stream: build the decoder to match it
   if (!decoder && openDecoderForStream(data, slot->bytes) != 0) { sessionAbort = 1; return; }

   int tries;
   for (tries = 0; tries < DECODE_BUSY_TRIES; tries++) {
      int rc = decodeAuH264(decoder, data, slot->bytes, (uint64_t)fedCount);
      if (rc == 0) {
         FrameTiming *timing = &feedTiming[fedCount % FEED_TIME_RING];
         timing->captureUs = slot->captureUs;
         timing->completeUs = slot->completeUs;
         timing->fedUs = getTimeUs();
         fedCount++;
         return;
      }
      if (rc < 0) { setStreamError("decode error"); sessionAbort = 1; return; }   // let the session tear down and reconnect
      drainDecodedFrames();   // decoder full: make room, then retry the same AU
      sleepMs(1);
   }
   logWarn("[cst] stream: decoder busy, dropped a frame\n");
}

// section: fragment reassembly

static void handleFragment(const uint8_t *packet, int packetBytes)
{
   if (packetBytes <= FRAGMENT_HEADER_BYTES) return;
   long frameId = ((long)packet[2] << 24) | ((long)packet[3] << 16) | ((long)packet[4] << 8) | packet[5];
   int fragIndex = (packet[6] << 8) | packet[7];
   int fragCount = (packet[8] << 8) | packet[9];
   int keyframe = packet[10] & 1;
   int payloadBytes = packetBytes - FRAGMENT_HEADER_BYTES;

   uint64_t captureUs = 0;
   int i;
   for (i = 0; i < 8; i++) captureUs = (captureUs << 8) | packet[12 + i];

   if (fragCount <= 0 || fragCount > FRAGMENT_MAX_COUNT || fragIndex >= fragCount) return;
   if ((long)(fragCount - 1) * FRAGMENT_PAYLOAD_BYTES + payloadBytes > FRAME_MAX_BYTES) return;

   AuSlot *slot = &auSlots[auWriteSlot];

   // a newer frame started before this one completed: its data is now missing from the stream, so drop the
   // incomplete frame - and, unless the stream repairs itself, stop decoding until the next keyframe
   if (frameId != assemblyFrameId) {
      if (assemblyFrameId >= 0 && assemblyFragsReceived > 0) {
         if (!streamSelfHeals) waitingForKeyframe = 1;
         lock(&statsLock);
         stats.framesIncomplete++;
         unlock(&statsLock);
      }
      assemblyFrameId = frameId;
      assemblyFragCount = fragCount;
      assemblyFragsReceived = 0;
      assemblyLastFragBytes = -1;
      slot->keyframe = keyframe;
      slot->captureUs = captureUs - clockOffsetUs;   // server clock -> ours
      memset(assemblyFragSeen, 0, fragCount);
   }
   if (assemblyFragSeen[fragIndex]) return;
   assemblyFragSeen[fragIndex] = 1;
   assemblyFragsReceived++;
   memcpy(auBuffers[auWriteSlot] + (long)fragIndex * FRAGMENT_PAYLOAD_BYTES, packet + FRAGMENT_HEADER_BYTES, payloadBytes);
   if (fragIndex == fragCount - 1) assemblyLastFragBytes = payloadBytes;
   windowBytes += packetBytes;

   if (assemblyFragsReceived != assemblyFragCount || assemblyLastFragBytes < 0) return;

   // frame complete: hand the slot to the decode thread
   slot->bytes = (assemblyFragCount - 1) * FRAGMENT_PAYLOAD_BYTES + assemblyLastFragBytes;
   slot->completeUs = getTimeUs();
   assemblyFrameId = -1;

   lock(&queueLock);
   int nextSlot = (auWriteSlot + 1) % AU_SLOT_COUNT;
   if (nextSlot == auReadSlot) {
      // decoder is a whole queue behind - drop this frame rather than stall the receive thread
      unlock(&queueLock);
      if (!streamSelfHeals) waitingForKeyframe = 1;
      lock(&statsLock);
      stats.framesDroppedBehind++;
      unlock(&statsLock);
      return;
   }
   auWriteSlot = nextSlot;
   unlock(&queueLock);
}

// section: audio
//
// the server sends the PC's speaker output as small uncompressed packets (5ms each, 16-bit stereo);
// we convert them to the mixer's float format and push them into its PCM feed. no decoder, so the
// only audio delay is the mixer's own backlog, which we deliberately keep small (AUDIO_PRIME_MS).
//
// audio packet layout (16-byte header, big-endian, must match AudioStreamer.cs):
//   [0]='A' [1]='F' [2..5]=packetId [6..7]=frameCount [8..15]=capture time (server clock)

#define AUDIO_HEADER_BYTES  16
#define AUDIO_PRIME_MS      60    // backlog the mixer builds before playing: rides out network jitter, and costs that much delay
#define AUDIO_DRIFT_MS      25    // how far the backlog may wander from that before we correct it
#define AUDIO_MAX_FRAMES    512   // one packet is 5ms (240 frames at 48kHz); this is generous headroom

static int audioFeedOpen, audioRate;
static uint64_t audioPushedFrames;   // vs getAudioPcmFeedPlayedFrames(), this gives the live backlog

static int getAudioBacklogMs(void)
{
   return (int)((audioPushedFrames - getAudioPcmFeedPlayedFrames()) * 1000 / audioRate);
}

static void openAudioFeed(const char *info)
{
   if (audioFeedOpen) return;
   long rate = parseNumberAfter(info, "AINFO ");
   if (rate <= 0) return;
   if (openAudioPcmFeed((int)rate, (int)(rate * AUDIO_PRIME_MS / 1000)) != 0) {
      logWarn("[cst] audio: feed open failed, playing silent\n");
      return;
   }
   audioRate = (int)rate;
   audioFeedOpen = 1;
   if (logSessionDetail) logInfo("[cst] audio: %ldHz stereo, %dms buffer\n", rate, AUDIO_PRIME_MS);
}

static void handleAudioPacket(const uint8_t *packet, int packetBytes)
{
   if (!audioFeedOpen || packetBytes <= AUDIO_HEADER_BYTES) return;
   int frames = (packet[6] << 8) | packet[7];   // packet[2..5] is the packet id, which nothing needs now
   if (frames <= 0 || frames > AUDIO_MAX_FRAMES || packetBytes < AUDIO_HEADER_BYTES + frames * 4) return;

   float stereo[AUDIO_MAX_FRAMES * 2];
   const uint8_t *samples = packet + AUDIO_HEADER_BYTES;
   int i;
   for (i = 0; i < frames * 2; i++) {
      int16_t value = (int16_t)((samples[i * 2] << 8) | samples[i * 2 + 1]);
      stereo[i] = value / 32768.0f;
   }
   // the two machines' clocks tick at slightly different speeds, so the PC sends a little faster (or
   // slower) than the PS3 plays and the backlog - which IS the audio delay - creeps in one direction
   // for as long as the stream runs. hold it near AUDIO_PRIME_MS by dropping this 5ms chunk when the
   // backlog has run long, or playing it twice when it has run short. measured drift is ~1ms per 5s,
   // so this fires roughly once every 25 seconds: far too little to hear.
   int repeats = 1;
   if (getAudioPcmFeedPlayedFrames() > 0) {   // still filling the initial backlog: nothing to correct yet
      int backlogMs = getAudioBacklogMs();
      if (backlogMs > AUDIO_PRIME_MS + AUDIO_DRIFT_MS) repeats = 0;
      else if (backlogMs < AUDIO_PRIME_MS - AUDIO_DRIFT_MS) repeats = 2;
   }
   for (i = 0; i < repeats; i++) audioPushedFrames += pushAudioPcm(stereo, frames);

   windowBytes += packetBytes;
}

static void closeAudioFeed(void)
{
   if (!audioFeedOpen) return;
   closeAudioPcmFeed();
   audioFeedOpen = 0;
}

static void updateWindowStats(void)
{
   uint64_t now = getTimeUs();
   if (now - windowStartUs < 1000000ull) return;
   uint64_t elapsedUs = now - windowStartUs;

   lock(&frameLock);
   uint64_t presentUs = windowPresentUs, totalUs = windowTotalUs, flipWaitUs = windowFlipWaitUs;
   int presentedFrames = windowPresentedFrames, flipWaits = windowFlipWaits;
   windowPresentUs = windowTotalUs = windowFlipWaitUs = 0;
   windowPresentedFrames = windowFlipWaits = 0;
   unlock(&frameLock);

   lock(&statsLock);
   stats.bitrateKbps = (int)(windowBytes * 8000 / elapsedUs);
   stats.receivedFps = (int)(windowDecodedFrames * 1000000ull / elapsedUs);
   stats.networkMsTenths = windowTimedFrames > 0 ? (int)(windowNetworkUs / windowTimedFrames / 100) : 0;
   stats.decodeMsTenths = windowDecodedFrames > 0 ? (int)(windowDecodeUs / windowDecodedFrames / 100) : 0;
   stats.presentMsTenths = presentedFrames > 0 ? (int)(presentUs / presentedFrames / 100) : 0;
   stats.displayWaitMsTenths = flipWaits > 0 ? (int)(flipWaitUs / flipWaits / 100) : 0;
   stats.totalMsTenths = presentedFrames > 0 ? (int)(totalUs / presentedFrames / 100) : 0;
   stats.totalMsTenths += stats.displayWaitMsTenths;   // the frame is not really seen until the display takes it
   stats.pipelineDepth = fedCount - decodedCount;
   unlock(&statsLock);

   windowStartUs = now;
   windowBytes = 0;
   windowNetworkUs = windowDecodeUs = 0;
   windowDecodedFrames = windowTimedFrames = 0;

   // the figures live on the stats panel, not in the log - a line every few seconds would fill dbg.txt
   // over a long session. the one thing still worth saying is that they cannot be trusted.
   if (clockOutOfSync && !clockOutOfSyncWarned) {
      clockOutOfSyncWarned = 1;   // once per episode; the re-sync line marks its end
      logWarn("[cst] the server's clock disagrees with ours - was it restarted mid-stream? "
              "latency figures cover only the frames that still made sense\n");
   }
}

// section: clock sync

// pairs the server's clock with ours so per-frame stamps become real latencies. takes the
// sample with the lowest round trip (that one has the least one-way skew). 1 on success.
static int syncServerClock(int socketValue, struct sockaddr_in *serverAddress)
{
   uint64_t bestRttUs = ~0ull;
   int synced = 0;

   drainSocket(socketValue);
   setReceiveTimeout(socketValue, 500);
   int sample;
   for (sample = 0; sample < TIME_SYNC_SAMPLES && !stopRequested; sample++) {
      uint64_t sentUs = getTimeUs();
      if (sendto(socketValue, "TIME", 4, 0, (struct sockaddr *)serverAddress, sizeof *serverAddress) < 0) continue;

      char reply[PACKET_MAX];
      int length = recv(socketValue, reply, sizeof reply - 1, 0);
      if (length <= 0) continue;
      reply[length] = 0;
      long long serverUs = parseBigNumberAfter(reply, "TIME ");   // microseconds since 2020: far too big for a long
      if (serverUs < 0) continue;

      uint64_t nowUs = getTimeUs();
      uint64_t rttUs = nowUs - sentUs;
      if (rttUs < bestRttUs) {
         bestRttUs = rttUs;
         // the reply was stamped ~halfway through the round trip
         clockOffsetUs = (int64_t)serverUs - (int64_t)(sentUs + rttUs / 2);
         synced = 1;
      }
      sleepMs(20);
   }
   if (synced && logSessionDetail) logInfo("[cst] clock synced, best round trip %ums\n", (unsigned)(bestRttUs / 1000));
   return synced;
}

// the two machines' clocks tick at slightly different speeds, so an offset measured once at startup
// drifts out over a few minutes and the latency figures quietly start to lie (the PS3 used to log "the
// server's clock disagrees with ours" and drop those frames from its averages). so re-measure it now
// and then - without ever blocking the receive loop: a probe goes out between packets, and its reply is
// picked up as it arrives, exactly like any other packet.

static uint64_t resyncRoundStartUs;
static uint64_t resyncProbeSentUs;   // 0 = no probe outstanding
static uint64_t resyncBestRttUs;
static int64_t resyncBestOffsetUs;
static int resyncProbesLeft;

static void updateClockSync(int socketValue, struct sockaddr_in *serverAddress)
{
   uint64_t now = getTimeUs();

   if (resyncProbesLeft == 0 && now - resyncRoundStartUs >= (uint64_t)CLOCK_RESYNC_INTERVAL_MS * 1000) {
      resyncRoundStartUs = now;
      resyncProbesLeft = TIME_SYNC_SAMPLES;
      resyncBestRttUs = ~0ull;
   }

   // a probe that never came back: forget it, so the next one can go out
   if (resyncProbeSentUs && now - resyncProbeSentUs > (uint64_t)CLOCK_PROBE_TIMEOUT_MS * 1000) {
      resyncProbeSentUs = 0;
      resyncProbesLeft--;
   }

   if (resyncProbesLeft > 0 && resyncProbeSentUs == 0) {
      resyncProbeSentUs = now;
      sendto(socketValue, "TIME", 4, 0, (struct sockaddr *)serverAddress, sizeof *serverAddress);
   }
}

// keeps the sample with the shortest round trip: that one has the least one-way skew in it
static void handleTimeReply(const char *reply)
{
   if (!resyncProbeSentUs) return;

   long long serverUs = parseBigNumberAfter(reply, "TIME ");
   if (serverUs < 0) return;

   uint64_t rttUs = getTimeUs() - resyncProbeSentUs;
   if (rttUs < resyncBestRttUs) {
      resyncBestRttUs = rttUs;
      resyncBestOffsetUs = (int64_t)serverUs - (int64_t)(resyncProbeSentUs + rttUs / 2);
   }
   resyncProbeSentUs = 0;

   if (--resyncProbesLeft <= 0 && resyncBestRttUs != ~0ull) {
      resyncProbesLeft = 0;
      clockOffsetUs = resyncBestOffsetUs;
      // only worth a line when it ends a distrusted-figures episode; the routine 30s re-sync is silent
      if (clockOutOfSync) logInfo("[cst] clock re-synced, best round trip %ums - latency figures trustworthy again\n",
                                  (unsigned)(resyncBestRttUs / 1000));
      clockOutOfSync = 0;
      clockOutOfSyncWarned = 0;
   }
}

// section: session setup

// SINFO <width> <height> <level> <refs> <fps> [intraRefresh]. the trailing flag is optional: a server that
// doesn't send it means keyframe recovery, which is the safe default.
static int parseSinfo(const char *text, long *width, long *height, long *level, long *refs, long *fps, long *intraRefresh)
{
   if (strncmp(text, "SINFO ", 6) != 0) return 0;
   char *cursor = (char *)text + 6;
   long *values[5] = { width, height, level, refs, fps };
   int i;
   for (i = 0; i < 5; i++) {
      *values[i] = strtol(cursor, &cursor, 10);
      if (*values[i] <= 0) return 0;
   }
   *intraRefresh = strtol(cursor, &cursor, 10);
   return 1;
}

// sends PLAY (with retries) and waits for the SINFO reply describing the stream. 1 on success.
static int requestPlay(int socketValue, struct sockaddr_in *serverAddress, long *width, long *height, long *level, long *refs,
                       long *fps, long *intraRefresh)
{
   setReceiveTimeout(socketValue, 1000);
   int attempt;
   for (attempt = 0; attempt < PLAY_TRIES && !stopRequested; attempt++) {
      sendto(socketValue, "PLAY", 4, 0, (struct sockaddr *)serverAddress, sizeof *serverAddress);
      char packet[PACKET_MAX];
      int length = recv(socketValue, packet, sizeof packet - 1, 0);
      if (length <= 0) continue;
      packet[length] = 0;
      if (parseSinfo(packet, width, height, level, refs, fps, intraRefresh)) return 1;
   }
   return 0;
}

// section: the controller up-channel
//
// the render loop hands us the pad once per frame and we send it to the PC, which replays it on a
// virtual gamepad. sent every frame rather than only on change: a dropped UDP packet would otherwise
// leave a button stuck down on the PC until it next moved, and at 20 bytes a frame the traffic is
// nothing next to the video.
//
// pad packet layout (20-byte header, big-endian, must match PadReceiver.cs):
//   [0]='C' [1]='P' [2..5]=packetId [6..7]=buttons [8]=leftX [9]=leftY [10]=rightX [11]=rightY
//   [12..19]=send time (in the SERVER's clock, so it can measure the trip without a clock of ours)

#define PAD_PACKET_BYTES 20

static int padSocket = -1;                  // the live session's socket, or -1 between sessions
static struct sockaddr_in padServerAddress;
static uint32_t padPacketId;

void sendPadState(unsigned buttons, int leftX, int leftY, int rightX, int rightY)
{
   if (padSocket < 0) return;

   uint8_t packet[PAD_PACKET_BYTES];
   packet[0] = 'C';
   packet[1] = 'P';
   packet[2] = (uint8_t)(padPacketId >> 24);
   packet[3] = (uint8_t)(padPacketId >> 16);
   packet[4] = (uint8_t)(padPacketId >> 8);
   packet[5] = (uint8_t)padPacketId;
   packet[6] = (uint8_t)(buttons >> 8);
   packet[7] = (uint8_t)buttons;
   packet[8] = (uint8_t)(int8_t)leftX;
   packet[9] = (uint8_t)(int8_t)leftY;
   packet[10] = (uint8_t)(int8_t)rightX;
   packet[11] = (uint8_t)(int8_t)rightY;

   uint64_t sentUs = getTimeUs() + clockOffsetUs;   // our clock -> the server's
   int i;
   for (i = 0; i < 8; i++) packet[12 + i] = (uint8_t)(sentUs >> (56 - i * 8));

   sendto(padSocket, (const char *)packet, sizeof packet, 0, (struct sockaddr *)&padServerAddress, sizeof padServerAddress);
   padPacketId++;
}

// tells the PC which of its two input devices the pad should drive
void sendPadMode(int useGamepad)
{
   if (padSocket < 0) return;

   const char *message = useGamepad ? "PADMODE gamepad" : "PADMODE mouse";
   sendto(padSocket, message, strlen(message), 0, (struct sockaddr *)&padServerAddress, sizeof padServerAddress);
}

// asks the PC to run one of its four Custom Commands - the PS3 only names the slot
void sendCustomCommand(int slot)
{
   if (padSocket < 0) return;

   char message[16];
   snprintf(message, sizeof message, "CUSTOM %d", slot);
   sendto(padSocket, message, strlen(message), 0, (struct sockaddr *)&padServerAddress, sizeof padServerAddress);
}

// one key from the on-screen keyboard. control keys arrive as their ASCII bytes
// ('\b' backspace, '\t' tab, '\n' return, ' ' space); the PC maps them to key presses.
void sendKeystroke(char key)
{
   if (padSocket < 0) return;

   char message[5] = { 'K', 'E', 'Y', ' ', key };
   sendto(padSocket, message, sizeof message, 0, (struct sockaddr *)&padServerAddress, sizeof padServerAddress);
}

// section: the decode thread - drains the queue, feeds the decoder, publishes pictures

static volatile int decodeThreadRunning;

static void runDecodeThread(uint64_t argument)
{
   (void)argument;
   while (decodeThreadRunning && !stopRequested && !sessionAbort) {
      if (getQueuedAuCount() == 0) {
         drainDecodedFrames();   // nothing new to feed; collect anything the decoder still owes us
         sleepMs(1);
         continue;
      }
      feedAu(&auSlots[auReadSlot], auBuffers[auReadSlot]);
      drainDecodedFrames();

      lock(&queueLock);
      auReadSlot = (auReadSlot + 1) % AU_SLOT_COUNT;
      unlock(&queueLock);
   }
   exitThread();
}

// section: the stream session
//
// one connect-and-stream attempt. returns when the server goes away (or was never there), leaving us
// ready to try again - the retry loop is runStreamThread below.

static void runStreamSession(void)
{
   int socketValue = -1;
   int i;

   // reset session state
   sessionAbort = 0;
   decoder = NULL;
   fedCount = decodedCount = 0;
   waitingForKeyframe = 1;
   assemblyFrameId = -1;
   auWriteSlot = auReadSlot = 0;
   decodeThreadRunning = 0;
   windowBytes = windowNetworkUs = windowDecodeUs = 0;
   windowDecodedFrames = 0;
   windowPresentUs = windowTotalUs = 0;
   windowPresentedFrames = 0;
   clockOffsetUs = 0;
   clockOutOfSync = 0;
   clockOutOfSyncWarned = 0;
   resyncProbesLeft = 0;
   resyncProbeSentUs = 0;
   resyncRoundStartUs = getTimeUs();
   windowTimedFrames = 0;
   streamSelfHeals = 0;
   padPacketId = 0;
   audioPushedFrames = 0;
   lock(&statsLock);
   memset(&stats, 0, sizeof stats);
   setStreamStatusLocked(STREAM_STATE_WAITING, "waiting for server ...");
   unlock(&statsLock);

   // the previous session's pictures are freed by the DRAW thread a few frames after it went away.
   // starting a new one before that would allocate on top of buffers the RSX may still be scanning.
   for (i = 0; i < BUFFER_RELEASE_WAIT_MS && buffersInUse && !stopRequested; i++) sleepMs(1);

   // connect: socket, beacon discovery, PLAY/SINFO handshake
   socketValue = openClientSocket();
   if (socketValue < 0) { setStreamError("socket/bind failed"); goto cleanup; }

   // wait for a beacon in 1s slices, so a stop request stays responsive. not finding one is normal -
   // the server simply is not up yet - so we just go round again rather than calling it an error.
   struct sockaddr_in serverAddress;
   if (!discoverServer(socketValue, &serverAddress, BEACON_WAIT_MS)) goto cleanup;

   setStreamStatus(STREAM_STATE_CONNECTING, "connecting ...");
   if (!syncServerClock(socketValue, &serverAddress)) {
      if (!stopRequested && logSessionDetail) logWarn("[cst] stream: clock sync failed, retrying\n");
      goto cleanup;
   }
   drainSocket(socketValue);

   // SINFO announces the source's frame rate and how the stream recovers from a loss; everything the
   // decoder needs (coded size, level, ref frames) is read from the stream's own SPS when its first
   // keyframe arrives, because only the stream itself knows its true coded size (see openDecoderForStream)
   long width, height, level, refs, fps, intraRefresh = 0;
   if (!requestPlay(socketValue, &serverAddress, &width, &height, &level, &refs, &fps, &intraRefresh)) {
      if (!stopRequested && logSessionDetail) logWarn("[cst] stream: no SINFO reply, retrying\n");
      goto cleanup;
   }
   streamSelfHeals = (intraRefresh != 0);
   if (logSessionDetail) logInfo("[cst] stream: server offers %ldx%ld at %ldfps, loss recovery = %s\n", width, height,
                                 fps, streamSelfHeals ? "intra refresh" : "keyframes");

   lock(&statsLock);
   stats.state = STREAM_STATE_STREAMING;
   stats.error[0] = 0;
   stats.sourceFps = (int)fps;
   unlock(&statsLock);
   streamLive = 1;

   // the render loop may now send the pad on this socket (see sendPadState)
   padServerAddress = serverAddress;
   padSocket = socketValue;

   // start the decode thread, then run the receive loop here. the receive loop must never
   // block on decoding, or the socket backs up and packets are lost (each loss then freezes
   // the picture until the next keyframe - this was the stutter).
   //
   // both the receive loop (cst-stream) and this decode thread run at HIGH: on the ~2-way PPU the video
   // path must not be preempted by the render/UI thread (which sits just below), or a decoded frame lands
   // late and hitches. verified smoother on hardware; keep them equal so neither starves the other.
   sys_ppu_thread_t decodeThreadId;
   decodeThreadRunning = 1;   // must be set BEFORE the thread starts, or it can see 0 and exit at once
   int decodeRc = spawnJoinableThread(&decodeThreadId, runDecodeThread, 0, THREAD_PRIORITY_HIGH, THREAD_STACK_SIZE_64KB, "cst-decode");
   if (decodeRc != 0) { decodeThreadRunning = 0; setStreamError("decode thread spawn failed"); goto cleanup; }

   setReceiveTimeout(socketValue, 500);
   windowStartUs = getTimeUs();
   uint64_t lastVideoUs = getTimeUs();
   while (!stopRequested && !sessionAbort) {
      uint8_t packet[PACKET_MAX];
      int length = recv(socketValue, packet, sizeof packet, 0);
      if (length > FRAGMENT_HEADER_BYTES && packet[0] == 'V' && packet[1] == 'F') {
         handleFragment(packet, length);
         lastVideoUs = getTimeUs();
      }
      else if (length > AUDIO_HEADER_BYTES && packet[0] == 'A' && packet[1] == 'F') handleAudioPacket(packet, length);
      else if (length > 6 && length < (int)sizeof packet && packet[0] == 'A' && packet[1] == 'I') {
         packet[length] = 0;   // AINFO is text; terminate it before parsing
         openAudioFeed((const char *)packet);
      }
      else if (length > 5 && length < (int)sizeof packet && packet[0] == 'T' && packet[1] == 'I') {
         packet[length] = 0;
         handleTimeReply((const char *)packet);   // a clock re-sync probe coming back
      }

      // the server went away (closed, crashed, or off the network): drop the session and go back to
      // looking for one. video is the signal - it flows continuously while a server is alive. before the
      // first frame the encoder is still cold-starting, so wait much longer; once video has flowed, a
      // short gap means it really is gone. only worth a line when video actually flowed: a server that
      // answers but never sends (PC idle) cycles here and would otherwise fill the log.
      int idleLimitMs = fedCount > 0 ? SERVER_TIMEOUT_MS : FIRST_FRAME_GRACE_MS;
      if (getTimeUs() - lastVideoUs > (uint64_t)idleLimitMs * 1000) {
         if (fedCount > 0) logWarn("[cst] stream: no video for %dms - server gone, waiting for it to come back\n", SERVER_TIMEOUT_MS);
         else logWarn("[cst] stream: no first frame in %dms - giving up on this server, retrying\n", FIRST_FRAME_GRACE_MS);
         break;
      }
      updateClockSync(socketValue, &serverAddress);
      updateWindowStats();
   }

   for (i = 0; i < 3; i++) sendto(socketValue, "STOP", 4, 0, (struct sockaddr *)&serverAddress, sizeof serverAddress);

cleanup:
   // stop the render loop sending the pad, and let any send already under way finish, before the
   // socket it is using goes away underneath it
   padSocket = -1;
   sleepMs(2);

   if (decodeThreadRunning) {
      decodeThreadRunning = 0;
      joinThread(decodeThreadId);   // the decoder must be idle before we destroy it
   }
   // unpublish so the draw thread stops using the buffers. do NOT free them here: the RSX may
   // still be scanning the last-drawn frame - the draw thread frees them via releaseStreamBuffers
   // once it has rendered a few frames without video (freeing early hard-froze the app).
   if (fedCount > 0) logInfo("[cst] stream: stopping (fed %d decoded %d)\n", fedCount, decodedCount);
   logSessionDetail = fedCount > 0;   // a session that streamed earns the next connect its log lines back
   closeAudioFeed();
   lock(&frameLock);
   publishedIndex = previousPublishedIndex = -1;
   drawnIndex = previousDrawnIndex = -1;
   publishSeq = presentSeq = 0;
   unlock(&frameLock);
   if (decoder) {
      logInfo("[cst] stream: destroying decoder\n");
      destroyH264Decoder(decoder);
      decoder = NULL;
      logInfo("[cst] stream: decoder destroyed\n");
   }
   if (socketValue >= 0) socketclose(socketValue);
   streamLive = 0;
   lock(&statsLock);
   if (stats.state != STREAM_STATE_ERROR) setStreamStatusLocked(STREAM_STATE_WAITING, "waiting for server ...");
   unlock(&statsLock);
}

// keeps a stream up for as long as a server is there to talk to: finds one, streams from it, and goes
// back to looking the moment it goes away. no button, and nothing to restart by hand.
static void runStreamThread(uint64_t argument)
{
   (void)argument;
   setStreamStatus(STREAM_STATE_WAITING, "waiting for server ...");
   while (!stopRequested) {
      runStreamSession();
      if (!stopRequested) sleepMs(RECONNECT_DELAY_MS);
   }
   logInfo("[cst] stream: connect loop exit\n");
   streamRunning = 0;
   exitThread();
}

// frees the decoded-picture buffers. call from the DRAW thread only, and only after a few
// frames have been rendered without video, so the RSX cannot still be reading them.
void releaseStreamBuffers(void)
{
   if (streamLive || !buffersInUse) return;   // only ever between sessions
   int i;
   for (i = 0; i < YUV_BUFFER_COUNT; i++) {
      if (yuvBuffers[i]) { freeGfxVideoBuffer(yuvBuffers[i]); yuvBuffers[i] = NULL; }
   }
   buffersInUse = 0;   // the next session may now allocate its own
   logInfo("[cst] stream: buffers released\n");
}

// section: UI-thread API

void initStream(void)
{
   createLock(&statsLock);
   createLock(&frameLock);
   createLock(&queueLock);

   streamRunning = 1;
   stopRequested = 0;
   sys_ppu_thread_t threadId;
   int rc = spawnThread(&threadId, runStreamThread, 0, THREAD_PRIORITY_HIGH, THREAD_STACK_SIZE_64KB, "cst-stream");
   if (rc != 0) { logError("[cst] stream thread spawn failed rc=0x%x\n", rc); streamRunning = 0; }
}

void stopStream(void)
{
   stopRequested = 1;
}

int isStreamRunning(void) { return streamRunning; }
int isStreamLive(void) { return streamLive; }


void getStreamStats(StreamStats *out)
{
   lock(&statsLock);
   *out = stats;
   unlock(&statsLock);
}

void setStreamBuffered(int on)
{
   lock(&frameLock);
   bufferOneFrame = on ? 1 : 0;
   presentSeq = 0;   // re-prime the reserve when the mode changes mid-stream
   unlock(&frameLock);
}

// which buffer the draw thread should show next, or -1 to hold the current one. straight mode shows the
// newest undrawn picture; buffered mode advances one frame per call and stays one behind the newest, so a
// reserve is always in hand. presentSeq is advanced here only, and only when a frame is actually taken.
static int nextDrawIndexLocked(void)
{
   if (publishedIndex < 0) return -1;
   if (!bufferOneFrame) return publishedIndex != drawnIndex ? publishedIndex : -1;

   uint64_t target;
   if (presentSeq == 0) {                       // prime: need one decoded reserve before the first show
      if (publishSeq < 2) return -1;
      target = publishSeq - 1;                  // start one behind the newest
   } else {
      if (presentSeq + 1 > publishSeq) return -1;   // starved: the next frame is late, hold the current one
      target = presentSeq + 1;
      if (target < publishSeq - 1) target = publishSeq - 1;   // only the newest two are kept; skip stale ones
   }
   presentSeq = target;
   return target >= publishSeq ? publishedIndex : previousPublishedIndex;
}

// true when nextDrawIndexLocked would take a frame. straight mode: a newer picture exists; buffered mode:
// the reserve is primed and the next frame is in hand. drawing only then keeps the render loop paced by the
// arriving video (straight) or by the display's refresh (buffered), never spinning on the same picture.
// this is a non-mutating peek; keep its three take-conditions in lockstep with nextDrawIndexLocked (which
// advances presentSeq, so the gate can't share its body).
int isNewStreamPictureReady(void)
{
   lock(&frameLock);
   int ready;
   if (publishedIndex < 0) ready = 0;
   else if (!bufferOneFrame) ready = publishedIndex != drawnIndex;
   else if (presentSeq == 0) ready = publishSeq >= 2;
   else ready = presentSeq + 1 <= publishSeq;
   unlock(&frameLock);
   return ready;
}

void drawStreamFrame(void)
{
   lock(&frameLock);
   int index = nextDrawIndexLocked();
   if (index < 0) index = drawnIndex;   // nothing new: redraw whatever is on screen
   if (index >= 0) {
      // letterbox: scale to fit the screen preserving aspect ratio
      int screenWidth = getGfxScreenWidth(), screenHeight = getGfxScreenHeight();
      int drawWidth = screenWidth, drawHeight = publishedHeight * screenWidth / publishedWidth;
      if (drawHeight > screenHeight) { drawHeight = screenHeight; drawWidth = publishedWidth * screenHeight / publishedHeight; }
      drawGfxYuvFrame((screenWidth - drawWidth) / 2, (screenHeight - drawHeight) / 2, drawWidth, drawHeight,
                      yuvBuffers[index], publishedWidth, publishedHeight);

      // time only the first draw of each new picture: that's when it is handed to the RSX. the wait for
      // the display is NOT part of this - it is timed separately (see noteStreamFlipWait) because it
      // happens inside the next endGfxFrame, and folding it in here would credit it to the wrong frame.
      if (drawnIndex != index) {
         previousDrawnIndex = drawnIndex;
         drawnIndex = index;
         uint64_t now = getTimeUs();
         windowPresentUs += now - bufferDecodedUs[index];
         windowTotalUs += now - bufferCaptureUs[index];
         windowPresentedFrames++;
      }
   }
   unlock(&frameLock);
}

// the render loop reports how long endGfxFrame blocked waiting for the display to take the previous
// frame. that wait is the whole cost of vsync, and it is what the immediate-flip mode removes.
void noteStreamFlipWait(uint64_t microseconds)
{
   lock(&frameLock);
   windowFlipWaitUs += microseconds;
   windowFlipWaits++;
   unlock(&frameLock);
}
