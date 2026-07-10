// downloads - background queue that remuxes a video's streams into one .mkv on disk (see downloads.h).

#include "downloads.h"
#include "stream-select.h"      // pickBestVideo / pickBestAudio
#include "mux-mkv.h"
#include "demux.h"              // VideoDemuxer + readVideoAu / readAudioAu
#include "extractor.h"

#include "vfs.h"                // openFs/writeFs + makeDir/deleteFile + MAX_PATH_LEN
#include "thread.h"            // lwmutex + spawnJoinableThread
#include "string-utilities.h"   // strCopy
#include "font.h"
#include "ui/label.h"
#include "colors.h"
#include "gfx.h"
#include "dbg.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define DOWNLOAD_DIR       "/dev_hdd0/tmp/yo-player/downloads"
#define MAX_QUEUE          16
#define TITLE_FILENAME_MAX 100      // longest title kept in a filename before truncating
#define DISPLAY_TITLE_MAX  32       // longest title shown in the "Downloading ..." overlay
#define OVERLAY_TEXT_SIZE  19       // matches the grid's Watch Later / duration badge
#define OVERLAY_BG         0xCC000000

typedef struct { char videoId[16]; char title[160]; } QueueItem;

static struct {
   sys_lwmutex_t    mutex;         // guards queue[], count, workerSpawned
   QueueItem        queue[MAX_QUEUE];
   int              count;
   int              workerSpawned;
   sys_ppu_thread_t worker;

   volatile int     cancelAll;     // app exit: abort the current download and drain the queue
   volatile int     active;        // a download is currently running (drives the overlay)
   volatile int     percent;
   char             activeTitle[160];
   char             activeId[16];  // videoId of the running download (for dedupe; valid while active)

   int              initialized;
} dl;

static Font  font;
static Label overlayLabel;

// ---- filename ----

// map a video title to a filesystem-safe name: drop control bytes, turn reserved characters into spaces,
// and trim trailing spaces/dots. UTF-8 continuation bytes (>= 0x80) pass through untouched.
static void sanitizeTitle(const char *title, char *out, int cap)
{
   int j = 0;
   for (int i = 0; title[i] && j < cap - 1; i++) {
      unsigned char ch = (unsigned char)title[i];
      if (ch < 0x20) continue;
      out[j++] = strchr("/\\:*?\"<>|", ch) ? ' ' : (char)ch;
   }
   while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '.')) j--;
   out[j] = 0;
}

static void buildDownloadPath(char *path, int cap, const char *title, const char *videoId)
{
   char clean[160];
   sanitizeTitle(title, clean, sizeof clean);
   if (!clean[0]) strCopy(clean, sizeof clean, "video");
   if ((int)strlen(clean) > TITLE_FILENAME_MAX) clean[TITLE_FILENAME_MAX] = 0;
   snprintf(path, cap, "%s/%s [%s].mkv", DOWNLOAD_DIR, clean, videoId);
}

// ---- the remux itself ----

