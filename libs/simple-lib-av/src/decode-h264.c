// decode-h264 - cellVdec H.264 decoder wrapper. See decode-h264.h.
//
// cellVdec runs asynchronously on its own PPU thread + 4 SPUs and reports progress through a
// callback: AUDONE (an access unit was consumed), PICOUT (a decoded picture is ready), ERROR. We
// feed access units and, per the sample, retrieve each picture with GetPicItem + GetPicture.
//
// Nothing here blocks. The decoder cannot consume the next AU until retrieved pictures free its
// internal frame buffers, so waiting for AUDONE without pulling pictures deadlocks (and times out).
// Instead decodeAuH264 just feeds (reporting BUSY back to the caller), isAuConsumedH264 tells the
// caller when the AU buffer may be reused, and the caller keeps pulling pictures in between.
#include "decode-h264.h"
#include "thread.h"             // createLock / lock / sleepMs
#include "dbg.h"                // logInfo / logError
#include <cell/codec/vdec.h>
#include <cell/codec/vdec_avc.h>
#include <cell/sysmodule.h>
#include <sys/spu_initialize.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// cellVdec resource tuning (from the SDK pamf_dmux sample): AVC uses 4 SPUs.
#define VDEC_SPU_COUNT       4
#define VDEC_PPU_PRIORITY    720
#define VDEC_SPU_PRIORITY    200
#define VDEC_PPU_STACK_SIZE  (16 * 1024)

struct H264Decoder {
   CellVdecHandle handle;
   void          *workMemory;
   int            width, height;   // coded size the caller sized its frame buffer for
   sys_lwmutex_t  lock;

   int            auFedCount;     // AUs handed to cellVdecDecodeAu (decode thread only)
   volatile int   auDoneCount;     // incremented by the callback on each AUDONE
   volatile int   picPending;      // decoded pictures reported but not yet retrieved
   volatile int   seqDone;         // EndSeq finished (set by the callback, cleared by reset)
   volatile int   errored;
   int            auRejectedCount;   // AUs the decoder refused (a broken stream refuses every one)
   int            picIncompleteCount;   // pictures that came out damaged
};

static uint32_t vdecCallback(CellVdecHandle handle, CellVdecMsgType type, int32_t data, void *arg)
{
   (void)handle;
   H264Decoder *decoder = (H264Decoder *)arg;
   lock(&decoder->lock);
   switch (type) {
      // a rejected access unit is reported here, NOT as MSG_TYPE_ERROR (which is fatal-only): the
      // decoder still emits a picture for it, so without this line a broken stream looks healthy.
      // throttled: a broken stream rejects every AU, which would be 30-60 lines a second
      case CELL_VDEC_MSG_TYPE_AUDONE:
         decoder->auDoneCount++;
         if (data != CELL_OK) {
            decoder->auRejectedCount++;
            if (decoder->auRejectedCount == 1 || decoder->auRejectedCount % 256 == 0)
               logWarn("[decode-h264] decoder rejected access unit #%d, rc=0x%x\n", decoder->auRejectedCount, data);
         }
         break;
      case CELL_VDEC_MSG_TYPE_PICOUT: decoder->picPending++;  break;
      case CELL_VDEC_MSG_TYPE_SEQDONE: decoder->seqDone = 1;  break;
      case CELL_VDEC_MSG_TYPE_ERROR:
         decoder->errored = 1;
         logError("[decode-h264] vdec fatal error 0x%x\n", data);
         break;
   }
   unlock(&decoder->lock);
   return 0;
}

// asks cellVdec (via the Ex query) for `buffers` decoded-frame buffers at this level/size; returns the
// required work-memory size, or 0 if the decoder rejects that buffer count.
static size_t queryWorkMemory(int width, int height, int level, int buffers)
{
   CellVdecAvcSpecificInfo avcInfo = { sizeof avcInfo, (uint16_t)width, (uint16_t)height, false, (uint8_t)buffers };
   CellVdecTypeEx type = { CELL_VDEC_CODEC_TYPE_AVC, (uint32_t)level, &avcInfo };
   CellVdecAttr attr;
   return cellVdecQueryAttrEx(&type, &attr) == CELL_OK ? attr.memSize : 0;
}

