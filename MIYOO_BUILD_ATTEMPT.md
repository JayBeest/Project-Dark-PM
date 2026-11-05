# Miyoo Mini build attempt — Project Midas

**Status: the cross-compile succeeds and the package assembles. It has never
been run on a Miyoo.**

A 32-bit ARM binary and an OnionOS `App/ProjectMidas/` folder are produced by
the scripts in this repo. Every step is scripted and reproducible; no manual
package installs are required beyond the two Docker images.

The remaining unknown is not the build, it is the renderer: see
[§7](#7-the-real-problem-the-renderer).

---

## 1. What was proven, and what was not

Proven on this machine:

- The ARM cross-compile links to completion: `ProjectMidas`, ELF 32-bit LSB
  executable, ARM, EABI5, hard-float, interpreter `/lib/ld-linux-armhf.so.3`.
- Its `DT_NEEDED` set is exactly what it should be, and `RPATH` is `$ORIGIN`
  with no build-machine path left in it:
  ```
  NEEDED  libSDL2-2.0.so.0
  NEEDED  libEGL.so
  NEEDED  libGLESv2.so
  NEEDED  libstdc++.so.6
  NEEDED  libm.so.6
  NEEDED  libgcc_s.so.1
  NEEDED  libc.so.6
  RPATH   [$ORIGIN]
  ```
- The SDL2 fork builds for ARM with both Miyoo drivers compiled in
  (`strings libSDL2-2.0.so.0` → `Mini`, `Miyoo Mini`).
- The package assembles: `scripts/port/package_miyoo.sh` → 43 MB in
  `dist/App/ProjectMidas/`.
- The macOS build is unregressed by the portability fixes: `ctest` →
  **100% tests passed, 14/14**.

Not proven — and this is the whole of the remaining risk:

- **Nothing has executed.** No boot, no frame, no audio, no input. Not on
  hardware, and not under QEMU either.
- Float ABI compatibility between the union toolchain, the SDL2 fork and the
  prebuilt SwiftShader libraries is asserted by flags, not observed at runtime.
- glibc compatibility between the toolchain sysroot (glibc 2.28-ish, buster)
  and the OnionOS firmware is unverified.
- Unaligned-access faults (`SIGBUS`) from decompiled code casting packed N64
  structs are a real ARM hazard and cannot be predicted statically.

---

## 2. Three code fixes the build required

All three are upstream-portable — they do not special-case the Miyoo.

**`port/fast3d/gfx_cc.h` — C++20 spaceship operator.** The toolchain is GCC
8.3.0 (`arm-linux-gnueabihf-gcc (Linaro GCC 8.2-2018.08~dev) 8.3.0`).
Three-way comparison landed in GCC 10, and GCC 8 has no `<compare>` header at
all, so `auto operator<=>(const ColorCombinerKey&) const = default;` could not
compile. Replaced with the same lexicographic ordering written out;
`ColorCombinerKey` is two `uint64_t`, so the default had been exactly
member-order. `CMAKE_CXX_STANDARD` drops 20 → 17.

**`port/fast3d/gfx_pc.h` — C++20 defaulted `operator==`.** Same cause.
`TextureCacheKey`'s identity is 11 members (address, both palette addresses,
format, size, palette index, line size, size, palette sum, both wrap masks);
the operator is now written out over all of them, because dropping a term is
exactly how the stale-texture bugs in the comments above it come back. `!=` is
also written out: C++20 rewrites `a != b` to `!(a == b)`, and that rewrite does
not exist at C++17.

**`port/src/input_pc.c` — `SDL_HINT_DIRECTINPUT_ENABLED`.** The engine targets
SDL 2.30; the Miyoo fork is **2.0.20**, where the hint does not exist. It was
the only unguarded newer hint in the file (the neighbours already sit behind
`#ifdef SDL_HINT_JOYSTICK_WGI`). Now guarded. The hint is a Windows backend
switch and means nothing on the device.

Verified: the comparison operators were checked against an independent
lexicographic predicate over 7×7 key pairs plus a `std::map` keying test — all
49 pairs matched — and all four fast3d C++ translation units compile at
`-std=gnu++17`.

---

## 3. Toolchain

Two images. The second exists because the upstream one is missing things the
build needs.

