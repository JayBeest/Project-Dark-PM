#!/bin/sh
# Cross-compile the M0 present-path probe for the Miyoo Mini.
#
#   docker run --rm -v "$PWD":/root/workspace/Miyoo_port miyoo-build \
#     sh /root/workspace/Miyoo_port/scripts/port/build_m0.sh
#
# Output: dist/App/ProjectMidas/m0_present (next to the game binary, so the
# launcher can run it with the same LD_LIBRARY_PATH).
#
# Why this is not a CMake target: it is a device probe, not part of the engine.
# It has to link libmi_gfx/libmi_sys from sdl2/mini/lib, which are ARM-only and
# are NOT shipped in the package - on the device they are firmware libraries in
# /config/lib and /customer/lib. A CMake target would have to be excluded from
# every desktop build; a four-line script is cheaper than that.
set -eu

REPO=${REPO:-/root/workspace/Miyoo_port}
CROSS=${CROSS:-/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-}
OUT=${OUT:-$REPO/dist/App/ProjectMidas}

SRC="$REPO/scripts/port/m0_present.c"
INC="$REPO/sdl2/mini/inc"
LIB="$REPO/sdl2/mini/lib"

for f in "$SRC" "$INC/mi_gfx.h" "$LIB/libmi_gfx.so"; do
    [ -e "$f" ] || { echo "build_m0: missing $f" >&2; exit 1; }
done

mkdir -p "$OUT"

echo ">> compiling m0_present"
"${CROSS}gcc" \
    -std=gnu11 -O2 -Wall \
    -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    -I"$INC" \
    -o "$OUT/m0_present" "$SRC" \
    -L"$LIB" \
    -Wl,-rpath,'$ORIGIN' \
    -lmi_gfx -lmi_sys -lmi_common \
    -lpthread -ldl -lrt -lm

"${CROSS}strip" "$OUT/m0_present" 2>/dev/null || true
chmod +x "$OUT/m0_present"

echo ">> built:"
ls -l "$OUT/m0_present"
"${CROSS}readelf" -d "$OUT/m0_present" 2>/dev/null | grep -E "NEEDED|RPATH|RUNPATH" || true
