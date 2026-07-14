#pragma once

// subtitles - fetch + parse one YouTube timed-text track into timestamped cues.
//
// The play screen picks a CaptionTrack (extractor.h), calls fetchSubtitles on a worker thread,
// then looks up the cue for the current playback position each frame with getSubtitleCueText.

#define MAX_SUBTITLE_CUES 4096
#define SUBTITLE_TEXT_MAX 184

typedef struct {
   float start, end;              // seconds
   char  text[SUBTITLE_TEXT_MAX];
} SubtitleCue;

// one whole parsed track (~800 KB); heap-allocate it, never on the stack.
typedef struct {
   int         count;
   SubtitleCue cues[MAX_SUBTITLE_CUES];
} SubtitleTrack;

// blocking GET + parse of a caption track url. 0 ok (count may still be 0), negative on error.
int fetchSubtitles(const char *url, SubtitleTrack *out);

// the cue text covering `seconds`, or "" if none. *scanFrom caches the search position across
// calls (cues are time-ordered); reset it to 0 after a seek or track switch.
const char *getSubtitleCueText(const SubtitleTrack *track, float seconds, int *scanFrom);