```sh
docker build -t miyoomini-toolchain ./union-miyoomini-toolchain
docker build -t miyoo-build -f scripts/port/miyoo-build.Dockerfile .
```

`miyoo-build` adds what the upstream image lacks, each for a concrete reason:

- `autoconf`/`automake`/`libtool` — `sdl2/sdl2/autogen.sh` dies with
  `autoconf: not found` without them.
- `CMake 3.27.9` (Kitware tarball) — the image ships **3.13.4** and
  `CMakeLists.txt` line 1 requires **3.16**. Fetched by tarball rather than pip
  because buster's pip is 18.1 on Python 3.7 and cannot resolve modern
  manylinux wheel tags.
- `ninja-build` — not installed, and the build uses `-G Ninja`.

The tarball is arch-selected (`x86_64` / `aarch64`) so the same Dockerfile
works on an Intel host and on Apple Silicon. This host is arm64 and ran the
image natively.

Vendored trees (cloned, not committed — `sdl2/` is ~1.5 GB with SwiftShader):

- `sdl2/` — `git@github.com:steward-fu/sdl2.git` @ `0631abc8e8916db6f9bc7e2afd0c22913d092a29`
- `union-miyoomini-toolchain/` — `git@github.com:shauninman/union-miyoomini-toolchain.git` @ `cfcae3083679de59d6600d4ba05676c654b6d24e`

---

## 4. The SDL2 linkage

**The engine must link the fork, not a stock SDL2**, because only the fork has
the `Mini` video driver (MI_GFX panel, EGL/GLES via `SDL_gles_mini.c`) and the
`Miyoo Mini` audio driver.

**The prebuilt `.so` files cannot be paired with `sdl2/sdl2/include/`.**
`SDL_config.h` is *generated* by configure, and the header tree in the repo has
no `__linux__` branch — on an ARM Linux target it falls through to
`SDL_config_minimal.h`, which is a different feature set and different struct
layouts from the library it would be linked against. So the headers come from a
configured build of the fork.

```sh
docker run --rm -v "$PWD":/root/workspace/Miyoo_port miyoo-build \
  bash /root/workspace/Miyoo_port/scripts/port/build_miyoo_sdl2.sh
```

That script also builds **json-c 0.15** (from `sdl2/sdl2/dependency/`) — the
mini *audio* driver really uses it, reading the system volume out of a json-c
config file, so it is a link dependency. Two wrinkles it handles:

- The bundled json-c tarball is a git snapshot with no generated `./configure`,
  so it is built with CMake through this repo's own
  `cmake/miyoomini-armhf.cmake` — which doubles as a check that that file works.
- `SDL_video_mini.c` does `#include "SDL_image.h"` but never calls an `IMG_*`
  function, so only the header is staged, not the library. Building SDL2_image
  would drag in libpng/libjpeg/webp for nothing.

Output goes to `build_miyoo_deps/` — deliberately inside the mounted repo,
because anything under `/root` is discarded with `docker run --rm`.

The engine links `SDL2 EGL GLESv2` from that prefix with
`BUILD_WITH_INSTALL_RPATH ON`, so the shipped binary carries `RPATH=$ORIGIN`
and not this machine's build prefix. `launch.sh` additionally puts its own
folder first on `LD_LIBRARY_PATH`, because SDL's mini driver `dlopen`s
`libEGL`/`libGLESv2` by bare name. `libmi_*.so` are **not** shipped — they are
firmware, in `/config/lib` and `/customer/lib`, which the same
`LD_LIBRARY_PATH` covers.

---

## 5. CMake changes

Three architecture branches and one SDL2 branch, all keyed on
`CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7.*)$"`:

- **Compile flags**: ARM gets `-ffp-contract=off` and none of
  `-m32 -msse2 -mfpmath=sse`, which are x86 requests the ARM compiler rejects.
  Contraction stays off because Cortex-A7's VFPv4 fuses multiply-add and this
  engine's N64 single-precision comparisons are load-bearing — the same class
  of hazard the i386 build dodges by disabling x87 excess precision.
- **Link flags**: `-no-pie` without `-m32`, for the same reason as i386 (the
  compiled-in asset objects are `-fno-pic` and carry absolute relocations a PIE
  link rejects).
