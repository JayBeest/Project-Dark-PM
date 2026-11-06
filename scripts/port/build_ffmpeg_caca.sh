#!/bin/sh
# Build FFmpeg with libcaca (the `-f caca` output device), into .deps/ffmpeg-caca.
#
# Homebrew's ffmpeg is built without --enable-libcaca, so `ffmpeg -f caca` fails
# with "Requested output format 'caca' is not known". This produces a private
# copy that has it, leaving /opt/homebrew untouched.
#
# The rest of the build is kept close to Homebrew's so the two behave alike;
# docs and the Qt/GTK front-ends are dropped because only the ffmpeg CLI matters.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC="$ROOT/.deps/src/ffmpeg-9.0.2"
PREFIX="$ROOT/.deps/ffmpeg-caca"

test -d "$SRC" || { echo "missing $SRC (extract the tarball there first)" >&2; exit 1; }

cd "$SRC"

./configure \
    --prefix="$PREFIX" \
    --enable-shared --disable-static \
    --enable-libcaca \
    --enable-gpl \
    --disable-doc \
    --disable-debug \
    --disable-ffplay \
    --disable-ffprobe \
    --enable-pthreads \
    --enable-videotoolbox \
    --enable-audiotoolbox \
    --cc=clang

make -j"$(sysctl -n hw.ncpu)"
make install

"$PREFIX/bin/ffmpeg" -hide_banner -devices | grep -i caca
echo "OK: $PREFIX/bin/ffmpeg has the caca device"
