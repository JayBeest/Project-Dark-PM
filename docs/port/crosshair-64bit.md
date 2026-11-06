# The crosshair SIGSEGV — a 32-bit word holding a 64-bit pointer

What crashed, what the backtrace could and could not say, the pointer rule that
was wrong, and what is still open. Written to be read on its own in six months;
the symbols it names are the authority, not this file.

Observed 2026-11-06 on macOS arm64 (Apple M5), from `-terminal` runs of attract
demos. Two crashes, two different demos, same backtrace.

---

## OPEN — bisect when this started

**Not yet answered: how long this has been broken.** The reporter never tested
the game between the changes that plausibly introduced it, so there is no known
good build. The fix below is correct on its own terms (the invariant is
provable, see "Why this is a bug and not a theory"), but it has **not** been
shown to be the cause of the observed SIGSEGV — the fault address was never
captured.

A future session should:

1. Reproduce the crash with the `SA_SIGINFO` handler in place, and read
   `ge: FAULT addr=…` from `Data/Logs/terminal.log`.
2. If `addr` is a near-miss (high bits intact, low bits plausible) the
   truncation in this document is the cause; if it is `0x0` it is a NULL
   dereference and this is a different bug; if `headroom` in the same block is
   near zero it is a stack overflow and neither.
3. `git log -S'portAllocHigh' -- src/game/gunfire.c port/src/port_textures.c`
   and `git log -S'TARGET_N64' -- src/game/gunfire.c` bracket the change that
   put a host pointer into a display list. Then build the suspect revisions on
   Apple Silicon and run a demo.

The reason this cannot be answered by reading alone is in "What the backtrace
could not say": the reported frame is a getter that cannot fault.

---

## The symptom

```
ge: fatal signal 11, backtrace (10 frames):
2   ProjectMidas    portDisplayCrosshairImage + 188
3   ProjectMidas    gunDrawSight + 712
4   ProjectMidas    maybe_mp_interface + 1696
5   ProjectMidas    lvlRender + 4824
6   ProjectMidas    bossMainloop + 2172
```

The trigger was a multiplayer spawn reached from an attract demo, with a
suggestive line just before it:

```
ge: mp spawn p0 startpad=0 (p885a1) stanroom=2 pos=(779 ? -60)
```

That `?` is a coordinate that never got set. It is a lead, not a conclusion.

## What the backtrace could not say

Each frame was checked against the binary by matching `+offset` to the
instruction *after* a `bl`:

- `gunDrawSight+712` is the instruction after `bl _portDisplayCrosshairImage`
- `maybe_mp_interface+1696` is the instruction after `bl _gunDrawSight`
- `portDisplayCrosshairImage+188` is the instruction after `bl _portCrosshairColor`

So the chain is real. But the last one is impossible as a fault site:
`portCrosshairColor()` compiles to three instructions —

```asm
adrp x8, ...
ldr  w0, [x8, #0x2cc]
ret
```

— reading a static `s32` from valid `__DATA`. **It cannot fault.**

What happened is that the genuinely faulting function was a leaf with no frame
pointer, so `backtrace()` walked past it and reported its caller's return
address. On this platform the backtrace points **one frame too high**. That is
why the report is not actionable on its own, and why the crash handler was
changed to capture the fault address (see "The instrumentation that was added",
below).

## The rule that was wrong

A `Gfx` word is 32 bits. When a host pointer is baked into one it must survive
`(u32) (uintptr_t)` *and* still be recognisable as a host pointer by
`gfx_pc.cpp`'s `seg_addr()`; otherwise the truncated value is read as an N64
segment reference:

```c
/* port/include/port_hostptr.h, before */
#define PORT_IS_HOST_ADDR(v) ((uintptr_t) (v) >= 0x10000000u)
```

Callers checked that test and, on every 32-bit build the port grew up on, it was
sufficient — a 32-bit pointer is always ≤ `0xFFFFFFFF`. On LP64 it says nothing
about the upper half. `src/game/gunfire.c` then did:

```c
g->words.w1 = (u32) (uintptr_t) copy;
```

### Why this is a bug and not a theory

