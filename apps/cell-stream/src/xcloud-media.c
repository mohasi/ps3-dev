// xcloud-media - see xcloud-media.h.
//
// The decoders cannot be built until the stream describes itself, and it only does that in the
// packets themselves, so both are started on the first packet that carries what they need rather
// than up front.

#include "xcloud-media.h"

#include "audio.h"
#include "dbg.h"
#include "decode-h264.h"
#include "decode-opus.h"
#include "gfx.h"
#include "h264.h"
#include "picture-pool.h"
#include "rtp-h264.h"
#include "srtp.h"   // getRtpPayloadOffset
#include "thread.h"

#define TAG "[cst] "

// the number the machine gave the picture in its description; anything else is sound
#define VIDEO_KIND 102

#define SOUND_RATE       48000   // what the machine said its sound runs at
#define SOUND_CHANNELS   2
#define SOUND_FRAMES_MAX 960     // the longest packet opus sends at that rate, twenty milliseconds
#define SOUND_PRIME_MS   60      // held back before playing starts, so a late packet is not a gap

// the piece of a picture that is a complete picture, and the piece that describes the stream
#define PIECE_COMPLETE    5
#define PIECE_DESCRIPTION 7

static H264Decoder *pictureDecoder;

// sized from the stream itself, so the buffers cannot be taken up front
static PicturePool picturePool;
static int pictureWidth, pictureHeight;
static sys_lwmutex_t pictureLock;

static int feedingPictures;      // set once a complete picture has arrived to build the rest on

static OpusDecoderHandle *soundDecoder;
static int soundFeedOpen;

static int pictures, pictureFailures;

// How long the console itself adds: from the last packet of a picture arriving to that picture
// being ready to show. The rest of the delay is the machine and the network between, which cannot
// be seen from this end without a clock both ends agree on.
static uint64_t decodeTotalUs, decodeWorstUs;
static int decodeSamples;

void openXcloudMedia(void)
{
   createLock(&pictureLock);
   resetPicturePool(&picturePool);
   openRtpH264();
   feedingPictures = 0;
   pictures = 0;
   pictureFailures = 0;
   decodeTotalUs = 0;
   decodeWorstUs = 0;
   decodeSamples = 0;
   pictureWidth = 0;
   pictureHeight = 0;
}

void closeXcloudMedia(void)
{
   if (pictureDecoder) { destroyH264Decoder(pictureDecoder); pictureDecoder = 0; }

   closeRtpH264();
   freePicturePool(&picturePool);
   destroyLock(&pictureLock);
   if (soundDecoder) { destroyOpusDecoder(soundDecoder); soundDecoder = 0; }
   if (soundFeedOpen) { closeAudioPcmFeed(); soundFeedOpen = 0; }
}

// section: the picture

// The size the decoder is given has to be the size the stream codes, which is not always the size
// it is displayed at. Too small decodes nothing, too large locks the console, so it is read from
// the stream rather than assumed.
static int openPictureDecoder(const uint8_t *picture, int length)
{
   H264StreamInfo info;
   if (readH264StreamInfo(picture, length, &info) != 0) return -1;

   // Anything already taken is given back on the way out. Without that, a failure here left the
   // buffers held while the caller tried again on the very next picture, so a single refusal
   // emptied main memory in a fraction of a second.
   size_t bytes = ((size_t)info.codedWidth * info.codedHeight * 3 / 2 + 127) & ~(size_t)127;
   if (allocPicturePool(&picturePool, bytes) != 0) return -1;

   pictureDecoder = createH264Decoder(info.codedWidth, info.codedHeight, info.level, info.maxRefFrames);
   if (!pictureDecoder) { freePicturePool(&picturePool); return -1; }

   logInfo(TAG "the stream codes %dx%d, level %d, %d reference pictures\n", info.codedWidth, info.codedHeight,
           info.level, info.maxRefFrames);
   return 0;
}

// Each piece of a picture is introduced by a four byte marker and then a byte saying what it is.
static int containsPieceOfType(const uint8_t *picture, int length, int type)
{
   for (int at = 0; at + 4 < length; at++)
      if (picture[at] == 0 && picture[at + 1] == 0 && picture[at + 2] == 0 && picture[at + 3] == 1)
         if ((picture[at + 4] & 0x1F) == type) return 1;
   return 0;
}

