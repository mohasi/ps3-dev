#pragma once

// stream-select - picks which of a resolved video's formats to use. Shared by playback and download so a
// downloaded file is the exact stream the player would show (same PS3-decodable H.264 + AAC pair).

#include "extractor.h"   // StreamInfo / StreamFormat

// best decodable H.264/mp4 video stream: highest resolution the PS3 decoder can keep up with, and at a
// given resolution the higher frame rate. favours the adaptive (video-only) streams over the 360p muxed
// one. NULL if none is usable.
const StreamFormat *pickBestVideo(const StreamInfo *info);

// the AAC/mp4 audio-only stream to pair with a video-only pick, preferring itag 140. NULL if none.
const StreamFormat *pickBestAudio(const StreamInfo *info);