// remux the video (and optional audio) url into one .mkv at outPath. reads elementary access units from
// the demuxers and writes MKV blocks, checking cancelAll each unit. 0 on a complete file, -1 otherwise
// (the partial file is removed by the caller).
static int remux(const char *videoUrl, const char *audioUrl, const char *outPath)
{
   VideoDemuxer *video = (VideoDemuxer *)malloc(sizeof *video);
   VideoDemuxer *audio = audioUrl ? (VideoDemuxer *)malloc(sizeof *audio) : NULL;
   if (!video || (audioUrl && !audio)) { free(video); free(audio); return -1; }

   int result = -1;
   int videoOpen = 0, hasAudio = 0;
   MkvMuxer mux;

   if (openVideoDemuxer(video, videoUrl) != 0) { logError("[dl] video demux open failed\n"); goto cleanup; }
   videoOpen = 1;
   if (audio && openAudioDemuxer(audio, audioUrl) == 0) hasAudio = 1;

   if (openMkvMuxer(&mux, outPath, &video->h264, video->width, video->height, video->frameDurationNs,
                    video->durationNs, hasAudio ? audio->audioRate : 0, hasAudio ? audio->audioChannels : 0) != 0)
      goto cleanup;

   // interleave by presentation time: hold one pending AU from each stream and write whichever comes first.
   VideoAu videoAu;
   AudioAu audioAu;
   int videoRc = readVideoAu(video, &videoAu);
   int audioRc = hasAudio ? readAudioAu(audio, &audioAu) : 0;
   uint64_t total = video->durationNs;
   int failed = 0;

   while (!dl.cancelAll && (videoRc == 1 || audioRc == 1)) {
      if (videoRc == 1 && (audioRc != 1 || videoAu.pts <= audioAu.pts)) {
         if (writeMkvVideo(&mux, videoAu.data, videoAu.size, videoAu.pts, videoAu.keyframe)) { failed = 1; break; }
         if (total) { uint64_t done = videoAu.pts * 100 / total; dl.percent = done > 100 ? 100 : (int)done; }
         videoRc = readVideoAu(video, &videoAu);
         if (videoRc < 0) { failed = 1; break; }
      } else {
         if (writeMkvAudio(&mux, audioAu.data, audioAu.size, audioAu.pts)) { failed = 1; break; }
         audioRc = readAudioAu(audio, &audioAu);
         if (audioRc < 0) { failed = 1; break; }
      }
   }

   if (closeMkvMuxer(&mux)) failed = 1;
   if (!failed && !dl.cancelAll) { dl.percent = 100; result = 0; }

cleanup:
   if (videoOpen) closeVideoDemuxer(video);
   if (hasAudio) closeVideoDemuxer(audio);
   free(video);
   free(audio);
   return result;
}

static void runOneDownload(const QueueItem *item)
{
   StreamInfo *info = (StreamInfo *)malloc(sizeof *info);
   const Extractor *extractor = findExtractor(item->videoId);
   if (!info || !extractor || extractor->extract(item->videoId, info) != 0 || info->formatCount == 0) {
      logError("[dl] resolve failed for %s\n", item->videoId);
      free(info);
      return;
   }

   const StreamFormat *video = pickBestVideo(info);
   const StreamFormat *audio = (video && !video->hasAudio) ? pickBestAudio(info) : NULL;
   if (!video) { logError("[dl] no downloadable video for %s\n", item->videoId); free(info); return; }
   if (video->isLiveSegmented) { logInfo("[dl] %s is a live stream, not downloadable\n", item->videoId); free(info); return; }

   char finalPath[MAX_PATH_LEN], partPath[MAX_PATH_LEN];
   buildDownloadPath(finalPath, sizeof finalPath, item->title, item->videoId);
   snprintf(partPath, sizeof partPath, "%s.part", finalPath);
   logInfo("[dl] %s -> %s (video itag %d, audio itag %d)\n", item->videoId, finalPath, video->itag, audio ? audio->itag : (video->hasAudio ? video->itag : 0));

   // download to a .part file and only publish it on success, so an interrupted or duplicate download can
   // never truncate or delete an already-finished file.
   int rc = remux(video->url, audio ? audio->url : NULL, partPath);
   free(info);

   if (rc != 0) { deleteFile(partPath); logInfo("[dl] %s %s\n", item->videoId, dl.cancelAll ? "cancelled" : "failed"); }
   else if (renamePath(partPath, finalPath) != 0) { deleteFile(partPath); logError("[dl] %s rename failed\n", item->videoId); }
   else logInfo("[dl] %s done\n", item->videoId);
}

// ---- worker + queue ----

// one persistent low-priority worker: drains the queue one item at a time, then parks until more arrive or
// the app exits. spawned lazily on the first enqueue and joined once in shutdownDownloads.
static void downloadWorker(uint64_t arg)
{
   (void)arg;
   for (;;) {
      QueueItem item;
      lock(&dl.mutex);
      if (dl.cancelAll) { dl.active = 0; unlock(&dl.mutex); break; }
      if (dl.count == 0) { dl.active = 0; unlock(&dl.mutex); sleepMs(100); continue; }
      item = dl.queue[0];
      for (int i = 1; i < dl.count; i++) dl.queue[i - 1] = dl.queue[i];
      dl.count--;
      dl.active = 1;
      dl.percent = 0;
      strCopy(dl.activeTitle, sizeof dl.activeTitle, item.title);
      strCopy(dl.activeId, sizeof dl.activeId, item.videoId);
      unlock(&dl.mutex);

      runOneDownload(&item);
   }
   exitThread();
}

