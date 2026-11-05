# The Miyoo Mini renderer backend

**Status: design. Nothing implemented yet.**

This is the plan for making Project Midas draw on a Miyoo Mini. The
cross-compile already works (`MIYOO_BUILD_ATTEMPT.md`) — what is missing is a
renderer that does not require a 3D GPU, because the device does not have one.

---

## 1. The constraint, stated precisely

The Miyoo Mini's SoC (SigmaStar SSD202D) has:

- a **2D blitter**: `MI_GFX`, with exactly two operations, `MI_GFX_BitBlit`
  (copy/scale a rectangle) and `MI_GFX_QuickFill` (fill a rectangle);
- a **video/display pipeline** (`MI_DISP`, `/dev/fb0`);
- a 1.2 GHz ARM Cortex-A7 with NEON, 128 MB RAM, a 640×480 panel.

It has **no 3D hardware**. Whatever draws GoldenEye's geometry is software.

That leaves three candidate paths.

### 1.1 SwiftShader (what `GE_VIDEO=ON` does today)

`libEGL.so` / `libGLESv2.so` in `sdl2/prebuilt/` are SwiftShader 4.1.0.7, and
that is what the current ARM binary links and `DT_NEEDED` names.

SwiftShader is a *general* OpenGL ES implementation: it compiles GLSL into its
own IR, then JITs that IR to machine code through Reactor/Subzero. That JIT is
heavy, it runs at startup, and the correctness surface it targets spans all of
ES 3.0. The engine needs a small fixed-function subset of that.

It also cannot be tuned. When it is 4× too slow there is no knob to turn.

**Use it as a reference implementation and a fallback, not as the target.**

### 1.2 Extend SDL's "Mini" renderer, then use `SDL_Renderer`

The fork ships `src/render/mini/SDL_render_mini.c`, and this looks attractive
until you read it: `Mini_QueueGeometry`, `Mini_QueueFillRects`,
`Mini_QueueDrawPoints` and `Mini_RunCommandQueue` are **empty stubs**. Only
`Mini_QueueCopy` → `GFX_Copy` does work, i.e. accelerated *blits*.

More decisively: **SDL2's renderer API has no depth buffer at all.** GoldenEye
needs z-testing and z-writing per triangle (`ZMODE_OPA`, `ZMODE_XLU`,
`ZMODE_DEC`, `ZMODE_INTER`). There is no place to put it in the SDL_Renderer
contract. So this path cannot work regardless of how much of the Mini renderer
is filled in.

### 1.3 A software rasteriser in the fast3d backend, presented via MI_GFX

`GfxRenderingAPI` (`port/fast3d/gfx_rendering_api.h`) is the seam. Implement it
in software, render into an offscreen surface, and blit that surface to the
panel with `MI_GFX_BitBlit`.

**This is the plan.** Reasons:

- The depth buffer, the four z-modes, the per-tile wrap modes and the colour
  combiner all have somewhere to live.
- GoldenEye's own constraints are a *performance gift*. The N64 rendered at
  320×240 with a fixed-function RDP: no mipmapping beyond what the game asks
  for, no MSAA in the base path, screen-linear (not perspective-correct) shade
  interpolation, and an RGBA5551 framebuffer. A rasteriser that reproduces
  exactly that does far less work per pixel than a general GL.
- The hardware gets used where it is good at it: stretching the finished
  320×240 frame to the panel.

---

## 2. What MI_GFX actually gives us

Read from `sdl2/mini/inc/mi_gfx.h` and what the fork's video driver does
(`sdl2/sdl2/src/video/mini/SDL_video_mini.c`):

```c
MI_S32 MI_GFX_BitBlit(MI_GFX_Surface_t *pstSrc, MI_GFX_Rect_t *pstSrcRect,
    MI_GFX_Surface_t *pstDst, MI_GFX_Rect_t *pstDstRect, MI_GFX_Opt_t *pstOpt,
    MI_U16 *pu16Fence);
MI_S32 MI_GFX_QuickFill(MI_GFX_Surface_t *pstDst, MI_GFX_Rect_t *pstDstRect,
    MI_U32 u32ColorVal, MI_U16 *pu16Fence);
```

