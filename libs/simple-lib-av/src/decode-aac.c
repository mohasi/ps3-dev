// decode-aac - cellAdec M4AAC decoder wrapper. See decode-aac.h.
//
// cellAdec reports progress through a callback: AUDONE (an ADTS frame was consumed), PCMOUT (a
// decoded frame is ready), ERROR. Like the video decoder, it cannot consume the next AU while its
// internal PCM buffers are full, so nothing here blocks: the caller pulls PCM between feeds.
#include "decode-aac.h"
#include "thread.h"             // sleepMs
#include "dbg.h"                // logInfo / logError
#include <cell/codec/adec.h>
#include <cell/codec/adec_m4aac.h>
#include <cell/sysmodule.h>
#include <stdlib.h>
#include <string.h>

#define ADEC_PPU_PRIORITY   450
#define ADEC_SPU_PRIORITY   250
#define ADEC_PPU_STACK_SIZE (16 * 1024)
#define START_SEQ_RETRIES   100

// decoded output scratch: up to 7.1 slots of an SBR-doubled frame, 128-byte aligned for GetPcm
#define PCM_SCRATCH_BYTES   (AAC_MAX_FRAME_SAMPLES * 8 * (int)sizeof(float))

struct AacDecoder {
   CellAdecHandle handle;
   void          *workMemory;
   float         *pcmScratch;      // raw decoder output before the stereo conversion

   int            auFedCount;      // ADTS frames handed to cellAdecDecodeAu (audio thread only)
   volatile int   auDoneCount;     // incremented by the callback on each AUDONE
   volatile int   pcmPending;      // decoded frames reported but not yet retrieved
   volatile int   seqDone;         // EndSeq finished (set by the callback, cleared by reset)
   volatile int   errored;
   int            discardedCount;  // concealed/invalid frames dropped (each one skips audible content)
};

static int32_t adecCallback(CellAdecHandle handle, CellAdecMsgType type, int32_t data, void *arg)
{
   (void)handle;
   AacDecoder *decoder = (AacDecoder *)arg;
   switch (type) {
      case CELL_ADEC_MSG_TYPE_AUDONE:  decoder->auDoneCount++; break;
      // atomic: the audio thread decrements concurrently, and a lost update strands a frame inside the
      // decoder forever (it stays BUSY, audio goes silent). data may carry a decode error; GetPcm still required.
      case CELL_ADEC_MSG_TYPE_PCMOUT:  (void)__sync_add_and_fetch(&decoder->pcmPending, 1); break;
      case CELL_ADEC_MSG_TYPE_SEQDONE: decoder->seqDone = 1;   break;
      case CELL_ADEC_MSG_TYPE_ERROR:
         decoder->errored = 1;
         logError("[decode-aac] adec fatal error 0x%x\n", data);
         break;
   }
   return 0;
}

// ADTS input; multi-channel streams downmix to 2.0 in the decoder. Retried while the decoder is busy.
static int startSequence(AacDecoder *decoder)
{
   CellAdecParamM4Aac param;
   memset(&param, 0, sizeof param);
   param.configNumber  = 1;   // ADTS (the only input form this SDK documents as supported)
   param.enableDownmix = 1;

   int ret = CELL_OK;
   for (int attempt = 0; attempt < START_SEQ_RETRIES; attempt++) {
      ret = cellAdecStartSeq(decoder->handle, &param);
      if (ret != (int)CELL_ADEC_ERROR_BUSY) break;
      sleepMs(1);
   }
   return ret;
}

