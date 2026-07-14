// subtitles - fetch + parse one YouTube timed-text track (see subtitles.h).
//
// The track url returns the classic "srv1" XML: <text start="9.53" dur="5.07">line</text> per cue.
// We request that format explicitly (&fmt=srv1) so the layout is deterministic, decode the XML
// entities (auto captions are often double-escaped, hence the repeated pass), and keep one flat
// time-ordered cue array the player scans forward through during playback.

#include "subtitles.h"
#include "http.h"
#include "text-sanitize.h"      // decodeXmlEntities / stripMarkupTags / removeInvisibleMarks / flattenWhitespace
#include "string-utilities.h"   // strCopy, truncateUtf8
#include "dbg.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SUBS_RESP_CAP (1024 * 1024)   // hardware-observed: a multi-hour video's track passed 512 KB

// parse one <text start=".." dur="..">content</text> element at `element`; returns the scan position
// after it, or NULL when no further element exists before `end`.
static const char *parseCue(const char *element, const char *end, SubtitleTrack *out)
{
   const char *tag = strstr(element, "<text ");
   if (!tag || tag >= end) return NULL;

   float start = 0, dur = 0;
   const char *startAttr = strstr(tag, "start=\"");
   const char *durAttr   = strstr(tag, "dur=\"");
   const char *open      = strchr(tag, '>');
   const char *close     = open ? strstr(open, "</text>") : NULL;
   if (!open || !close || close >= end) return NULL;
   if (startAttr && startAttr < open) start = (float)atof(startAttr + 7);
   if (durAttr && durAttr < open)     dur   = (float)atof(durAttr + 5);
   if (dur <= 0.0f) dur = 3.0f;   // rare missing duration: show the line briefly rather than dropping it

   // clean in a roomy scratch buffer FIRST, truncate LAST: styled tracks wrap a short line in
   // hundreds of bytes of tags, and truncating the raw text mid-tag leaves tag fragments visible.
   // (the caller's loop bounds out->count, so no cap check is needed here.)
   char raw[1024];
   int length = (int)(close - open - 1);
   if (length > (int)sizeof raw - 1) length = (int)sizeof raw - 1;
   memcpy(raw, open + 1, length);
   raw[length] = 0;
   for (int pass = 0; pass < 4 && decodeXmlEntities(raw); pass++) {}   // repeat: styled tracks double-escape
   stripMarkupTags(raw);
   removeInvisibleMarks(raw);
   flattenWhitespace(raw);
   truncateUtf8(raw, SUBTITLE_TEXT_MAX - 1);

   if (raw[0]) {
      // word-highlight ("karaoke") tracks repeat one line as many short cues; merge the repeats so
      // the line neither flickers nor burns through the cue cap.
      SubtitleCue *previous = out->count ? &out->cues[out->count - 1] : NULL;
      if (previous && strcmp(previous->text, raw) == 0 && start <= previous->end + 0.5f) {
         if (start + dur > previous->end) previous->end = start + dur;
      } else {
         SubtitleCue *cue = &out->cues[out->count++];
         strCopy(cue->text, sizeof cue->text, raw);
         cue->start = start;
         cue->end   = start + dur;
      }
   }
   return close + 7;
}

int fetchSubtitles(const char *url, SubtitleTrack *out)
{
   memset(out, 0, sizeof *out);

   // the track url usually carries its own fmt=srv3 (a layout we don't parse); fmt isn't among the
   // signature-protected params, so rewrite it to srv1 rather than appending a duplicate.
   char fullUrl[2200];
   const char *fmt = strstr(url, "&fmt=");
   if (fmt) {
      const char *afterValue = strchr(fmt + 5, '&');
      snprintf(fullUrl, sizeof fullUrl, "%.*s&fmt=srv1%s", (int)(fmt - url), url, afterValue ? afterValue : "");
   } else
      snprintf(fullUrl, sizeof fullUrl, "%s&fmt=srv1", url);

   char *resp = malloc(SUBS_RESP_CAP);
   if (!resp) return -1;

   int respLen = 0, status = 0;
   int rc = getHttp(fullUrl, resp, SUBS_RESP_CAP, &respLen, &status);
   if (rc < 0)        { logWarn("[subs] fetch failed rc=%d\n", rc); free(resp); return rc; }
   if (status != 200) { logWarn("[subs] unexpected status=%d\n", status); free(resp); return -status; }

   const char *end = resp + respLen;
   const char *scan = resp;
   while (scan && out->count < MAX_SUBTITLE_CUES) scan = parseCue(scan, end, out);
   logInfo("[subs] %d cue(s) parsed (%d bytes)\n", out->count, respLen);
   if (respLen >= SUBS_RESP_CAP - 1 || out->count == MAX_SUBTITLE_CUES)
      logWarn("[subs] track truncated at a cap, later lines will be missing\n");

   free(resp);
   return 0;
}

const char *getSubtitleCueText(const SubtitleTrack *track, float seconds, int *scanFrom)
{
   if (*scanFrom >= track->count || seconds < track->cues[*scanFrom].start) *scanFrom = 0;   // sought backwards

   // auto captions "roll up": cue windows overlap the next line's, so among the cues covering this
   // moment always show the LATEST-started one - returning the first match displays the previous
   // sentence for its whole overlap and reads as lagging subtitles.
   const char *text = "";
   for (int i = *scanFrom; i < track->count; i++) {
      if (track->cues[i].start > seconds) break;
      *scanFrom = i;
      if (seconds < track->cues[i].end) text = track->cues[i].text;
   }
   return text;
}