Facts that shape the design:

- **`MI_GFX_Surface_t` holds a physical address** (`MI_PHY phyAddr`), not a
  pointer. The framebuffer must be physically contiguous *and* mapped into the
  process. The fork does this with `MI_SYS_MMA_Alloc` + `MI_SYS_Mmap` for its
  offscreen buffers, and `MI_SYS_Mmap` over `/dev/fb0`'s `smem_start` for the
  panel. Our surface has to be allocated the same way — **`malloc` will not
  work here**, and neither will the engine's existing high-arena strategy
  without checking.
- **Supported formats include `E_MI_GFX_FMT_RGBA5551`** (`mi_gfx_datatype.h`),
  which is the N64's own framebuffer format. `RGB565`, `ARGB4444`,
  `ARGB8888` are also available.
- The panel framebuffer is `ARGB8888`, `FB_BPP == 4`, double-buffered by
  setting `vinfo.yres_virtual = yres * 2` and flipping `vinfo.yoffset`.
- The fork blits with `E_MI_GFX_ROTATE_180` — **the panel is mounted rotated**.
  Any present path must match that or the image will be upside down.
- `MI_GFX_Opt_t` carries a clip rect, colour-key info, mirror/rotate, and blend
  flags (`E_MI_GFX_DFB_BLEND_*`).

**Open question, and the first thing to measure:** does `MI_GFX_BitBlit`
scale when the source and destination rects differ in size? The display path
passes differing rects (window size → panel size), which suggests yes. If it
does, the present step is free; if it only crops, we scale manually or render
at panel resolution.

---

## 3. Architecture

```
  game code
      |
      v
  gfx_pc.cpp          fast3d interpreter: display lists -> GfxRenderingAPI calls
      |
      v
  GfxRenderingAPI
      |
      +-- gfx_opengl.cpp   (desktop, existing)
      |
      +-- gfx_sw_miyoo.cpp (NEW: software rasteriser)
                |
                |  ARGB8888 or RGB565, physically contiguous, MI_SYS-mapped
                v
           MI_GFX_BitBlit  -->  /dev/fb0  (ARGB8888, rotate 180, scaled)
```

The new file is `port/fast3d/gfx_sw.cpp`, exporting
`struct GfxRenderingAPI gfx_sw_api`. `port/src/video.c:5326` currently does
`set.rapi = &gfx_opengl_api`; that becomes a build-time or runtime choice:

- Build-time is simpler: a `GE_RENDERER` CMake option (`opengl` | `software`),
  defaulting to `opengl` so nothing else changes.
- The window manager can stay `gfx_sdl2.cpp` *if* SDL's mini driver tolerates a
  window with no GL context. It probably needs a small `gfx_sdl2.cpp` branch
  that skips `SDL_GL_*` entirely and creates a plain window, plus a present hook
  that hands our surface to MI_GFX. That branch is the main integration risk —
  see §8.

### Why not write it as a new window manager too

Because `gfx_pc.cpp` owns input/timing through the window manager API and
`gfx_sdl2.cpp` already handles all of it for the fork. Only the GL-specific
parts need bypassing.

---

## 4. Staging plan

Each milestone should be independently demoable, in this order. Do not skip
ahead: milestone 2 proves the whole plumbing with no 3D risk at all.

### M0 — Prove the present path (no geometry)

A standalone test that allocates an MI_SYS surface, fills it with a gradient on
the CPU, `MI_GFX_BitBlit`s it to `/dev/fb0`, and flips. Run from `launch.sh`.

Proves: MMA allocation, mapping, blit, scaling, rotation, double-buffer flip.
**This retires the highest-risk unknown in the whole plan.**

### M1 — `GfxRenderingAPI` skeleton that clears the screen

Implement every pointer as a no-op or a stub returning a sane value, plus
`clear_framebuffer` actually filling the surface. The game should boot through
the legal screen to the menu with no geometry drawn, and not crash.

