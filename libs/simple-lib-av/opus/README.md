# opus, vendored

Upstream libopus 1.5.2 from https://downloads.xiph.org/releases/opus/, decoder only. The licence
is in COPYING and the files are exactly as upstream wrote them: nothing here is patched, and
nothing here is compiled with warnings turned off.

It is here because every WebRTC connection carries opus, the console has no decoder for it, and
the Xbox machine will not agree to anything else. Offering it PCMU alongside opus, with PCMU
first, was answered with opus only.

Reach it through `decode-opus.h`, not directly. Nothing outside `src/decode-opus.c` includes
anything from this folder.

## What was taken

`celt/`, `silk/`, `src/` and `include/`, then everything that only encodes was deleted:

- `silk/fixed/` in its entirety. It is the fixed-point encoder and contains nothing that decodes.
- `celt/celt_encoder.c`, and the thirty silk files that only encode (`enc_API.c`, `NSQ*.c`,
  `NLSF_encode.c`, `VAD.c`, the `control_*` and `stereo_*_encode` files, and so on).
- `src/opus_encoder.c` and the analysis and multi-stream files that go with it. What is left of
  `src/` is `opus.c` and `opus_decoder.c`.
- `src/extensions.c`, which the decoder declares but never calls.
- `dnn/` (18 MB of neural models), `tests/` and `doc/`, none of which a decoder needs.
- Every per-architecture folder. All of them are x86, ARM or MIPS, and this is PowerPC.

Five files were deleted as encoder-only and put back because the linker showed the decoder needs
them: `pitch_est_tables.c`, `LPC_fit.c`, `sort.c`, `sum_sqr_shift.c`, `LPC_analysis_filter.c`.
`celt/entenc.c` looks like encoder code and is not removable either: the files that hold both
halves of the range coder refer to it.

The remaining set is what the app links against, proved by the link succeeding with nothing
undefined. 7.8 MB of tarball became 1.7 MB of source.

## Updating it

Take the same folders from the new release, delete the same files, and build. The linker names
anything the decoder needs that was cut. `config.h` is written by hand here in place of the build
system upstream uses: it selects fixed point, turns off the floating point API, and says the
toolchain has `lrint` and `lrintf`, which it does.
