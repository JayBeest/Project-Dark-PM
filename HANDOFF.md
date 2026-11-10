# Handoff — Miyoo Mini port

Written 2026-11-05, branch `Miyoo_mini_port`. Everything below was verified on
this machine; anything unverified is marked as such.

---

## The job

Port Project Midas (the SDL2/OpenGL PC engine — not the raw N64
decompilation) to the Miyoo Mini: ARM Cortex-A7, OnionOS, 128 MB RAM, 640×480
panel, **no 3D GPU**.

Two deliverables, and only the first is done:

1. **Cross-compile it.** Done. It builds, links and packages. Never run.
2. **Make it draw.** Not started. The device has no 3D hardware, the shipped
   GLES is a CPU rasteriser, and a software renderer has to be written.
   Plan: `docs/port/miyoo-backend.md`.

Read `MIYOO_BUILD_ATTEMPT.md` first — it is the working record of the build:
what was proven, what was not, and every failure that had to be worked around.

---

## Where things stand

Commits on top of `7fbbfb1`:

```
76d4a43  fast3d: drop C++20-only comparison operators; guard a newer SDL hint
0d5c67a  build: cross-compile for the Miyoo Mini (ARMv7 Cortex-A7, OnionOS)
224f433  build: Miyoo SDL2/dependency build, ROM prep, and OnionOS packaging scripts
2a725ed  docs: record the Miyoo cross-compile attempt and its outcome
```

The first is genuinely portable (any GCC < 10 / SDL < 2.0.22 needs it) and
macOS is unregressed by it — `ctest` 14/14.

### Verified

- `ProjectMidas` is a 32-bit ARM EABI5 hard-float executable,
  interpreter `/lib/ld-linux-armhf.so.3`.
- `DT_NEEDED` is exactly `libSDL2-2.0.so.0`, `libEGL.so`, `libGLESv2.so`,
  `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6`; `RPATH` is
  `[$ORIGIN]` with no build path left in it.
- The ARM SDL2 fork builds with both Miyoo drivers compiled in (`Mini` video,
  `Miyoo Mini` audio).
- `scripts/port/package_miyoo.sh` assembles 43 MB into
  `dist/App/ProjectMidas/`.
- `m0_present` cross-compiles and links: 32-bit ARM EABI5 hard-float, `DT_NEEDED`
  `libmi_gfx.so`, `libmi_sys.so`, `libmi_common.so`, libpthread/libdl/librt/libm/libc,
  `RPATH` `[$ORIGIN]`. It needs `-lpthread -ldl -lrt` on the link line because the
  SDK's `libmi_common.so` has no SONAME and does not carry its own dependencies.
- macOS: full build, `ctest` 100% (14/14).

### Not verified — do not assume any of this

- **Nothing has ever executed.** No boot, no frame, no audio, no input. Not on
  hardware, not under QEMU.
- Float ABI agreement between the union toolchain, the SDL2 fork and the
  prebuilt SwiftShader libs is asserted by flags, not observed.
- glibc compatibility between the toolchain sysroot and the OnionOS firmware.
- Unaligned-access `SIGBUS` hazards from decompiled code on ARMv7.

---

## Reproducing the build

Nothing here is cached in git; `sdl2/`, `union-miyoomini-toolchain/` and
`build_miyoo_deps/` are all gitignored. From a clean tree:

```sh
# 0. vendored trees (exact commits the build was proven against)
git clone https://github.com/steward-fu/sdl2              # 0631abc8e8916db6f9bc7e2afd0c22913d092a29
git clone https://github.com/shauninman/union-miyoomini-toolchain  # cfcae3083679de59d6600d4ba05676c654b6d24e

# 1. images
docker build -t miyoomini-toolchain ./union-miyoomini-toolchain
docker build -t miyoo-build -f scripts/port/miyoo-build.Dockerfile .

# 2. ARM SDL2 + json-c -> build_miyoo_deps/
docker run --rm -v "$PWD":/root/workspace/Miyoo_port miyoo-build \
  bash /root/workspace/Miyoo_port/scripts/port/build_miyoo_sdl2.sh

# 3. ROM -> baserom.u.z64 -> extracted assets (once per clean checkout)
python3 scripts/port/make_baserom_z64.py
docker run --rm -v "$PWD":/root/workspace/Miyoo_port miyoo-build bash -lc \
  'cd /root/workspace/Miyoo_port && bash scripts/extract_baserom.u.sh && make convert_props'

# 4. engine
docker run --rm -v "$PWD":/root/workspace/Miyoo_port miyoo-build bash -lc \
  'cd /root/workspace/Miyoo_port && cmake -S . -B build_miyoo -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE=cmake/miyoomini-armhf.cmake \
     -DCMAKE_BUILD_TYPE=Release -DGE_VIDEO=ON \
     -DSDL2_MIYOO_ROOT=/root/workspace/Miyoo_port/build_miyoo_deps && \
   cmake --build build_miyoo --parallel'

# 5. package
sh scripts/port/package_miyoo.sh
```

Deploy: copy `dist/App/ProjectMidas/` to `/mnt/SDCARD/App/ProjectMidas/`, put a
cart image in its `Data/`, run. `launch.sh` logs to `Data/Logs/launch.log`.

