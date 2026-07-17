#!/usr/bin/env bash
# Cross-builds the trimmed ffmpeg.exe that ships next to cell-stream-server.exe.
# Runs in WSL (Debian 13 here) and produces a static Windows binary via mingw-w64.
#
# WHY THIS EXISTS / THE ONE THING THAT MATTERS:
#   nvenc and amf are gated at RUNTIME on the GPU driver version. ffmpeg's own nvenc header
#   (nv-codec-headers) decides the MINIMUM driver a user needs. A bleeding-edge header (13.1)
#   demands a brand-new driver (~580+); even a recent 2025 driver (576.80 = NVENC API 13.0)
#   falls short, and the encoder silently drops to CPU. So we PIN nv-codec-headers to the
#   OLDEST tag ffmpeg 7.1 still accepts (n12.1.14.0 -> NVENC API 12.1 -> min Windows driver
#   531.61, early 2023). Same reasoning caps AMF at its lowest accepted tag.
#
#   If you ever bump ffmpeg, re-check the two floors:
#     - ffnvcodec:  grep for "ffnvcodec >=" in ffmpeg's configure; use that lower bound.
#     - amf:        grep for "AMF_VERSION_MAJOR" in configure; the hex is MAJ.MIN.RELEASE.BUILD.
#
# ONE-TIME PACKAGES (Debian/Ubuntu):
#   sudo apt install -y gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 git make nasm cmake clang pkg-config
#
# RUN:  bash build-ffmpeg.sh    ->    ~/ff-build/ffmpeg/ffmpeg.exe
# Then copy that over dev/tools/cell-stream-server/ffmpeg.exe.
set -euo pipefail

FFROOT="$HOME/ff-build"
PREFIX="$FFROOT/prefix"
TARGET="x86_64-w64-mingw32"
NVHEADERS_TAG="n12.1.14.0"     # lowest ffmpeg 7.1 accepts -> min Windows NVIDIA driver 531.61 (early 2023)
AMF_TAG="v1.4.34"             # lowest ffmpeg 7.1 accepts (needs release >= 33); AMF runtime is driver-tolerant
LIBVPL_TAG="v2.13.0"
FFMPEG_TAG="n7.1"
JOBS="$(nproc)"

export PATH="$PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

mkdir -p "$FFROOT" "$PREFIX"
cd "$FFROOT"

echo "=== [1/6] mingw cmake toolchain file ==="
cat > "$FFROOT/mingw-w64.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER ${TARGET}-gcc)
set(CMAKE_CXX_COMPILER ${TARGET}-g++)
set(CMAKE_RC_COMPILER ${TARGET}-windres)
set(CMAKE_FIND_ROOT_PATH "/usr/${TARGET};${PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

echo "=== [2/6] nv-codec-headers ${NVHEADERS_TAG} ==="
rm -rf nv-codec-headers
git clone -q https://github.com/FFmpeg/nv-codec-headers.git
git -C nv-codec-headers checkout -q "$NVHEADERS_TAG"
make -s -C nv-codec-headers PREFIX="$PREFIX" install
grep Version "$PREFIX/lib/pkgconfig/ffnvcodec.pc"

echo "=== [3/6] AMF headers ${AMF_TAG} ==="
rm -rf AMF
git clone -q --depth 1 --branch "$AMF_TAG" https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git
mkdir -p "$PREFIX/include/AMF"
cp -r AMF/amf/public/include/* "$PREFIX/include/AMF/"
ls "$PREFIX/include/AMF/core/Version.h"

echo "=== [4/6] x264 (static) ==="
rm -rf x264
git clone -q --depth 1 --branch stable https://code.videolan.org/videolan/x264.git
cd x264
./configure --host="$TARGET" --cross-prefix="${TARGET}-" --prefix="$PREFIX" \
   --enable-static --enable-pic --disable-cli --disable-opencl --bit-depth=8 >/dev/null
make -j"$JOBS" >/dev/null
make install >/dev/null
cd "$FFROOT"

echo "=== [5/6] libvpl ${LIBVPL_TAG} (Intel Quick Sync dispatcher) ==="
rm -rf libvpl
git clone -q --depth 1 --branch "$LIBVPL_TAG" https://github.com/intel/libvpl.git
cd libvpl
cmake -B _build -G "Unix Makefiles" \
   -DCMAKE_TOOLCHAIN_FILE="$FFROOT/mingw-w64.cmake" \
   -DCMAKE_INSTALL_PREFIX="$PREFIX" \
   -DCMAKE_BUILD_TYPE=Release \
   -DBUILD_SHARED_LIBS=OFF \
   -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TOOLS=OFF >/dev/null
cmake --build _build -j"$JOBS" >/dev/null
cmake --install _build >/dev/null
cd "$FFROOT"

echo "=== [6/6] ffmpeg ${FFMPEG_TAG} ==="
rm -rf ffmpeg
git clone -q --depth 1 --branch "$FFMPEG_TAG" https://github.com/FFmpeg/FFmpeg.git ffmpeg
cd ffmpeg
./configure \
   --prefix="$PREFIX" --arch=x86_64 --target-os=mingw32 \
   --cross-prefix="${TARGET}-" --pkg-config=pkg-config --pkg-config-flags=--static \
   --extra-cflags="-I$PREFIX/include" \
   --extra-ldflags="-L$PREFIX/lib -static -static-libgcc -static-libstdc++" \
   --extra-libs="-lstdc++" \
   --disable-everything --disable-autodetect --disable-doc --disable-network \
   --disable-ffplay --disable-ffprobe \
   --enable-gpl --enable-libx264 --enable-libvpl --enable-ffnvcodec --enable-nvenc \
   --enable-amf --enable-cuda-llvm --enable-d3d11va --enable-dxva2 --enable-avdevice \
   --enable-indev=lavfi \
   --enable-encoder=h264_qsv,h264_nvenc,h264_amf,libx264 \
   --enable-decoder=wrapped_avframe \
   --enable-filter=ddagrab,vpp_qsv,scale_qsv,scale_cuda,scale_d3d11,hwmap,hwupload,hwdownload,format,scale,setpts,null,anull,color,testsrc2 \
   --enable-muxer=h264,null \
   --enable-protocol=pipe,file \
   --enable-parser=h264 \
   --enable-bsf=h264_metadata,h264_mp4toannexb
make -j"$JOBS"

echo "=== DONE -> copy this over dev/tools/cell-stream-server/ffmpeg.exe ==="
ls -la "$FFROOT/ffmpeg/ffmpeg.exe"
