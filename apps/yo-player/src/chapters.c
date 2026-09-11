// chapters - parse description timestamp lines into a chapter list, or fetch YouTube's own
// chapter list from the /next endpoint (see chapters.h).

#include "chapters.h"
#include "string-utilities.h"   // truncateUtf8
#include "text-sanitize.h"      // flattenWhitespace
#include "json.h"               // decodeJsonString
#include "http.h"
#include "dbg.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// parse an M:SS or H:MM:SS token at `text`; returns seconds, or -1 when no timestamp starts here.
// *consumed gets the token length so the caller can take the title from just after it.
static int parseTimestamp(const char *text, int *consumed)
{
   int parts[3], partCount = 0, i = 0;
   while (partCount < 3) {
      int value = 0, digits = 0;
      while (text[i] >= '0' && text[i] <= '9' && digits < 3) { value = value * 10 + (text[i] - '0'); i++; digits++; }
      if (digits == 0) return -1;
      parts[partCount++] = value;
      if (text[i] != ':') break;
      i++;
   }
   if (partCount < 2 || parts[partCount - 1] >= 60 || (partCount == 3 && parts[1] >= 60)) return -1;
   *consumed = i;
   return partCount == 3 ? parts[0] * 3600 + parts[1] * 60 + parts[2] : parts[0] * 60 + parts[1];
}

static int isTitleSeparator(char c)
{
   return c == ' ' || c == '\t' || c == '-' || c == ':' || c == '|' || c == '.' || c == ',' ||
          c == '[' || c == ']' || c == '(' || c == ')';
}

// copy [start,end) into a chapter title, trimming separator punctuation off both ends.
static void copyTitle(char *title, const char *start, const char *end)
{
   while (start < end && isTitleSeparator(*start)) start++;
   while (end > start && isTitleSeparator(end[-1])) end--;
   int length = (int)(end - start);
   if (length > CHAPTER_TITLE_MAX - 1) length = CHAPTER_TITLE_MAX - 1;
   memcpy(title, start, length);
   title[length] = 0;
   truncateUtf8(title, CHAPTER_TITLE_MAX - 1);   // a clamped copy may have cut mid-character
}

// find one timestamp in the line and take the rest of the line as the title ("0:00 Intro");
// when nothing follows the stamp, take what precedes it instead ("Intro - 0:00").
static void parseChapterLine(const char *line, int lineLength, ChapterList *out)
{
   for (int i = 0; i < lineLength;) {
      if (line[i] < '0' || line[i] > '9') { i++; continue; }
      int consumed, seconds = parseTimestamp(line + i, &consumed);
      if (seconds >= 0 && (i + consumed >= lineLength || line[i + consumed] != ':')) {
         Chapter *chapter = &out->chapters[out->count];
         chapter->start = (float)seconds;
         copyTitle(chapter->title, line + i + consumed, line + lineLength);
         if (!chapter->title[0]) copyTitle(chapter->title, line, line + i);
         if (chapter->title[0]) out->count++;
         return;
      }
      while (i < lineLength && ((line[i] >= '0' && line[i] <= '9') || line[i] == ':')) i++;
   }
}

int parseChapters(const char *description, ChapterList *out)
{
   out->count = 0;

   // one candidate chapter per line
   const char *line = description;
   while (line && *line && out->count < MAX_CHAPTERS) {
      const char *lineEnd = strchr(line, '\n');
      int lineLength = lineEnd ? (int)(lineEnd - line) : (int)strlen(line);
      parseChapterLine(line, lineLength, out);
      line = lineEnd ? lineEnd + 1 : NULL;
   }

   // validity: at least two chapters, starting at 0:00, strictly ascending
   int valid = out->count >= 2 && out->chapters[0].start == 0.0f;
   for (int i = 1; valid && i < out->count; i++)
      if (out->chapters[i].start <= out->chapters[i - 1].start) valid = 0;
   if (!valid) out->count = 0;
   return out->count;
}

#define NEXT_URL      "https://www.youtube.com/youtubei/v1/next?prettyPrint=false"
#define NEXT_RESP_CAP (1536 * 1024)   // /next responses run ~500 KB+
static const char *NEXT_BODY_FMT =
   "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"2.20240726.00.00\"}},\"videoId\":\"%s\"}";

int fetchChapters(const char *videoId, ChapterList *out)
{
   out->count = 0;

   char body[256];
   int bodyLength = snprintf(body, sizeof body, NEXT_BODY_FMT, videoId);
   char *resp = malloc(NEXT_RESP_CAP);
   if (!resp) return 0;

   HttpHeader headers[] = { { "Content-Type", "application/json" } };
   int respLen = 0, status = 0;
   int rc = fetchHttp("POST", NEXT_URL, headers, 1, body, bodyLength, resp, NEXT_RESP_CAP - 1, &respLen, &status);
   if (rc != 0 || status != 200) { logWarn("[yt] chapter fetch failed rc=%d status=%d\n", rc, status); free(resp); return 0; }
   resp[respLen] = 0;

   // scan the "chapterRenderer" entries: {"title":{"simpleText":"..."},"timeRangeStartMillis":N}.
   // the list appears once, in order, so a start that doesn't ascend is a repeated list (or noise)
   // and ends the scan.
   const char *scan = resp;
   while (out->count < MAX_CHAPTERS && (scan = strstr(scan, "\"chapterRenderer\":{"))) {
      scan += 19;
      const char *title  = strstr(scan, "\"simpleText\":\"");
      const char *millis = strstr(scan, "\"timeRangeStartMillis\":");
      if (!title || !millis) break;
      float start = (float)(atof(millis + 23) / 1000.0);
      if (out->count && start <= out->chapters[out->count - 1].start) break;
      Chapter *chapter = &out->chapters[out->count];
      chapter->start = start;
      const char *titleText = title + 14;   // past the "simpleText":" it was found by
      decodeJsonString(titleText, (int)(resp + respLen - titleText), chapter->title, CHAPTER_TITLE_MAX);
      flattenWhitespace(chapter->title);   // a newline in a title reads fine as a space
      if (chapter->title[0]) out->count++;
   }

   logInfo("[yt] chapters fetched: %d (%d bytes)\n", out->count, respLen);
   free(resp);
   return out->count;
}
