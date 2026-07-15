# simple-lib-av

Audio and video playback for PS3 homebrew apps (Sony SDK 4.75). This is where all media code
lives: the audio mixer and the full H.264/AAC video stack. It uses the PS3's built-in hardware
decoders, so playback is real-time without pegging the main CPU.

## What it can play

**Video**
- H.264 video, 8-bit only, decoded on the PS3's hardware video decoder (cellVdec, spread across 4 SPUs).
- AAC audio, decoded on the hardware audio decoder (cellAdec). Video with no audio plays silent.
- Containers: MKV (Matroska) and MP4. Both plain MP4 and *fragmented* MP4 (the streaming format
  YouTube serves) are handled.
- Sources: a local file, an `http(s)://` URL (streamed as it downloads), or a YouTube-style live
  segment stream. The demuxers don't care where the bytes come from.
- What it will NOT play: HEVC, 10-bit video, Opus/E-AC3/DTS audio. The probe below reports these
  up front with a reason to show the user.

**Audio (standalone mixer)**
- WAV, OGG Vorbis, MP3 and FLAC.
- Either streamed from disk (decoded a little at a time, so an hour-long file needs almost no memory)
  or loaded fully into memory for short sound effects.
- Per-stream and master volume, volume fades, seeking, looping, and a rolling waveform for on-screen
  visualisers. Track titles are pulled from ID3 / Vorbis-comment tags.

## Public headers

Include only what you need:

- `video-player.h` - the playback engine (open, get frames, pause, seek, position/duration).
- `video-probe.h` - "will this file play?" check, with a user-facing reason when it won't.
- `audio.h` - the standalone audio mixer.

Everything else (`demux*.h`, `decode-*.h`, `h264.h`, `ebml.h`, `mp4.h`, `video-source.h`,
`live-source.h`, `mux-mkv.h`) is internal plumbing the two entry points sit on top of.

## Driving video playback

The player decodes ahead on its own background thread; the UI just pulls the frame that's due each
time it draws.

1. **Probe first (optional but recommended).** `probeVideo(path, &out)` fills in the codecs, size
   and a verdict (`VIDEO_PLAYABLE` / `VIDEO_UNSUPPORTED` / `VIDEO_UNREADABLE`). On a reject, `out.reason`
   is a ready-to-show message.
2. **Open.** `createVideoPlayer(path, alloc, free)` returns a `VideoPlayer *`, or NULL if the file
   can't be opened. The two `alloc`/`free` hooks let frames land in RSX-visible memory so they draw
   with no copy; pass NULL for plain heap buffers. Use `createVideoPlayerSplit(videoUrl, audioUrl, ...)`
   when video and audio are separate streams (the adaptive/DASH case).
3. **Each render frame, call `getVideoFrame(player, &width, &height)`.** It returns the YUV 4:2:0
   frame that should be shown right now (paced against the clock), or NULL if none is ready yet.
   The engine skips ahead if the UI fell behind and holds if it's early, so playback runs at the
   file's true frame rate. The returned pointer stays valid until the second-next call.
4. **Control it:** `setVideoPaused`, `seekVideoPlayer(seconds)` (jumps to the nearest keyframe at or
   before the target), `seekVideoPlayerPast(seconds)` (lands on the next keyframe at or after - use
   this to skip a whole span such as a sponsor segment cleanly). Seeks are asynchronous; the current
   frame stays on screen until the new position is ready, so there's no black flash.
5. **Query:** `getVideoPositionSeconds`, `getVideoDurationSeconds`, `isVideoPaused`, `isVideoEnded`,
   `getAudioTrackInfo`.
6. **Close** with `destroyVideoPlayer`.

Sync: while an AAC track is playing, the audio drives the clock (frames are timed against how much
audio has actually reached the speakers). With no audio track, it falls back to wall-clock time.

## Drawing the frames

`getVideoFrame` returns planar YUV (Y, then U, then V) - not RGB on purpose. Colour conversion runs
on the RSX (GPU) at draw time via `simple-lib-app`'s `drawGfxYuvFrame` (a BT.709 shader). Asking
cellVdec for RGB instead would run the conversion on the same SPUs that do the decoding and cost real
throughput. Allocate the frame buffers with gfx's `allocGfxVideoBuffer` (RSX-mapped memory) and pass
those as the alloc/free hooks for true zero-copy.

## Hardware limits

- **Frame rate.** 1080p decodes at roughly 35 fps on the 4 SPUs. 24 fps films play with headroom;
  1080p50 broadcasts play best-effort and may drop frames.
- **Reference frames (the black-screen trap).** The PS3's decoder can only hold about 4 reference
  frames at 1080p. The library rewrites a stream's `max_num_ref_frames` down to fit where it can. Some
  encodes (e.g. 5-reference-frame anime Blu-ray rips) need more than fit and can't be patched down -
  the probe flags these (`refFramesExceedDpb`) and rejects them, because on real hardware they decode
  to an all-black picture with no error. The same files play fine in the RPCS3 emulator, which has no
  such cap - so "works in RPCS3" is not proof it'll work on the console.
- **Coded size must be exact.** cellVdec must be told the stream's *coded* size (whole 16x16 blocks,
  which some encoders pad further - e.g. 1280x720 coded as 1280x736). Too small and it decodes nothing
  into a black buffer; too large and it locks the console. The demuxers read the real coded size from
  the stream, so callers don't handle this - but a network sender feeding raw frames must transmit it.

## Building against it

1. Build `simple-lib-av`, which produces `libsimple-lib-av.a` in `bin/Release/`.
2. In your app's vcxproj:
   - Add `$(SolutionDir)libs\simple-lib-av\include` to the include directories.
   - Link in this order (core last): `-lsimple-lib-app -lsimple-lib-av -lsimple-lib-core`, plus
     `-lvdec_stub -ladec_stub` for video playback.
3. Depends on `simple-lib-core`; apps that draw video also need `simple-lib-app` (for gfx).

The audio decoders are vendored single-header libraries pulled straight into `audio.c` (not compiled
separately): `vorbis.h` (stb_vorbis) and `mp3.h` / `flac.h` / `wav.h` (the dr_libs).