H264Decoder *createH264Decoder(int width, int height, int level, int maxRefFrames)
{
   H264Decoder *decoder = (H264Decoder *)calloc(1, sizeof *decoder);
   if (!decoder) return 0;
   decoder->width  = width;
   decoder->height = height;
   createLock(&decoder->lock);

   // SPU runtime + codec module are process-global: init the SPUs once, and tolerate a duplicate
   // module load if another decoder already brought it up.
   static int spuInitialized;
   if (!spuInitialized) { sys_spu_initialize(6, 0); spuInitialized = 1; }
   int ret = cellSysmoduleLoadModule(CELL_SYSMODULE_VDEC_AVC);
   if (ret != CELL_OK && ret != CELL_SYSMODULE_ERROR_DUPLICATED) {
      logError("[decode-h264] load VDEC_AVC failed 0x%x\n", ret);
      goto fail;
   }

   // the decoded-picture buffer must hold every reference frame plus the one being decoded. Ask for
   // refs+1 and, if the decoder rejects it, step down to the largest count it accepts.
   int wantBuffers = maxRefFrames > 0 ? maxRefFrames + 1 : 4;
   size_t memSize = 0; int gotBuffers = 0;
   for (int buffers = wantBuffers; buffers >= 2 && memSize == 0; buffers--) {
      memSize = queryWorkMemory(width, height, level, buffers);
      if (memSize) gotBuffers = buffers;
   }
   if (!memSize) { logError("[decode-h264] no acceptable DPB size at %dx%d level %d\n", width, height, level); goto fail; }

   decoder->workMemory = malloc(memSize);
   if (!decoder->workMemory) goto fail;

   CellVdecAvcSpecificInfo avcInfo = { sizeof avcInfo, (uint16_t)width, (uint16_t)height, false, (uint8_t)gotBuffers };
   CellVdecTypeEx type = { CELL_VDEC_CODEC_TYPE_AVC, (uint32_t)level, &avcInfo };

   CellVdecResourceEx resource;
   resource.memAddr            = decoder->workMemory;
   resource.memSize            = memSize;
   resource.ppuThreadPriority  = VDEC_PPU_PRIORITY;
   resource.ppuThreadStackSize = VDEC_PPU_STACK_SIZE;
   resource.spuThreadPriority  = VDEC_SPU_PRIORITY;
   resource.numOfSpus          = VDEC_SPU_COUNT;
   resource.spursResource      = 0;   // NULL: use raw SPU threads, not a user SPURS instance

   CellVdecCb callback = { vdecCallback, decoder };
   ret = cellVdecOpenEx(&type, &resource, &callback, &decoder->handle);
   if (ret != CELL_OK) { logError("[decode-h264] OpenEx failed 0x%x\n", ret); goto fail; }

   ret = cellVdecStartSeq(decoder->handle);
   if (ret != CELL_OK) { logError("[decode-h264] StartSeq failed 0x%x\n", ret); cellVdecClose(decoder->handle); decoder->handle = 0; goto fail; }

   logInfo("[decode-h264] opened %dx%d level %d, %d DPB buffers, workMem %d KB\n", width, height, level, gotBuffers, (int)(memSize / 1024));
   return decoder;

fail:
   destroyH264Decoder(decoder);
   return 0;
}

int decodeAuH264(H264Decoder *decoder, const uint8_t *data, int size, uint64_t pts)
{
   if (decoder->errored) return -1;

   uint64_t ticks90kHz = pts / 100000 * 9 + pts % 100000 * 9 / 100000;   // ns -> 90 kHz, no overflow

   CellVdecAuInfo au;
   au.startAddr         = (void *)data;
   au.size              = size;
   au.pts.upper         = (uint32_t)(ticks90kHz >> 32) & 1;   // the decoder expects 33-bit 90 kHz stamps
   au.pts.lower         = (uint32_t)ticks90kHz;
   au.dts.upper         = CELL_VDEC_DTS_INVALID;
   au.dts.lower         = CELL_VDEC_DTS_INVALID;
   au.userData          = pts;                                // full-resolution pts, echoed on the pic item
   au.codecSpecificData = 0;

   int ret = cellVdecDecodeAu(decoder->handle, CELL_VDEC_DEC_MODE_NORMAL, &au);
   if (ret == CELL_OK) { decoder->auFedCount++; return 0; }
   if (ret == (int)CELL_VDEC_ERROR_BUSY) return 1;   // queues full: pull pictures, then retry this AU
   logError("[decode-h264] DecodeAu failed 0x%x\n", ret);
   return -1;
}

int getAuBacklogH264(const H264Decoder *decoder)
{
   return decoder->auFedCount - decoder->auDoneCount;
}

// releases decoded pictures the callback has reported, one GetPicItem + GetPicture(NULL discard)
// pair per PICOUT — the SDK samples never retrieve speculatively, only in response to a PICOUT
static void discardReportedPictures(H264Decoder *decoder, const CellVdecPicFormat *format)
{
   while (decoder->picPending > 0) {
      lock(&decoder->lock); decoder->picPending--; unlock(&decoder->lock);
      const CellVdecPicItem *picItem;
      if (cellVdecGetPicItem(decoder->handle, &picItem) == CELL_OK) cellVdecGetPicture(decoder->handle, format, 0);
   }
}

