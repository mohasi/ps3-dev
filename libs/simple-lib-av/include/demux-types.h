#pragma once

// demux-types - the container-independent pieces both demuxers (MKV, MP4) share: the access-unit
// types the player consumes, the single-producer/single-consumer audio AU queue, and the Annex-B
// access-unit builder.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "h264.h"
#include "dbg.h"

typedef struct {
   const uint8_t *data;   // Annex-B access unit (points into the demuxer's internal buffer)
   int      size;
   uint64_t pts;          // presentation time in nanoseconds (0 if unknown)
   int      keyframe;     // 1 when this access unit is an IDR (carries SPS/PPS)
} VideoAu;

#define AUDIO_AU_MAX_BYTES  4096   // one raw AAC frame (5.1 high-bitrate frames stay well under this)
#define AUDIO_AU_QUEUE_SIZE 64     // power of two; ~1.4 s of AAC at 48 kHz

typedef struct {
   uint8_t  data[AUDIO_AU_MAX_BYTES];
   int      size;
   uint64_t pts;          // presentation time in nanoseconds
} AudioAu;

#define AU_BUFFER_COUNT 4                     // rotating video AU buffers (bounds how far the decoder queue runs ahead)
#define AU_BUFFER_SIZE  (4 * 1024 * 1024)     // holds one Annex-B access unit (+ SPS/PPS on an IDR)

// audio AU queue: single producer (the video thread demuxing), single consumer (the audio decode
// thread). Overflow drops the newest AU.
typedef struct {
   AudioAu *slots;        // heap ring of AUDIO_AU_QUEUE_SIZE (NULL when no audio track)
   volatile uint32_t head, tail;
   int dropWarned;
} AudioAuQueue;

static inline int createAudioAuQueue(AudioAuQueue *queue)   // 0 / -1
{
   queue->slots = (AudioAu *)malloc(AUDIO_AU_QUEUE_SIZE * sizeof(AudioAu));
   queue->head = queue->tail = 0;
   queue->dropWarned = 0;
   return queue->slots ? 0 : -1;
}

static inline void destroyAudioAuQueue(AudioAuQueue *queue)
{
   free(queue->slots);
   queue->slots = 0;
}

static inline int isAudioAuQueueFull(const AudioAuQueue *queue) { return queue->head - queue->tail >= AUDIO_AU_QUEUE_SIZE; }

// discards everything queued; only call with the consumer parked (e.g. during a seek)
static inline void clearAudioAuQueue(AudioAuQueue *queue) { queue->tail = queue->head; }

static inline void enqueueAudioAu(AudioAuQueue *queue, const uint8_t *data, int size, uint64_t pts)
{
   if (size <= 0 || size > AUDIO_AU_MAX_BYTES) return;
   uint32_t head = queue->head;
   if (head - queue->tail >= AUDIO_AU_QUEUE_SIZE) {   // consumer stalled: drop the newest
      if (!queue->dropWarned) { logWarn("[demux] audio queue full, dropping\n"); queue->dropWarned = 1; }
      return;
   }
   AudioAu *slot = &queue->slots[head & (AUDIO_AU_QUEUE_SIZE - 1)];
   memcpy(slot->data, data, size);
   slot->size = size;
   slot->pts  = pts;
   __sync_synchronize();               // the slot must be fully written before it becomes visible
   queue->head = head + 1;
}

static inline int takeQueuedAudioAu(AudioAuQueue *queue, AudioAu *au)   // 1 = got one, 0 = queue empty
{
   uint32_t tail = queue->tail;
   if (queue->head == tail) return 0;
   __sync_synchronize();               // see the slot contents the producer published
   *au = queue->slots[tail & (AUDIO_AU_QUEUE_SIZE - 1)];
   __sync_synchronize();               // finish the copy before the producer may reuse the slot
   queue->tail = tail + 1;
   return 1;
}

// converts one length-prefixed (AVCC) sample into an Annex-B access unit in auBuffer, prepending the SPS/PPS
// header when the sample is a random-access point (IDR, or a container-marked sync sample). fills `au`;
// returns 1, or 0 when malformed / doesn't fit.
int buildVideoAu(const H264Config *h264, const uint8_t *sample, int sampleSize, int isSyncSample, uint8_t *auBuffer, int auCapacity, uint64_t pts, VideoAu *au);