void enqueueDownload(const SearchResult *item)
{
   if (!dl.initialized || !item->videoId[0]) return;

   int spawn = 0, duplicate = 0, full = 0;
   lock(&dl.mutex);
   if (dl.active && strcmp(dl.activeId, item->videoId) == 0) duplicate = 1;
   for (int i = 0; i < dl.count && !duplicate; i++)
      if (strcmp(dl.queue[i].videoId, item->videoId) == 0) duplicate = 1;

   if (!dl.cancelAll && !duplicate) {
      if (dl.count >= MAX_QUEUE) full = 1;
      else {
         strCopy(dl.queue[dl.count].videoId, sizeof dl.queue[dl.count].videoId, item->videoId);
         strCopy(dl.queue[dl.count].title,   sizeof dl.queue[dl.count].title,   item->title);
         dl.count++;
         if (!dl.workerSpawned) { dl.workerSpawned = 1; spawn = 1; }
      }
   }
   unlock(&dl.mutex);

   if (duplicate) logInfo("[dl] %s already queued or downloading, skipped\n", item->videoId);
   else if (full) logWarn("[dl] queue full, %s skipped\n", item->videoId);

   if (spawn && spawnJoinableThread(&dl.worker, downloadWorker, 0, THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB, "yt-dl") != 0) {
      lock(&dl.mutex); dl.workerSpawned = 0; unlock(&dl.mutex);
      logError("[dl] worker spawn failed\n");
   }
}

// ---- overlay ----

// "Downloading "<title>" NN%" with the title truncated so it can't fill the screen; "(+N)" when more are
// queued. drops a dangling UTF-8 byte if the cut lands mid-character.
static void formatOverlay(char *out, int cap)
{
   char clipped[DISPLAY_TITLE_MAX + 4];
   int i = 0;
   for (; i < DISPLAY_TITLE_MAX && dl.activeTitle[i]; i++) clipped[i] = dl.activeTitle[i];
   int truncated = dl.activeTitle[i] != 0;
   if (truncated) while (i > 0 && ((unsigned char)clipped[i - 1] & 0xC0) == 0x80) i--;   // don't split a UTF-8 char
   clipped[i] = 0;

   int queued = dl.count;
   if (queued > 0) snprintf(out, cap, "Downloading \"%s%s\" %d%% (+%d)", clipped, truncated ? "..." : "", dl.percent, queued);
   else            snprintf(out, cap, "Downloading \"%s%s\" %d%%", clipped, truncated ? "..." : "", dl.percent);
}

void updateDownloadOverlay(void)
{
   if (!dl.initialized || !dl.active) return;
   char text[128];
   formatOverlay(text, sizeof text);
   setLabelText(&overlayLabel, text);   // only rasterises on a real change
}

void drawDownloadOverlay(int screenWidth)
{
   if (!dl.initialized || !dl.active || overlayLabel.tt.tex.w == 0) return;
   int badgeW = overlayLabel.tt.tex.w + 12, badgeH = OVERLAY_TEXT_SIZE + 8;
   int x = screenWidth - badgeW - 6, y = 6;
   fillGfxRectangle(x, y, badgeW, badgeH, OVERLAY_BG);
   drawLabelAt(&overlayLabel, x + 6, y + 4);
}

// ---- lifecycle ----

void initDownloads(void)
{
   memset(&dl, 0, sizeof dl);
   createLock(&dl.mutex);
   makeDir(DOWNLOAD_DIR);
   font = openSystemFont(FONT_POP);
   initLabel(&overlayLabel, &font, 0, 0, AUTO, AUTO, OVERLAY_TEXT_SIZE, COLOR_SLATE_100, TEXT_NOWRAP, "");
   dl.initialized = 1;
}

void shutdownDownloads(void)
{
   if (!dl.initialized) return;
   dl.cancelAll = 1;
   if (dl.workerSpawned) joinThread(dl.worker);
   dl.count = 0;
   finishGfx();   // the overlay label may have been sampled last frame
   freeLabel(&overlayLabel);
   closeFont(&font);
   destroyLock(&dl.mutex);
   dl.initialized = 0;
}