---

## The next concrete action

**Milestone M0 in `docs/port/miyoo-backend.md`: prove the present path with no
3D involved.**

The tool now exists. `scripts/port/m0_present.c` allocates an MI_SYS surface,
paints a gradient with corner markers on the CPU, `MI_GFX_BitBlit`s it to
`/dev/fb0`, flips, reads the panel back and times 60 present passes. It answers
the two questions the whole plan hangs on:

1. Does `MI_GFX_BitBlit` **scale** when the source and destination rects differ?
2. Does an MI_SYS allocation land **above `0x10000000`**, which
   `port/src/pcsched.c` requires (below it, `seg_addr()` misreads display-list
   words as segment references)?

It is written, cross-compiled and packaged — **and has never executed**, because
nothing in this repository can execute ARM code without the device.

```sh
docker run --rm -v "$PWD":/root/workspace/Miyoo_port miyoo-build \
  sh /root/workspace/Miyoo_port/scripts/port/build_m0.sh   # -> dist/.../m0_present
```

On the device, from a terminal in the app folder, or by dropping the marker
file and launching from OnionOS:

```sh
cd /mnt/SDCARD/App/ProjectMidas && ./launch.sh m0
touch /mnt/SDCARD/App/ProjectMidas/Data/m0_probe   # alternative
```

Either way the answers land in `Data/Logs/m0.log`, one `key=value` per line,
ending in three `VERDICT` lines. It also leaves the gradient on screen for
three seconds after the blits.

Do not start the rasteriser before M0 passes. If the blit or the allocation
does not behave, the architecture changes, not the code.

---

## Traps that already cost time — do not rediscover

**Docker and the sandbox.** The daemon socket is at
`~/.docker/run/docker.sock`, outside the workspace sandbox, so calls need
`danger-full-access` escalation. In `Auto-Review` posture that escalation
cannot be approved at all; the session has to be in `Ask`.

**The base image is missing things.** `miyoomini-toolchain` lacks autotools,
CMake ≥ 3.16 (it has 3.13.4) and Ninja. All three are in `miyoo-build`; see
`scripts/port/miyoo-build.Dockerfile` for why CMake comes from a tarball rather
than pip.

**`docker run --rm` deletes `/root`.** Build outputs must land inside the
mounted repo (`build_miyoo_deps/`), or they vanish with the container.

**SDK version skew is real.** The engine targets SDL 2.30; the fork is
**2.0.20**. One hint (`SDL_HINT_DIRECTINPUT_ENABLED`) already failed to
compile. Expect more of these as more code is pulled in — guard them, do not
lower the engine's expectations globally.

**`assets/` is gitignored and ROM-derived.** Extraction is safe and local.
`baserom.u.z64` is produced from `rom/` by
`scripts/port/make_baserom_z64.py`; the user's file is byte-swapped despite its
`.n64` name, and the extractor reads raw offsets, so the reorder is required.

**The macOS build is the correctness reference.** It builds and passes tests in
seconds. Re-run it after every change that touches engine code — that is how
the `gfx_pc.h` edit was checked.

---

## Files added by this work

| Path | What |
| --- | --- |
| `MIYOO_BUILD_ATTEMPT.md` | the build record: proven / not proven, failures, reproduction |
| `docs/port/miyoo-backend.md` | the renderer plan, M0–M8, with the constraint analysis |
| `cmake/miyoomini-armhf.cmake` | ARM toolchain file |
| `scripts/port/miyoo-build.Dockerfile` | derived build image |
| `scripts/port/build_miyoo_sdl2.sh` | builds the ARM SDL2 fork + json-c |
| `scripts/port/make_baserom_z64.py` | byte-order normalisation of the cart |
| `scripts/port/m0_present.c` | M0 present-path probe: MMA alloc, gradient, BitBlit, readback, timing |
| `scripts/port/build_m0.sh` | cross-compiles and links that probe against `sdl2/mini/lib` |
| `scripts/port/package_miyoo.sh` | assembles the OnionOS folder |
| `dist/App/ProjectMidas/launch.sh` | OnionOS launcher (park MainUI, log the run) |
| `dist/App/ProjectMidas/config.json` | App entry |

Modified: `CMakeLists.txt` (ARM branches, C++17), `port/fast3d/gfx_cc.h`,
`port/fast3d/gfx_pc.h`, `port/src/input_pc.c`, `.gitignore`,
`dist/App/ProjectMidas/launch.sh` (an `m0` mode), `scripts/port/package_miyoo.sh`
(reports whether the probe is in the package).

---

## The thing worth saying plainly

The build succeeding is a smaller result than it looks. It proves the ABI,
the toolchain and the packaging path, which is real and necessary work — but
until something draws, the interesting question is unanswered.

The evidence so far points one way: **the device cannot run the renderer this
engine ships with.** `GE_VIDEO=ON` links SwiftShader and will rasterise
GoldenEye in software on a 1.2 GHz single-core-class part, at 640×480, on
128 MB. The measured-by-arithmetic budget in the plan says a purpose-built
rasteriser at 320×240 plausibly lands at 40–80 fps, and SwiftShader plausibly
does not. That difference is the whole project.