Proves: the API contract is satisfiable, the stubs are the right shape, and
`video.c`'s expectations are met.

### M2 — Flat triangles with depth

Vertex transform is already done by `gfx_pc.cpp` (it hands over clip-space
`x, y, z, w`); the backend only needs the perspective divide, the viewport
map, a scanline fill and a z-buffer. No textures, no combiner: write
`vInput1` (primitive colour) straight out.

Proves: triangle setup, winding/culling, depth compare modes, scissor, and the
y-orientation (which is a classic source of upside-down output).

### M3 — The colour combiner

Evaluate `CCFeatures` per pixel exactly as `gfx_opengl.cpp` generates GLSL for
it. The reference is mechanical: `shader_item_to_str` + `append_formula`
(`gfx_opengl.cpp` ~L235–330). The two-cycle path, `do_single` / `do_multiply` /
`do_mix`, and the `WRAP(texel, -1.01, 1.01)` then `WRAP(..., -0.51, 1.51)` then
`clamp` sequence all have to be reproduced.

Proves: untextured and textured geometry both draw correctly with the right
colours.

### M4 — Texture sampling

The largest single chunk. Formats the game uses: RGBA16, IA8/IA4/I8?, RGBA32,
CI4, CI8, YUV16. Filtering: nearest and (where requested) bilinear. Wrap:
`G_TX_WRAP` / `G_TX_MIRROR` / `G_TX_CLAMP`, per axis, driven by
`gfx_cm_to_opengl` (`gfx_opengl.cpp` ~L1050).

`upload_texture` receives RGBA32 from `gfx_pc.cpp`, so the *sampling-side*
formats may already be normalised by then — **verify this early**, because if
so M4 collapses to nearest/bilinear over RGBA32 and is far smaller.

### M5 — Blending, alpha test, coverage

`set_use_alpha(use_alpha, modulate)`:
`SRC_ALPHA, ONE_MINUS_SRC_ALPHA` normally, `DST_COLOR, ZERO` when
`modulate`. Plus `SHADER_OPT_ALPHA_THRESHOLD` (discard `a < 8/256`),
`opt_texture_edge` (discard `a <= 0.19`, else force opaque), and
`opt_alpha_cvg_sel` (force opaque, discarding zero-alpha first when noise is
on).

### M6 — Fog, grayscale, far clip, sky RGB5

Vertex fog (`vFog`), per-pixel fog (`opt_ge_pixfog`, recomputing the register
curve from `z/w`), `opt_ge_farclip` (discard where `z > w`),
`opt_grayscale`, `opt_sky_rgb5` (truncate to 5 bits).

### M7 — Framebuffer effects

`create_framebuffer`, `update_framebuffer_parameters`, `start_draw_to_framebuffer`,
`copy_framebuffer`, `select_texture_fb`, `clear_framebuffer`. These drive the
N64's framebuffer effects and menu transitions. `resolve_n64_aa` is
NULL-checked in `gfx_pc.cpp` (~L7939) so it can stay `NULL` initially;
`get_framebuffer_texture_id` is **not** NULL-checked where it is used, so it
has to return something valid once framebuffers are enabled. If framebuffers
can be disabled wholesale for the first boot, prefer that.

### M8 — Performance

Only after it is correct. Profile; likely wins are a 16-bit depth buffer at
320×240, avoiding the divide for attributes the N64 interpolated linearly
anyway, and SIMD-texturing 2×2 blocks with NEON.

---

## 5. Correctness traps the engine already encodes

These are not optional details; each one is already a documented workaround in
the GL backend, and each will produce visible corruption if skipped.

- **`gl_Position.z *= 0.3f` when there is no `GL_DEPTH_CLAMP`**
  (`gfx_opengl.cpp` ~L455). A clipped-vertex workaround; find out whether the
  software path needs it (it does not clamp either).
- **`set_depth_range` pulls `zfar` to `0.9999995`** so far-plane-clamped
  geometry still passes `GL_LESS` against a buffer cleared to 1.0.