// takes whatever the decoder has finished with, so it does not fill up and refuse the next one.
// a picture delivered answers 1; anything else means there is none waiting.
static int takeFinishedPictures(void)
{
   int taken = 0;
   for (;;) {
      lock(&pictureLock);
      int into = getFreePicture(&picturePool);
      unlock(&pictureLock);

      int width = 0, height = 0;
      uint64_t when = 0;
      if (getFrameH264(pictureDecoder, picturePool.buffers[into], &width, &height, &when) != 1) return taken;

      // only the newest is worth showing: anything older is already late
      lock(&pictureLock);
      picturePool.previouslyPublished = picturePool.published;
      picturePool.published = into;
      pictureWidth = width;
      pictureHeight = height;
      unlock(&pictureLock);

      taken++;
   }
}

static int feedPicture(const uint8_t *packet, int length)
{
   const uint8_t *picture = 0;
   int pictureLength = feedRtpH264(packet, length, &picture);
   if (pictureLength <= 0) return 0;
   pictures++;

   if (!pictureDecoder && openPictureDecoder(picture, pictureLength) != 0) return 0;

   // The machine sends the stream's description as a picture of its own, just before the complete
   // picture it applies to. The decoder needs that description, so it is fed as soon as it
   // arrives. What must wait is the pictures that only say what changed, because there is nothing
   // yet for them to change. Once a complete one has arrived none of this applies again, and the
   // search is skipped: it reads every byte of every picture, which at this bitrate is millions of
   // bytes a second on the thread that also has to keep the socket drained.
   if (!feedingPictures) {
      if (containsPieceOfType(picture, pictureLength, PIECE_COMPLETE)) feedingPictures = 1;
      else if (!containsPieceOfType(picture, pictureLength, PIECE_DESCRIPTION)) return 0;
   }

   uint64_t startedAt = getTimeUs();
   if (decodeAuH264(pictureDecoder, picture, pictureLength, (uint64_t)pictures) < 0) pictureFailures++;

   int ready = takeFinishedPictures() > 0;
   if (ready) {
      uint64_t took = getTimeUs() - startedAt;
      decodeTotalUs += took;
      if (took > decodeWorstUs) decodeWorstUs = took;
      decodeSamples++;
   }
   return ready;
}

void drawXcloudMedia(void)
{
   lock(&pictureLock);
   int showing = picturePool.published;
   int width = pictureWidth, height = pictureHeight;
   if (showing >= 0 && showing != picturePool.drawn) {
      picturePool.previouslyDrawn = picturePool.drawn;
      picturePool.drawn = showing;
   }
   unlock(&pictureLock);

   const uint8_t *picture = showing >= 0 ? picturePool.buffers[showing] : 0;
   if (!picture || width <= 0) return;

   int atX, atY, drawWidth, drawHeight;
   getGfxLetterboxRect(width, height, &atX, &atY, &drawWidth, &drawHeight);
   drawGfxYuvFrame(atX, atY, drawWidth, drawHeight, picture, width, height);
}

// section: the sound

// the feed wants pairs of fractions and the decoder gives whole numbers, so they are converted on
// the way through
static void feedSound(const uint8_t *packet, int length)
{
   if (!soundDecoder) {
      if (openAudioPcmFeed(SOUND_RATE, SOUND_RATE * SOUND_PRIME_MS / 1000) != 0) return;
      soundFeedOpen = 1;

      soundDecoder = createOpusDecoder(SOUND_RATE, SOUND_CHANNELS);
      if (!soundDecoder) return;
   }

   int payloadAt = getRtpPayloadOffset(packet, length);
   if (payloadAt < 0 || payloadAt >= length) return;

   int16_t samples[SOUND_FRAMES_MAX * SOUND_CHANNELS];
   int frames = decodeOpus(soundDecoder, packet + payloadAt, length - payloadAt, samples, SOUND_FRAMES_MAX);
   if (frames <= 0) return;

   float feed[SOUND_FRAMES_MAX * SOUND_CHANNELS];
   for (int sample = 0; sample < frames * SOUND_CHANNELS; sample++) feed[sample] = samples[sample] / 32768.0f;
   pushAudioPcm(feed, frames);
}

// section: what arrives

int feedXcloudMedia(const uint8_t *packet, int length)
{
   if ((packet[1] & 0x7F) == VIDEO_KIND) return feedPicture(packet, length);

   feedSound(packet, length);
   return 0;
}

void reportXcloudMedia(void)
{
   logInfo(TAG "media: %d pictures, %d incomplete, %d the decoder would not take\n", pictures,
           getRtpH264Dropped(), pictureFailures);

   if (decodeSamples > 0)
      logInfo(TAG "decoding a picture takes %d us on average, %d at worst, over %d of them\n",
              (int)(decodeTotalUs / decodeSamples), (int)decodeWorstUs, decodeSamples);
}
