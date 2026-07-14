#pragma once

// chapters - parse the "0:00 Intro" timestamp lines creators put in a video description.
// YouTube's own rule applies: the list only counts as chapters when the first stamp is 0:00
// and the stamps ascend - anything else is just prose that happens to mention times.

#define MAX_CHAPTERS      32
#define CHAPTER_TITLE_MAX 64

typedef struct {
   float start;                   // seconds into the video
   char  title[CHAPTER_TITLE_MAX];
} Chapter;

typedef struct {
   Chapter chapters[MAX_CHAPTERS];
   int     count;
} ChapterList;

// fills `out` from the description text; returns the chapter count (0 = no valid chapter list).
int parseChapters(const char *description, ChapterList *out);

// fills `out` from YouTube's own chapter list (the /next endpoint) - it covers videos whose chapters
// were set in the studio editor rather than typed into the description. blocking network I/O; run it
// on a worker. returns the chapter count (0 = none or fetch failed).
int fetchChapters(const char *videoId, ChapterList *out);