- **`ZMODE_DEC`** is a two-sided depth window in hardware; the GL backend
  emulates the near side with `LEQUAL` plus a bias, and adds the far side
  through a stencil pass (`gfx_opengl.cpp` ~L1275+). A stencil buffer is the
  honest reproduction.
- **Far-plane geometry is clamped, not clipped** — `gfx_opengl.cpp`'s
  `set_depth_range` comment.
- **`vGeZw`** is the fragment's own `z/w`, used for per-pixel fog and far clip;
  it must interpolate perspective-correct (that is the point).
- **Shade interpolation is screen-linear** (`opt_nopersp_shade`, the
  `noperspective` qualifier) for sky/water cells — so the software path's
  default linear interpolation is *correct* there, and perspective-correct
  elsewhere.
- **The ROM buffer and the display-list arena must stay above `0x10000000`**,
  because `seg_addr()` treats untagged low words as segment references
  (`port/src/pcsched.c`). An MI_SYS allocation that lands low would break
  display lists. Check `phyAddr`/`virAddr` against that bound.

---

## 6. Memory budget

128 MB total. Currently reserved lazily, so address space rather than RSS:

- `PORT_MEMP_ARENA_SIZE` 64 MB (N64 general heap)
- `PORT_HIGH_ARENA_SIZE` 64 MB (converted display lists)
- ROM 12 MB
- SwiftShader ~22 MB mapped (goes away if we stop linking it)

A software framebuffer is small: 320×240×4 = 300 KB for colour, 150 KB for a
16-bit depth buffer, and two of each for double buffering — under 1 MB. That is
not the problem. The two 64 MB arenas and the texture cache are.

**Measure before optimising.** The plausible first change is dropping the
arenas to 32 MB each.

---

## 7. Performance budget

Crude but useful, at 320×240:

- 76,800 pixels per frame; GoldenEye's overdraw is maybe 2–4×, so call it
  200–300 K fragment evaluations per frame.
- If a fragment costs 50–100 cycles (texture fetch + combiner + blend, no
  perspective divide), that is 15–30 M cycles per frame.
- At 1.2 GHz that is **12–25 ms/frame, i.e. 40–80 fps**, before the blit.

At 640×480 the pixel count is 4×, which puts it at 10–20 fps — so **320×240
with an MI_GFX upscale is the right target**, and it is also what the N64 did.
The `sdl2/prebuilt/320x240/` library set exists for exactly this.

The budget is therefore not obviously doomed. The danger is a naive per-pixel
path (a divide, a `float`→`int` conversion and a virtual call per pixel), which
can easily be 10× over and is the difference between playable and not.

---

## 8. Open questions and decision points

1. **Does `MI_GFX_BitBlit` scale?** Decides whether present is free. Resolve in
   M0.
2. **Can `gfx_sdl2.cpp` run without a GL context?** It currently always sets
   `SDL_WINDOW_OPENGL` and calls `SDL_GL_CreateContext` in a version-negotiation
   loop. Either add a "no GL" mode or accept that the GL context still gets
   created (via SwiftShader) and simply never draw with it — which would work
   but keeps 22 MB of SwiftShader resident. Prefer the former.
3. **Where does the surface live relative to `0x10000000`?** See §5.
4. **Is `upload_texture`'s input always RGBA32?** If yes M4 shrinks a lot.
5. **Does the game tolerate `gfx_framebuffers_enabled == false`?** If yes, M7
   can be deferred indefinitely.
6. **320×240 or panel-native?** Depends on (1), and on what the menu text
   needs to be legible.

---

## 9. What "done" means

In order, and each is a real milestone rather than a vibe:

1. The menu draws, correctly oriented, at a readable size.
2. A stage loads and draws without crashing (untextured geometry is acceptable).
3. Textured, fogged, alpha-blended geometry looks right.
4. Playable framerate at 320×240 (the number to pick when we get there; the
   N64 ran at ~20–30 fps).
5. `scripts/port/test_the_tests.sh` still passes, and the macOS build is still
   unregressed.

Milestone 3 is where the port becomes interesting; milestone 4 is where it
becomes a product.