Measured on the machine that crashed, not assumed:

```
arena address      : 0x1071ac000        (ge: portAllocHigh boot line)
fits in 32 bits?   : False
truncated to u32   : 0x071ac000
PORT_IS_HOST_ADDR? : False              → seg_addr sees segment 7
```

And the platform offers no way out by allocating lower:

```
hint                     returned       fits u32?
NULL (let the OS pick)   0x104afc000    NO
0x10000000               0x104afc000    NO
0x40000000               0x104afc000    NO
MAP_FIXED below 4 GB     failed         —
```

`mmap` ignores the high-arena hint entirely, and `MAP_FIXED` into
`[0x10000000, 0x100000000)` fails because on arm64 macOS **user space starts at
`0x100000000`**. There is no address below 4 GB to be had.

So on Apple Silicon every `portAllocHigh` result is unrepresentable in a `Gfx`
word, and the comment above those call sites — "use the high arena so `seg_addr`
does not misread it" — describes a guarantee that the platform cannot provide.
It is true, and insufficient, at the same time.

## The fix

One rule, stated once, in `port/include/port_hostptr.h`:

```c
#define PORT_DL_POINTER_FITS(p) \
    (PORT_IS_HOST_ADDR(p) && (uintptr_t) (p) <= 0xFFFFFFFFu)
```

The lower bound is `PORT_IS_HOST_ADDR`, not a literal — the Xbox build rebases
its image to `0x00010000` and legitimately has host pointers below the PC floor.
On any 32-bit target the upper bound is vacuous, so this reduces exactly to the
old rule there: **no behaviour change on Windows, Linux or x86**.

Enforced at both ends of the crosshair path:

- **`portTexCrosshairCopyFor`** (`port/src/port_textures.c`) returns `NULL` when
  the copy it just allocated cannot be represented, instead of handing back a
  pointer that will truncate. The caller already handles `NULL`, so the visible
  result is the vanilla crosshair.
- **`gunfire.c`** re-checks before writing the word. Same rule at the write
  site, because the copy pool is not the only thing that could ever supply one.

It logs once, so a silent degradation cannot be mistaken for success:

```
ge: xhair: tint copy at 0x… does not fit a 32-bit display-list word
(user space starts at 4 GB here) - drawing the vanilla crosshair
```

`port/tests/dl_pointer.c` pins the rule, including the case that bit the port:

```
heap pointer      : 0x10525d0f0
>= 0x10000000     : yes      ← what the code used to check
fits a Gfx word   : NO       ← the condition that actually matters
```

## The instrumentation that was added

`port/src/main.c` now installs `portArmCrashSignals()`, which uses
`sigaction`/`SA_SIGINFO` instead of `signal()`, and prints:

```
ge: FAULT addr=0x… (unmapped)
ge: FAULT pc=0x… sp=0x… fp=0x… lr=0x…
ge: FAULT stack: sp is N of M bytes used (X%), headroom K
```

Three failure modes look identical in a backtrace and need opposite fixes: a
wild pointer, a NULL dereference, and a stack overflow. These three numbers
separate them. Verified against a deliberate NULL dereference
(`build_macos/crash_probe.c`), which correctly reports `addr=0x0`.

## Known scope, deliberately not fixed here

The same pattern exists at other sites that bake `portAllocHigh` results into
display lists, and is probably broken on Apple Silicon for the same reason:

- `port/src/port_textures.c` — external BMP texture packs (`t->blob`).
- `src/game/objecthandler_2.c` — the model arena, whose own comment says
  "pool pointers bake into DLs".

Those were left alone: the reported path was the crosshair, and widening a fix
for a fault that has not been reproduced is guesswork. They should be audited
with `PORT_DL_POINTER_FITS` when one of them misbehaves.

## What is *not* claimed

- That this caused the observed crash. It is a real defect and it is fixed; the
  causal link is unproven because the fault address was never captured.
- That the crash is gone. It was not reproducible locally: ~13,000 frames across
  a full demo loop on the reporter's ROM exited cleanly.
- That two crashes in two demos means anything specific. It means the *site* is
  deterministic and the *conditions* leading to it are not.
