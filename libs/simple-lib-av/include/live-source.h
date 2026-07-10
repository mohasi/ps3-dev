#pragma once

// live-source - a forward-only byte stream over a DASH live "sq/<n>" segment sequence (as YouTube live
// serves). Fetches each segment (<base>sq/<n>) on demand, strips the repeated ftyp/moov init from every
// segment after the first, and concatenates the moof/mdat fragments into one continuous fragmented-mp4
// stream the mp4 demuxer reads exactly as it reads a VOD fragmented file. Total size is unknown; when it
// catches up to the live edge it waits (bounded) for the next segment to appear.

#include <stdint.h>

typedef struct LiveSource LiveSource;

// open the sq window; playback begins a few segments behind edgeSq so there is a small buffer. NULL on failure.
LiveSource *openLiveSource(const char *baseUrl, long startSq, long edgeSq);

int64_t  readLiveSource(LiveSource *source, void *buffer, uint64_t length);   // bytes, 0 = eof/cancelled, -1 err
int      seekLiveSource(LiveSource *source, uint64_t offset);                 // within the buffered window; 0 / -1
uint64_t getLiveSourcePosition(const LiveSource *source);

void cancelLiveSource(LiveSource *source);   // unblock a read waiting on the edge, so teardown can't hang
void closeLiveSource(LiveSource *source);