AacDecoder *createAacDecoder(void)
{
   AacDecoder *decoder = (AacDecoder *)calloc(1, sizeof *decoder);
   if (!decoder) return 0;

   int ret = cellSysmoduleLoadModule(CELL_SYSMODULE_ADEC_M4AAC);
   if (ret != CELL_OK && ret != CELL_SYSMODULE_ERROR_DUPLICATED) {
      logError("[decode-aac] load ADEC_M4AAC failed 0x%x\n", ret);
      goto fail;
   }

   CellAdecType type;
   type.audioCodecType = CELL_ADEC_TYPE_M4AAC;

   CellAdecAttr attr;
   ret = cellAdecQueryAttr(&type, &attr);
   if (ret != CELL_OK) { logError("[decode-aac] QueryAttr failed 0x%x\n", ret); goto fail; }

   size_t memSize = (attr.workMemSize + 4095) & ~(size_t)4095;
   decoder->workMemory = memalign(128, memSize);
   decoder->pcmScratch = (float *)memalign(128, PCM_SCRATCH_BYTES);
   if (!decoder->workMemory || !decoder->pcmScratch) goto fail;

   CellAdecResource resource;
   resource.totalMemSize       = memSize;
   resource.startAddr          = decoder->workMemory;
   resource.ppuThreadPriority  = ADEC_PPU_PRIORITY;
   resource.spuThreadPriority  = ADEC_SPU_PRIORITY;
   resource.ppuThreadStackSize = ADEC_PPU_STACK_SIZE;

   CellAdecCb callback = { adecCallback, decoder };
   ret = cellAdecOpen(&type, &resource, &callback, &decoder->handle);
   if (ret != CELL_OK) { logError("[decode-aac] Open failed 0x%x\n", ret); goto fail; }

   ret = startSequence(decoder);
   if (ret != CELL_OK) {
      logError("[decode-aac] StartSeq failed 0x%x\n", ret);
      cellAdecClose(decoder->handle); decoder->handle = 0;
      goto fail;
   }

   logInfo("[decode-aac] opened, workMem %d KB\n", (int)(memSize / 1024));
   return decoder;

fail:
   destroyAacDecoder(decoder);
   return 0;
}

int decodeAuAac(AacDecoder *decoder, const uint8_t *data, int size, uint64_t pts)
{
   if (decoder->errored) return -1;

   uint64_t ticks90kHz = pts / 100000 * 9 + pts % 100000 * 9 / 100000;   // ns -> 90 kHz, no overflow

   CellAdecAuInfo au;
   au.startAddr = (void *)data;
   au.size      = size;
   au.pts.upper = (uint32_t)(ticks90kHz >> 32) & 1;
   au.pts.lower = (uint32_t)ticks90kHz;
   au.userData  = pts;                                // full-resolution pts, echoed on the pcm item

   int ret = cellAdecDecodeAu(decoder->handle, &au);
   if (ret == CELL_OK) { decoder->auFedCount++; return 0; }
   if (ret == (int)CELL_ADEC_ERROR_BUSY) return 1;    // internal PCM full: pull PCM, then retry
   logError("[decode-aac] DecodeAu failed 0x%x\n", ret);
   return -1;
}

int getAuBacklogAac(const AacDecoder *decoder)
{
   return decoder->auFedCount - decoder->auDoneCount;
}

int getPcmAac(AacDecoder *decoder, float *stereoOut, int *outFrames, int *outRate, uint64_t *outPts)
{
   if (decoder->errored) return -1;
   if (decoder->pcmPending <= 0) return 0;

   const CellAdecPcmItem *item;
   if (cellAdecGetPcmItem(decoder->handle, &item) != CELL_OK) return 0;   // counter ran ahead: nothing queued yet
   (void)__sync_sub_and_fetch(&decoder->pcmPending, 1);

   const CellAdecM4AacInfo *info = (const CellAdecM4AacInfo *)item->pcmAttr.bsiInfo;
   int samplesPerFrame = info->enableSBR ? 1024 * (int)info->SBRUpsamplingFactor : 1024;
   int slots = samplesPerFrame > 0 ? (int)(item->size / (uint32_t)(samplesPerFrame * sizeof(float))) : 0;

   // concealed/invalid frames still occupy an internal slot: release and discard. Each discard skips
   // one frame of audible content, so repeated discards accumulate A/V desync - log so drift is traceable.
   if (item->status != 0 || item->size == 0 || slots < 1 || (int)item->size > PCM_SCRATCH_BYTES) {
      decoder->discardedCount++;
      if (decoder->discardedCount == 1 || decoder->discardedCount % 256 == 0)
         logWarn("[decode-aac] discarded frame #%d (status 0x%x size %d)\n", decoder->discardedCount, item->status, (int)item->size);
      cellAdecGetPcm(decoder->handle, 0);
      return 0;
   }

   if (cellAdecGetPcm(decoder->handle, decoder->pcmScratch) != CELL_OK) return -1;

   // to stereo: mono decodes as [Center, zero] pairs, so duplicate the centre; otherwise take L R
   int mono = info->numberOfChannels == 1;
   for (int i = 0; i < samplesPerFrame; i++) {
      float left  = decoder->pcmScratch[i * slots];
      float right = mono || slots < 2 ? left : decoder->pcmScratch[i * slots + 1];
      stereoOut[i * 2]     = left;
      stereoOut[i * 2 + 1] = right;
   }

   *outFrames = samplesPerFrame;
   *outRate   = (int)info->samplingFreq;
   *outPts    = item->auInfo.userData;
   return 1;
}