int resetH264Decoder(H264Decoder *decoder)
{
   // flush for a seek per the SDK protocol (pamf_dmux_trick_play, libavdecode): EndSeq retried while
   // the input queue is full, every flushed picture released as its PICOUT arrives until SEQDONE
   // (one last drain after it — a PICOUT can land between our final check and SEQDONE, and a picture
   // left unreleased starves the refs+1-sized DPB of the next sequence), then StartSeq on the same
   // handle, retried — Sony's own middleware notes StartSeq "sometimes fails with 'fatal' errors
   // that aren't fatal" right after a flush and brute-forces it.
   decoder->seqDone = 0;

   int ret;
   for (int tries = 0; (ret = cellVdecEndSeq(decoder->handle)) == (int)CELL_VDEC_ERROR_BUSY && tries < 1000; tries++) sleepMs(1);
   if (ret != CELL_OK) { logError("[decode-h264] EndSeq failed 0x%x\n", ret); return -1; }

   CellVdecPicFormat format;
   format.formatType      = CELL_VDEC_PICFMT_YUV420_PLANAR;
   format.colorMatrixType = CELL_VDEC_COLOR_MATRIX_TYPE_BT709;
   format.alpha           = 0xFF;

   for (int waited = 0; !decoder->seqDone && !decoder->errored && waited < 2000; waited++) {
      discardReportedPictures(decoder, &format);
      sleepMs(1);
   }
   discardReportedPictures(decoder, &format);

   if (!decoder->seqDone) {
      logError("[decode-h264] EndSeq never completed%s\n", decoder->errored ? " (decoder errored)" : "");
      return -1;
   }

   decoder->auFedCount = 0;
   decoder->auDoneCount = 0;
   decoder->picPending = 0;

   for (int tries = 0; (ret = cellVdecStartSeq(decoder->handle)) != CELL_OK && tries < 512; tries++) sleepMs(1);
   if (ret != CELL_OK) { logError("[decode-h264] StartSeq (reset) failed 0x%x\n", ret); return -1; }
   return 0;
}

int getFrameH264(H264Decoder *decoder, void *yuvOut, int *outWidth, int *outHeight, uint64_t *outPts)
{
   if (decoder->errored) return -1;
   if (decoder->picPending <= 0) return 0;

   const CellVdecPicItem *picItem;
   if (cellVdecGetPicItem(decoder->handle, &picItem) != CELL_OK) return 0;   // counter ran ahead: nothing queued yet
   lock(&decoder->lock); decoder->picPending--; unlock(&decoder->lock);

   // throttled: a degraded stream damages every picture, which would be 30-60 lines a second
   if (picItem->status != CELL_OK) {
      decoder->picIncompleteCount++;
      if (decoder->picIncompleteCount == 1 || decoder->picIncompleteCount % 256 == 0)
         logWarn("[decode-h264] picture #%d came out incomplete, status=0x%x\n", decoder->picIncompleteCount, picItem->status);
   }

   const CellVdecAvcInfo *info = (const CellVdecAvcInfo *)picItem->picInfo;
   int frameW = info->horizontalSize;
   int frameH = info->verticalSize;
   *outPts = picItem->auUserData[0];   // the ns pts fed with the AU; auPts is the truncated 90 kHz stamp

   // YUV planar output: the decoder skips its (SPU-costly) colorspace conversion and hands back the
   // native planes; the RSX converts to RGB in a fragment shader at draw time
   CellVdecPicFormat format;
   format.formatType     = CELL_VDEC_PICFMT_YUV420_PLANAR;
   format.colorMatrixType = frameH >= 720 ? CELL_VDEC_COLOR_MATRIX_TYPE_BT709 : CELL_VDEC_COLOR_MATRIX_TYPE_BT601;
   format.alpha          = 0xFF;

   // a skipped picture (B_SKIP mode) has no data but must still be released with a NULL GetPicture,
   // or the decoder's output queue clogs and it stops consuming input (per the SDK vpost sample)
   if (picItem->attr == CELL_VDEC_PICITEM_ATTR_SKIPPED) {
      cellVdecGetPicture(decoder->handle, &format, 0);
      return 0;
   }

   int ret = cellVdecGetPicture(decoder->handle, &format, yuvOut);
   if (ret != CELL_OK) { logError("[decode-h264] GetPicture failed 0x%x\n", ret); return -1; }

   *outWidth  = frameW;
   *outHeight = frameH;
   return 1;
}

void destroyH264Decoder(H264Decoder *decoder)
{
   if (!decoder) return;
   if (decoder->handle) { cellVdecEndSeq(decoder->handle); cellVdecClose(decoder->handle); }
   free(decoder->workMemory);
   destroyLock(&decoder->lock);
   free(decoder);
}
