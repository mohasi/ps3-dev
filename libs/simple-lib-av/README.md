# simple-lib-av

Audio and video playback library for PS3 homebrew apps. Sony official SDK 4.75. Everything media
lives here: the audio mixer (moved out of `simple-lib-app`) and the H.264/AAC video stack.

## Features

- **audio** - multi-stream mixer playing WAV, OGG Vorbis, MP3 and FLAC. Streamed playback (WAV read
  from disk via dr_wav callbacks; OGG/MP3/FLAC decoded on demand from their compressed bytes) or
  fully memory-loaded for short SFX. Per-stream and master volume, seeking, a rolling waveform
  envelope for visualisers, track-title (ID3 / Vorbis comment) extraction, and an external PCM feed
  the video player pushes decoded audio through (its consumed-frames counter is the A/V clock)
- **video-player** - the playback engine: a decode thread demuxes + decodes ahead into a small ring
  of YUV frame slots, the UI pulls the frame that is due each render (`getVideoFrame`), an audio
  thread feeds decoded AAC into the mixer. Audio-master A/V sync (wall clock when there is no audio
  track), pause, and asynchronous seeking with both pipelines flushed per the SDK protocol
- **video-probe** - fast header parse (MKV + MP4) answering "will this play?" with a user-facing
  reason when it won't (HEVC, 10-bit, unsupported container/codec)
- **demux** - container-independent demuxer facade: sniffs MKV vs MP4 and dispatches; both produce
  the same Annex-B video access units and queued AAC audio frames
- **demux-mkv** - Matroska demuxer: track metadata, avcC, cue index (follows chained SeekHeads; falls
  back to lazily scanning cluster headers when a file has no Cues), video blocks to Annex-B access
  units, audio blocks unlaced (Xiph/EBML/fixed) into a lock-free queue
- **demux-mp4** - MP4 (ISOBMFF) demuxer: flattens the moov sample tables into in-memory per-track
  sample arrays (offset, size, pts, keyframe), so reads and keyframe seeks are array walks. Also
  handles fragmented MP4 (moof/traf/trun), streamed fragment-by-fragment - the adaptive format
  yo-player pulls from YouTube
- **decode-h264** - cellVdec wrapper (4 SPUs): non-blocking feed, YUV 4:2:0 planar output straight
  into RSX-mapped buffers, seek flush per the SDK EndSeq/SEQDONE/StartSeq protocol
- **decode-aac** - cellAdec M4AAC wrapper (1 SPU): raw MKV AAC frames get an ADTS header, output is
  interleaved stereo float32 for the mixer feed
- **h264** - avcC parsing, AVCC-to-Annex-B conversion, and the SPS `max_num_ref_frames` bit-patch
  that lets 5-ref 1080p encodes fit the hardware's 4-frame DPB cap
- **ebml / mp4 / video-source** - shared EBML reader, shared ISOBMFF box reader, and a buffered
  seekable source under everything that reads from either a local file or an `http(s)://` URL
  (via `simple-lib-core`'s http streaming), so the demuxers are agnostic to where the media lives

## Usage

1. Build `simple-lib-av` - produces `libsimple-lib-av.a` in `bin/Release/`.
2. In your app's vcxproj:
   - Add `$(SolutionDir)libs\simple-lib-av\include` to include directories.
   - Link order matters: `-lsimple-lib-app -lsimple-lib-av -lsimple-lib-core` (core last), plus
     `-lvdec_stub -ladec_stub` for video playback.
3. `#include "audio.h"` / `#include "video-player.h"` / `#include "video-probe.h"` as needed.

## Notes

- Video frames are drawn zero-copy: the player decodes into buffers the app allocated with gfx's
  `allocGfxVideoBuffer` (RSX-mapped main memory) and the RSX samples the YUV planes directly
  (`drawGfxYuvFrame`, BT.709 fragment shader). Never ask cellVdec for RGB - the conversion runs on
  the same SPUs that decode.
- Hardware ceiling: 1080p is decodable up to roughly 35 fps (4 SPUs). 24 fps content plays with
  plenty of headroom; 1080p50 broadcasts play best-effort with dropped frames.
- The audio decoders are vendored single-header libraries `#include`d by `audio.c` (not compiled
  separately): `vorbis.h` (stb_vorbis) and `mp3.h`/`flac.h`/`wav.h` (the dr_libs).
- Depends on `simple-lib-core`; apps that draw video also need `simple-lib-app` (gfx).
