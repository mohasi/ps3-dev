// sponsorblock - fetch + parse skip segments from the SponsorBlock API (see sponsorblock.h).
//
// One blocking HTTPS GET to /api/skipSegments returns a small json array of segments, each with a
// category, an actionType and a [start,end] time pair. We keep only the "skip" actions in the
// categories we recognise; the player auto-skips them and marks them on the seek bar.
//
// The request goes through the http module: with the modern (BearSSL) transport bound, it reaches
// sponsor.ajay.app, which is behind Cloudflare and serves an ECDSA certificate the firmware TLS can't
// negotiate (it is RSA-only). Under the system (cellHttp) transport this host would be unreachable.

#include "sponsorblock.h"
#include "http.h"
#include "dbg.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SB_RESP_CAP  (64 * 1024)   // ample: a video rarely has more than a few dozen short segments

// api category name <-> enum, human label, and the canonical SponsorBlock colour (ARGB, opaque).
static const char *CATEGORY_API_NAMES[SPONSOR_CATEGORY_COUNT] = {
   "sponsor", "selfpromo", "interaction", "intro", "outro", "music_offtopic", "filler"
};
static const char *CATEGORY_LABELS[SPONSOR_CATEGORY_COUNT] = {
   "Sponsor", "Self-promo", "Interaction", "Intro", "Outro", "Off-topic music", "Filler"
};
static const unsigned CATEGORY_COLORS[SPONSOR_CATEGORY_COUNT] = {
   0xFF00D400,   // sponsor        - green
   0xFFFFFF00,   // selfpromo      - yellow
   0xFFCC00FF,   // interaction    - magenta
   0xFF00FFFF,   // intro          - cyan
   0xFF0202ED,   // outro          - blue
   0xFFFF9900,   // music_offtopic - orange
   0xFF7300FF,   // filler         - purple
};

unsigned    getSponsorCategoryColor(SponsorCategory category) { return CATEGORY_COLORS[category]; }
const char *getSponsorCategoryName(SponsorCategory category)  { return CATEGORY_LABELS[category]; }

// the category enum for a "category":"..." within [from,end), or -1 if it's one we don't handle.
static int readCategory(const char *from, const char *end)
{
   const char *p = strstr(from, "\"category\":\"");
   if (!p || p >= end) return -1;
   p += strlen("\"category\":\"");

   char name[32];
   int i = 0;
   while (p < end && *p && *p != '"' && i < (int)sizeof name - 1) name[i++] = *p++;
   name[i] = 0;
   for (int category = 0; category < SPONSOR_CATEGORY_COUNT; category++)
      if (strcmp(name, CATEGORY_API_NAMES[category]) == 0) return category;
   return -1;
}

// a segment with no actionType defaults to skip; "mute"/"full"/"poi" segments are not auto-skipped.
static int isSkipAction(const char *from, const char *end)
{
   const char *p = strstr(from, "\"actionType\":\"");
   if (!p || p >= end) return 1;
   p += strlen("\"actionType\":\"");
   return strncmp(p, "skip", 4) == 0;
}

// read the "segment":[start,end] float pair within [from,end). 1 if both were parsed.
static int readSegment(const char *from, const char *end, float *start, float *stop)
{
   const char *p = strstr(from, "\"segment\":[");
   if (!p || p >= end) return 0;
   p += strlen("\"segment\":[");
   if (p >= end) return 0;   // the key can start within its own length of end; keeps end-p positive for memchr
   *start = (float)atof(p);
   const char *comma = memchr(p, ',', end - p);
   if (!comma) return 0;
   *stop = (float)atof(comma + 1);
   return 1;
}

// each response object leads with "category", so anchoring on it bounds one segment object.
static void parseSegments(const char *resp, const char *end, SponsorSegments *out)
{
   const char *scan = resp;
   while (out->count < MAX_SPONSOR_SEGMENTS) {
      const char *object = strstr(scan, "\"category\":\"");
      if (!object || object >= end) break;
      const char *next = strstr(object + 1, "\"category\":\"");
      const char *objectEnd = (next && next < end) ? next : end;

      int category = readCategory(object, objectEnd);
      float start, stop;
      if (category >= 0 && isSkipAction(object, objectEnd) && readSegment(object, objectEnd, &start, &stop) && stop > start) {
         SponsorSegment *segment = &out->segments[out->count++];
         segment->start = start; segment->end = stop; segment->category = (SponsorCategory)category; segment->skipped = 0;
      }
      scan = objectEnd;
   }
}

int fetchSponsorSegments(const char *videoId, SponsorSegments *out)
{
   memset(out, 0, sizeof *out);

   char url[512];
   snprintf(url, sizeof url,
            "https://sponsor.ajay.app/api/skipSegments?videoID=%s"
            "&category=sponsor&category=selfpromo&category=interaction&category=intro"
            "&category=outro&category=music_offtopic&category=filler",
            videoId);

   char *resp = malloc(SB_RESP_CAP);
   if (!resp) return -1;

   int respLen = 0, status = 0;
   int rc = getHttp(url, resp, SB_RESP_CAP, &respLen, &status);
   if (rc < 0)        { logWarn("[sb] fetch failed rc=%d\n", rc); free(resp); return rc; }
   if (status == 404) { free(resp); return 0; }   // 404 = no segments submitted for this video (normal)
   if (status != 200) { logWarn("[sb] unexpected status=%d\n", status); free(resp); return 0; }

   parseSegments(resp, resp + respLen, out);
   logInfo("[sb] %d skip segment(s) for %s\n", out->count, videoId);
   free(resp);
   return 0;
}
