#!/bin/sh
# Assemble the OnionOS package: dist/App/ProjectMidas/.
#
#   scripts/port/package_miyoo.sh [build_dir] [sdl2_prefix] [gl_variant]
#
#   build_dir    CMake build directory produced with cmake/miyoomini-armhf.cmake
#                (default: build_miyoo)
#   sdl2_prefix  install prefix of the ARM SDL2 fork (default: build_miyoo_deps)
#   gl_variant   640x480 (default) or 320x240 - which prebuilt libEGL/libGLESv2
#                to ship. Both are SwiftShader, i.e. a CPU rasteriser; the
#                320x240 set is the cheaper one if the renderer turns out to be
#                fill-rate bound (it will be).
#
# dist/App/ProjectMidas/ is both the staging directory and the checked-in home
# of launch.sh/config.json. Copy the finished folder to the SD card as
# /mnt/SDCARD/App/ProjectMidas/.
set -eu

BUILD_DIR=${1:-build_miyoo}
SDL2_PREFIX=${2:-build_miyoo_deps}
GL_VARIANT=${3:-640x480}

REPO_DIR=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$REPO_DIR/dist/App/ProjectMidas"
BIN="$BUILD_DIR/ProjectMidas"

if [ ! -f "$BIN" ]; then
    echo "package_miyoo: no binary at $BIN" >&2
    echo "               build it first (see MIYOO_BUILD_ATTEMPT.md)" >&2
    exit 1
fi

echo ">> staging $OUT"
mkdir -p "$OUT/Data/Logs"

# Strip only if the cross strip matches the binary's architecture - a host
# strip silently mangles an ARM ELF, so test before trusting it.
if command -v arm-linux-gnueabihf-strip >/dev/null 2>&1; then
    cp "$BIN" "$OUT/ProjectMidas"
    arm-linux-gnueabihf-strip "$OUT/ProjectMidas" || echo "   (strip failed, keeping symbols)"
else
    echo "   arm-linux-gnueabihf-strip not on PATH; copying unstripped"
    cp "$BIN" "$OUT/ProjectMidas"
fi
chmod +x "$OUT/ProjectMidas"

# ---- the Miyoo SDL2 fork's runtime -------------------------------------------
# libmi_*.so are NOT shipped: they belong to the firmware and sit in
# /config/lib and /customer/lib, which launch.sh puts on LD_LIBRARY_PATH.
copy_lib() {
    src=$1
    [ -f "$src" ] || { echo "package_miyoo: missing $src" >&2; exit 1; }
    cp "$src" "$OUT/"
}

copy_lib "$SDL2_PREFIX/lib/libSDL2-2.0.so.0"

if [ -f "$SDL2_PREFIX/lib/libEGL.so" ]; then
    copy_lib "$SDL2_PREFIX/lib/libEGL.so"
    copy_lib "$SDL2_PREFIX/lib/libGLESv2.so"
else
    echo "   GL libs not in the install prefix; using sdl2/prebuilt/$GL_VARIANT"
    copy_lib "$REPO_DIR/sdl2/prebuilt/$GL_VARIANT/libEGL.so"
    copy_lib "$REPO_DIR/sdl2/prebuilt/$GL_VARIANT/libGLESv2.so"
fi

# ---- files the engine expects beside the binary ------------------------------
# launch.sh, config.json and this script's output share a directory: dist/App/
# ProjectMidas/ IS the staging area, so those two are already in place and are
# not copied onto themselves.
cp "$REPO_DIR/port/include/icon.png" "$OUT/icon.png"
chmod +x "$OUT/launch.sh"

# CMake's GE_VIDEO POST_BUILD already drops these next to the binary; copy
# again from the source tree so packaging works even from a stale build dir.
cp "$REPO_DIR/tools/gamecontrollerdb.txt" "$OUT/" 2>/dev/null || true
cp "$REPO_DIR/tools/gamecontrollerdefault.txt" "$OUT/" 2>/dev/null || true
cp "$REPO_DIR/tools/midas_ini_help.txt" "$OUT/" 2>/dev/null || true
cp "$REPO_DIR/tools/console_help.txt" "$OUT/" 2>/dev/null || true
mkdir -p "$OUT/Data"
cp "$REPO_DIR/tools/shaders_base.txt" "$OUT/Data/" 2>/dev/null || true
cp "$REPO_DIR/tools/Data_README.txt" "$OUT/Data/" 2>/dev/null || true

# ---- the cart image ----------------------------------------------------------
# Not copied automatically even when one is present: the image is the user's,
# and a 12 MB file in a git-tracked dist/ is an accident waiting to happen.
# Copy it yourself, or let the engine find it in Data/.
if [ -f "$REPO_DIR/rom/007 - GoldenEye (USA).n64" ] && [ "${MIYOO_COPY_ROM:-0}" = "1" ]; then
    cp "$REPO_DIR/rom/007 - GoldenEye (USA).n64" "$OUT/Data/baserom.n64"
fi

echo ">> done: $OUT"
du -sh "$OUT"
find "$OUT" -maxdepth 1 -type f -exec ls -l {} \;