- **`ASSET_C_FLAGS`**: drops `-m32 -malign-data=abi`. The part of the layout
  contract that matters (`-fno-toplevel-reorder`,
  `-fno-zero-initialized-in-bss`, `-fno-common`, declaration order) is
  architecture-neutral, and both targets are little-endian, so the byte-for-byte
  `.data` reproduction the loader depends on is unchanged.
- **`GE_VIDEO` SDL2 branch**: requires `-DSDL2_MIYOO_ROOT`, links
  `SDL2 EGL GLESv2` from it, sets the `$ORIGIN` RPATH.

`cmake/miyoomini-armhf.cmake` carries the ABI:
`-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`, sysroot under
`/opt/miyoomini-toolchain`, all paths as cache variables.

Full command:

```sh
cmake -S . -B build_miyoo -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/miyoomini-armhf.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DGE_VIDEO=ON \
  -DSDL2_MIYOO_ROOT=/root/workspace/Miyoo_port/build_miyoo_deps
cmake --build build_miyoo --parallel
```

---

## 6. Assets — no host-side extraction in the shipped build

With `GE_NATIVE_64=OFF` and the default `GE_ROMDATA_FALLBACK=OFF`, the exe
compiles in **three** menu models (`PlegalpageZ`, `PnintendologoZ`,
`PgoldeneyelogoZ`) and reads every other asset from the user's cart at runtime
via `port/src/romdata.c`. The other 699 entries in
`port/generated/asset_manifest.cmake` get custom commands registered but are
never built, because they are not in the target.

So the packaged app needs no extraction step and no converted `assets/` tree —
just the cart image in `Data/`.

Those three models *are* needed at build time, though, and they are generated
from the ROM, so a from-scratch build does need the cart once:

```sh
python3 scripts/port/make_baserom_z64.py     # rom/ -> baserom.u.z64
docker run --rm -v "$PWD":/root/workspace/Miyoo_port miyoo-build bash -lc \
  'cd /root/workspace/Miyoo_port && bash scripts/extract_baserom.u.sh && make convert_props'
```

`make_baserom_z64.py` exists because the extraction scripts read raw ROM
offsets and want `baserom.u.z64`, while the user's file is at
`rom/007 - GoldenEye (USA).n64` and is **byte-swapped** (`.v64` magic
`37 80 40 12` — the game accepts it because `romdata.c` sniffs the header, but
the extractor would not). Normalising to `.z64` order yields
`sha1 abe01e4aeb033b6c0836819f549c791b26cfde83`, the exact stock (U) image.
The extraction produces 2698 files and 340 converted props; `baserom.u.z64` and
everything under `assets/` and `bin/` are gitignored, as they are ROM-derived.

---

## 7. The real problem: the renderer

**The Miyoo Mini has no 3D GPU.** The SoC (SigmaStar SSD202D) has a 2D blitter
(`MI_GFX`) and a video pipeline. `libEGL.so`/`libGLESv2.so` in
`sdl2/prebuilt/` are **SwiftShader 4.1.0.7**, Google's CPU rasteriser:

```
$ strings sdl2/prebuilt/640x480/libGLESv2.so | head -2
OpenGL ES 3.0 SwiftShader 4.1.0.7
OpenGL ES GLSL ES 3.00 SwiftShader 4.1.0.7
```

So `GE_VIDEO=ON` links and then rasterises GoldenEye on a 1.2 GHz Cortex-A7 in
software, at 640×480, with 128 MB of RAM. That is the thing to expect when this
is first run, and the expected outcome is single-digit fps, not a playable game.

The fork's `SDL_Renderer` "Mini" backend is not an escape hatch as shipped:
`Mini_QueueGeometry`, `Mini_QueueFillRects`, `Mini_QueueDrawPoints` and
`Mini_RunCommandQueue` are all empty stubs. Only `Mini_QueueCopy` →
`GFX_Copy` does real work, i.e. accelerated *blits*, not triangles. MI_GFX
itself exposes exactly two calls — `MI_GFX_BitBlit` and `MI_GFX_QuickFill` — so
a hardware triangle path would have to be built from blitted spans, which is
another project.

The design being pursued instead, and its rationale, is in
`docs/port/miyoo-backend.md` and `HANDOFF.md`.

---

