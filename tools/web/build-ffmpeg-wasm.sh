#!/bin/sh
# Builds the minimal FFmpeg the game needs, for WebAssembly, into
# third_party/ffmpeg-wasm (git-ignored). Needs Emscripten on PATH (emcc).
#
# The game uses FFmpeg only in src/media/smacker.cpp: it probes in-memory
# Smacker and WAV files, decodes Smacker video, Smacker/Bink audio and PCM,
# and converts with swscale/swresample. Everything else is disabled.
set -eu

VERSION=${FFMPEG_VERSION:-7.1.1}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PREFIX="$ROOT/third_party/ffmpeg-wasm"
WORK="$ROOT/third_party/ffmpeg-wasm-build"
TARBALL="ffmpeg-$VERSION.tar.xz"

command -v emcc >/dev/null || { echo "emcc not found; install Emscripten" >&2; exit 1; }
mkdir -p "$WORK"
cd "$WORK"
if [ ! -d "ffmpeg-$VERSION" ]; then
    [ -f "$TARBALL" ] || curl -fsSLO "https://ffmpeg.org/releases/$TARBALL"
    tar xf "$TARBALL"
fi
cd "ffmpeg-$VERSION"

emconfigure ./configure \
    --prefix="$PREFIX" \
    --target-os=none --arch=x86_32 --enable-cross-compile \
    --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm=emnm \
    --disable-everything --disable-autodetect --disable-programs --disable-doc \
    --disable-asm --disable-x86asm --disable-inline-asm --disable-pthreads \
    --disable-network --disable-debug --disable-runtime-cpudetect \
    --enable-static --disable-shared \
    --enable-avformat --enable-avcodec --enable-avutil \
    --enable-swscale --enable-swresample \
    --enable-demuxer=smacker,wav \
    --enable-decoder=smacker,smackaud,binkaudio_rdft,binkaudio_dct,pcm_s16le,pcm_u8 \
    --extra-cflags="-O3" --extra-ldflags="-O3"
emmake make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
emmake make install
echo "FFmpeg $VERSION for WebAssembly installed in $PREFIX"