// releases decoded frames the callback has reported, one GetPcm(NULL discard) per PCMOUT
static void discardReportedPcm(AacDecoder *decoder)
{
   while (decoder->pcmPending > 0) { (void)__sync_sub_and_fetch(&decoder->pcmPending, 1); cellAdecGetPcm(decoder->handle, 0); }
}

int resetAacDecoder(AacDecoder *decoder)
{
   // end the sequence, discarding any frames it flushes out, then start a fresh one. Like the video
   // reset, one last drain runs after SEQDONE: a PCMOUT can land right before it, and an unreleased
   // frame would occupy an internal slot into the next sequence.
   decoder->seqDone = 0;
   int ret = CELL_OK;
   for (int attempt = 0; attempt < START_SEQ_RETRIES; attempt++) {
      ret = cellAdecEndSeq(decoder->handle);
      if (ret != (int)CELL_ADEC_ERROR_BUSY) break;
      discardReportedPcm(decoder);
      sleepMs(1);
   }
   if (ret != CELL_OK) { logError("[decode-aac] EndSeq failed 0x%x\n", ret); return -1; }

   for (int waited = 0; !decoder->seqDone && !decoder->errored && waited < 1000; waited++) {
      discardReportedPcm(decoder);
      sleepMs(1);
   }
   discardReportedPcm(decoder);
   if (!decoder->seqDone) { logError("[decode-aac] EndSeq never completed\n"); return -1; }

   decoder->auFedCount = 0;
   decoder->auDoneCount = 0;
   decoder->pcmPending = 0;
   decoder->errored = 0;

   ret = startSequence(decoder);
   if (ret != CELL_OK) { logError("[decode-aac] StartSeq (reset) failed 0x%x\n", ret); return -1; }
   return 0;
}

int buildAdtsHeader(uint8_t out[ADTS_HEADER_BYTES], int payloadBytes, int rate, int channels)
{
   static const int adtsRates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
   int freqIndex = -1;
   for (int i = 0; i < (int)(sizeof adtsRates / sizeof adtsRates[0]); i++) if (adtsRates[i] == rate) { freqIndex = i; break; }
   if (freqIndex < 0 || channels < 1 || channels > 6 || payloadBytes <= 0) return -1;

   int frameLength = payloadBytes + ADTS_HEADER_BYTES;
   out[0] = 0xFF;                                                        // syncword
   out[1] = 0xF1;                                                        // syncword, MPEG-4, layer 0, no CRC
   out[2] = (uint8_t)(0x40 | (freqIndex << 2) | (channels >> 2));        // AAC-LC profile, freq index, channel config high bit
   out[3] = (uint8_t)(((channels & 3) << 6) | ((frameLength >> 11) & 3));
   out[4] = (uint8_t)(frameLength >> 3);
   out[5] = (uint8_t)(((frameLength & 7) << 5) | 0x1F);                  // buffer fullness: all ones (VBR)
   out[6] = 0xFC;
   return 0;
}

void destroyAacDecoder(AacDecoder *decoder)
{
   if (!decoder) return;
   if (decoder->handle) { cellAdecEndSeq(decoder->handle); cellAdecClose(decoder->handle); }
   free(decoder->workMemory);
   free(decoder->pcmScratch);
   free(decoder);
}
