// demux - container sniffing/dispatch and the shared Annex-B AU builder. See demux.h.
#include "demux.h"
#include <string.h>

int buildVideoAu(const H264Config *h264, const uint8_t *sample, int sampleSize, uint8_t *auBuffer, int auCapacity, uint64_t pts, VideoAu *au)
{
   int reserve = h264->headerSize;   // room to prepend SPS/PPS on an IDR
   int isIdr = 0;
   int annexLen = avccToAnnexB(sample, sampleSize, h264->nalLengthSize, auBuffer + reserve, auCapacity - reserve, &isIdr);
   if (annexLen <= 0) return 0;

   au->pts = pts;
   if (isIdr && reserve > 0) {
      memcpy(auBuffer, h264->header, reserve);
      au->data = auBuffer;
      au->size = reserve + annexLen;
      au->keyframe = 1;
   } else {
      au->data = auBuffer + reserve;
      au->size = annexLen;
      au->keyframe = isIdr;
   }
   return 1;
}

int openVideoDemuxer(VideoDemuxer *demuxer, const char *path)
{
   memset(demuxer, 0, sizeof *demuxer);

   // sniff the container: MP4 has 'ftyp' at offset 4, anything else is tried as MKV
   VideoSource sniff;
   if (openVideoSource(&sniff, path) != 0) return -1;
   uint8_t magic[8] = {0};
   readVideoSourceAt(&sniff, 0, magic, sizeof magic);
   closeVideoSource(&sniff);
   demuxer->isMp4 = (magic[4] == 'f' && magic[5] == 't' && magic[6] == 'y' && magic[7] == 'p');

   if (demuxer->isMp4) {
      Mp4Demuxer *mp4 = &demuxer->container.mp4;
      if (openMp4Demuxer(mp4, path) != 0) return -1;
      demuxer->h264 = mp4->h264;                        demuxer->level = mp4->level;
      demuxer->width = mp4->width;                      demuxer->height = mp4->height;
      demuxer->hasAudio = mp4->hasAudio;
      demuxer->audioRate = mp4->audioRate;              demuxer->audioChannels = mp4->audioChannels;
      demuxer->frameDurationNs = mp4->frameDurationNs;  demuxer->durationNs = mp4->durationNs;
   } else {
      MkvDemuxer *mkv = &demuxer->container.mkv;
      if (openMkvDemuxer(mkv, path) != 0) return -1;
      demuxer->h264 = mkv->h264;                        demuxer->level = mkv->level;
      demuxer->width = mkv->width;                      demuxer->height = mkv->height;
      demuxer->hasAudio = mkv->audioTrack != 0;
      demuxer->audioRate = mkv->audioRate;              demuxer->audioChannels = mkv->audioChannels;
      demuxer->frameDurationNs = mkv->frameDurationNs;  demuxer->durationNs = mkv->durationNs;
   }
   return 0;
}