## 8. Reproducing it

```sh
# 1. toolchain images (needs network)
docker build -t miyoomini-toolchain ./union-miyoomini-toolchain
docker build -t miyoo-build -f scripts/port/miyoo-build.Dockerfile .

# 2. ARM SDL2 + json-c into build_miyoo_deps/
docker run --rm -v "$PWD":/root/workspace/Miyoo_port miyoo-build \
  bash /root/workspace/Miyoo_port/scripts/port/build_miyoo_sdl2.sh

# 3. ROM extraction (once per clean checkout)
python3 scripts/port/make_baserom_z64.py
docker run --rm -v "$PWD":/root/workspace/Miyoo_port miyoo-build bash -lc \
  'cd /root/workspace/Miyoo_port && bash scripts/extract_baserom.u.sh && make convert_props'

# 4. the engine
docker run --rm -v "$PWD":/root/workspace/Miyoo_port miyoo-build bash -lc \
  'cd /root/workspace/Miyoo_port && cmake -S . -B build_miyoo -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE=cmake/miyoomini-armhf.cmake \
     -DCMAKE_BUILD_TYPE=Release -DGE_VIDEO=ON \
     -DSDL2_MIYOO_ROOT=/root/workspace/Miyoo_port/build_miyoo_deps && \
   cmake --build build_miyoo --parallel'

# 5. package
sh scripts/port/package_miyoo.sh
# copy dist/App/ProjectMidas/ to /mnt/SDCARD/App/ProjectMidas/
# and put the cart image in its Data/ folder
```

On Apple Silicon, run the containers without `--platform` (this host did, and
the toolchain ran natively). On an Intel host they are x86_64 either way.

---

## 9. Where it is most likely to break on first boot

Ordered by confidence, none of it observed yet.

1. **Context profile round-trip.** `gfx_opengl_init` decides `gl_es` by reading
   back `SDL_GL_CONTEXT_PROFILE_MASK`. The mini driver ignores major/minor in
   `glCreateContext`, so if the mask does not come back as `ES`, the engine
   emits GLSL `130` and a GLES 3.0 context rejects it. `gfx_sdl2.cpp` also
   tries desktop profiles (index 1) before ES (index 4), so expect the first
   attempts to fail and the log to show retries.
2. **`glad` is a desktop-GL loader.** It was generated for GL 1.0–4.1 and
   resolves through `SDL_GL_GetProcAddress` → `eglGetProcAddress`, so most
   entry points will resolve, and `gfx_opengl.cpp` has a genuine `gl_es` path
   (GLSL `300 es`, VAO forced, `precision mediump float`). But `GLVersion` is
   parsed from a `GL_VERSION` string that reads `"OpenGL ES 3.0 SwiftShader"`.
3. **`sysArgCheck`/`sysArgGetString` are stubs** (`port/src/sysstubs.c` return
   0/NULL, "no command-line option system yet"), so `--gl-version` and
   `--debug-gl` do nothing and §9.1 has no escape hatch without a code change.
4. **Unaligned access.** `SIGBUS` from decompiled code; will surface as a crash
   in some gameplay path.
5. **Audio.** `port/src/audio_pc.c` has `#if defined(__linux__) &&
   defined(__i386__)` PipeWire blocks that compile out on ARM — correct, since
   the mini audio driver is what is wanted, but it means the i386 path has no
   exercised ARM equivalent.

---

## 10. Memory

Two 64 MB reservations, both `mmap`'d lazily, so they cost address space rather
than committed RAM:

- `PORT_MEMP_ARENA_SIZE` — 64 MB, the N64 general heap (`port/src/pcsched.c`).
- `PORT_HIGH_ARENA_SIZE` — 64 MB, for converted display lists.

On a 128 MB device with the ROM (12 MB), SwiftShader (~22 MB mapped), textures
and the framebuffer, committed RSS is the thing to measure, not address space
(3 GB available on 32-bit ARM). The two constants are the first knobs. Not
changed.

Load-bearing and ARM-relevant: `portAllocHigh*` refuses any mapping below
`0x10000000`, because fast3d's `seg_addr()` treats untagged low words as
segment references. 32-bit ARM Linux `mmap` returns high addresses, so this
should hold — but a failure there degrades textures rather than failing loudly.
