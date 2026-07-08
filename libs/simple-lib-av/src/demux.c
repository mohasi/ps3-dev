// demux - container sniffing/dispatch and the shared Annex-B AU builder. See demux.h.
#include "demux.h"
#include "dbg.h"
#include <string.h>

// isSyncSample: the container marked this sample a random-access point (mp4 sync sample / trun sync flag).
// A fragment/segment can open on a non-IDR recovery-point I-frame, which carries no type-5 NAL; treat that
// like an IDR so a freshly reset decoder gets its SPS/PPS and the post-seek decode gate starts on it.
int buildVideoAu(const H264Config *h264, const uint8_t *sample, int sampleSize, int isSyncSample, uint8_t *auBuffer, int auCapacity, uint64_t pts, VideoAu *au)
{
   int reserve = h264->headerSize;   // room to prepend SPS/PPS ahead of a random-access frame
   int isIdr = 0;
   int annexLen = avccToAnnexB(sample, sampleSize, h264->nalLengthSize, auBuffer + reserve, auCapacity - reserve, &isIdr);
   if (annexLen <= 0) return 0;
   int isKeyframe = isIdr || isSyncSample;

   au->pts = pts;
   if (isKeyframe && reserve > 0) {
      memcpy(auBuffer, h264->header, reserve);
      au->data = auBuffer;
      au->size = reserve + annexLen;
      au->keyframe = 1;
   } else {
      au->data = auBuffer + reserve;
      au->size = annexLen;
      au->keyframe = isKeyframe;
   }
   return 1;
}

int openVideoDemuxer(VideoDemuxer *demuxer, const char *path)
{
   memset(demuxer, 0, sizeof *demuxer);

   // open the source once and sniff the container (MP4 has 'ftyp' at offset 4, else MKV), then hand
   // the still-open source to the chosen demuxer to adopt. Opening+closing it here just to sniff would,
   // over http, tear libhttp/libssl down mid-open (undrained connection) and fault the next request.
   VideoSource source;
   if (openVideoSource(&source, path) != 0) { logError("[demux] could not open source\n"); return -1; }
   uint8_t magic[8] = {0};
   readVideoSourceAt(&source, 0, magic, sizeof magic);
   demuxer->isMp4 = (magic[4] == 'f' && magic[5] == 't' && magic[6] == 'y' && magic[7] == 'p');

   if (demuxer->isMp4) {
      Mp4Demuxer *mp4 = &demuxer->container.mp4;
      if (openMp4Demuxer(mp4, &source, 0) != 0) return -1;   // adopts source (closes it on failure)
      demuxer->h264 = mp4->h264;                        demuxer->level = mp4->level;
      demuxer->width = mp4->width;                      demuxer->height = mp4->height;
      demuxer->hasAudio = mp4->hasAudio;
      demuxer->audioRate = mp4->audioRate;              demuxer->audioChannels = mp4->audioChannels;
      demuxer->audioAdtsRate = mp4->audioAdtsRate;
      demuxer->frameDurationNs = mp4->frameDurationNs;  demuxer->durationNs = mp4->durationNs;
   } else {
      MkvDemuxer *mkv = &demuxer->container.mkv;
      if (openMkvDemuxer(mkv, &source) != 0) return -1;   // adopts source (closes it on failure)
      demuxer->h264 = mkv->h264;                        demuxer->level = mkv->level;
      demuxer->width = mkv->width;                      demuxer->height = mkv->height;
      demuxer->hasAudio = mkv->audioTrack != 0;
      demuxer->audioRate = mkv->audioRate;              demuxer->audioChannels = mkv->audioChannels;
      demuxer->audioAdtsRate = mkv->audioRate;          // MKV AAC is AAC-LC: ADTS rate == output rate
      demuxer->frameDurationNs = mkv->frameDurationNs;  demuxer->durationNs = mkv->durationNs;
   }
   return 0;
}

// opens a standalone audio stream (a DASH/adaptive audio-only mp4, e.g. YouTube itag 139/140). The
// player pairs this with a separate video demuxer and reads its frames with readAudioAu.
int openAudioDemuxer(VideoDemuxer *demuxer, const char *path)
{
   memset(demuxer, 0, sizeof *demuxer);
   demuxer->isMp4 = 1;   // adaptive audio is mp4 (audio-only webm is not supported)
   VideoSource source;
   if (openVideoSource(&source, path) != 0) return -1;
   if (openMp4Demuxer(&demuxer->container.mp4, &source, 1) != 0) return -1;   // adopts source (closes it on failure)
   demuxer->hasAudio      = demuxer->container.mp4.hasAudio;
   demuxer->audioRate     = demuxer->container.mp4.audioRate;
   demuxer->audioAdtsRate = demuxer->container.mp4.audioAdtsRate;
   demuxer->audioChannels = demuxer->container.mp4.audioChannels;
   demuxer->durationNs    = demuxer->container.mp4.durationNs;
   if (!demuxer->hasAudio) { closeMp4Demuxer(&demuxer->container.mp4); return -1; }   // don't leak the open source
   return 0;
}
