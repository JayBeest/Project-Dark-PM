# Project Dark (Perfect Midas)

**The bridge between the C legacy and the memory-safe, thread-safe future.**

## What is this?

Project Dark is a fork of the GoldenEye 007 PC port ([Project Midas](https://github.com/murkantor/Project-Midas)),
maintained under the OmniDyne Industries ecosystem.

Where the original port remains the stable, mainstream preservation project, this fork is the
test-bench and architectural vanguard for the next generation of Rareware PC ports. Its first
deliverable is **Plumb**, the diagnostic toolkit described below.

The name reflects a dual trajectory:

- **Perfect Midas (C)** — *the present.* A highly optimized, cross-platform (Windows, Linux,
  native macOS) C engine with deep audio and rendering fixes. It is the reference implementation
  for the OmniDyne Shared Format.
- **Project Dark (Rust)** — *the destination.* A ground-up rewrite of the engine leveraging Rust's
  memory safety and fearless concurrency, so audio, rendering and netplay can run in parallel
  without data races. No Rust is in this repository yet. The C engine is what makes that rewrite
  tractable, because it is the behaviour a rewrite has to reproduce.

## Plumb

**Plumb is the diagnostic toolkit in this repository** — the part that measures the engine rather
than changing it.

The name is a double meaning, both literal: a *plumb line* is what you hold up to check that a wall
is actually true, and *plumbing* is the single choke point every write passes through. This toolkit
does both. It instruments the engine, and it records what actually ran, so a claim about performance
or settings can be checked instead of trusted.

What it gives you today:

- **The audio pump block** — live pump telemetry, drawn in the front end *and* in play, so a problem
  is visible before a stage loads. `cl_audio_pump 1` in the console, or `-audio_pump` at launch.
- **A per-second pump log** — `cl_audio_log 1` writes `Data/Logs/audio_pump.log`: one line a second
  of queue depth, dry counts, underruns and the wall-clock gap between pumps. A long session's
  drift becomes a file you can read.
- **An in-game console** — press `` ` ``. `help` lists every command; `cl_classic` reports whether
  Classic Mode and its settings agree.
- **A claim line per run** — the config line in the log says what the renderer was *created with*,
  not what a menu thought it set. When the log and `midas.ini` disagree, the log is the truth.
- **Arguments you can script** — `-skip`, `-stage`, `-classic`, `-audio_pump`, `-audio_log`, so a
  capture or investigation run needs no console and no hand-edited config.
- **Tests that can fail** — `scripts/port/test_the_tests.sh` mutates the shipped source and checks
  that something breaks. A passing test proves nothing on its own; this asks whether it *can* fail.

## The OmniDyne Ecosystem

OmniDyne Industries aims to solve the fragmentation of the GE/PD modding scene. This repository is
where we build the rails.

- **The Universal Modding Format** — a shared, platform-agnostic data model, so mods, level editors
  and character injectors work across GE, PD and future engines. *Status: design.*
- **Modular Packaging and Distribution** — no more bolted-on EXEs: a unified packaging standard so
  players can install, update and manage mods, character packs and total conversions.
  *Status: design.*
- **Modular Netplay** — netcode as a pluggable module, so different implementations can be
  benchmarked, tested and swapped without forking the engine. *Status: design.*
- **Diagnostic Tooling** — *status: shipping.* This is Plumb, above. We prioritize observability
  because every other item on this list needs a way to tell whether it is working.

Nothing else on that list has an implementation yet. It is published so the direction is legible,
not because it is ready.

### A roof, not a rival

OmniDyne is meant to be somewhere the people already building in this space work *together*
rather than in parallel — the GoldenEye X team, the Phantom work in the KAI branch, and everyone
else with a fork, a tool, a map or an opinion about how a GoldenEye port should feel.

None of that is a merger, and nothing here is a claim on anyone's work. It is an invitation, and
the [credits](#credits-and-origins) are where an invitation has to be made good. If you build for
GE or PD and want the rails shaped around what you actually need, open an issue.

## Core tenets of development

### 1. Clean slices and testability

We do not merge 5,000-line unreadable PRs. A commit does exactly one thing. We write tests, and
mutation audits to prove the tests can actually fail. If the pacing code breaks, the diagnostic log
should say exactly why.

### 2. AI-assisted, human-reviewed

We use AI tools aggressively to map codebases, write tests and find edge cases. AI is a tool, not
an author: every line of generated code is understood, tested and owned by a human. See the
[AI disclaimer](#ai-disclaimer) at the end.

### 3. Concurrency and thread safety

The goal is to decouple the game loop, the audio synthesiser and the renderer into independent
threads. The C engine provides the stable baseline and the measured behaviour; the Rust engine will
provide the safe, parallel future.

### 4. Preservation and convergence

We do not throw away the legacy C code; we honour it. But we do not let a 32-bit ABI dictate the
future. GoldenEye and Perfect Dark converge toward one native, modern architecture.

## Play and build

You need your own GoldenEye 007 (USA) cart image. Nothing here ships game data.

1. Put the cart in `Data/` next to the executable — see `Data/README.txt` for accepted formats.
2. Run `ProjectMidas.exe` (Windows) or `ProjectMidas` (Linux/macOS).

Quick build and test on a native platform:

```bash
cmake -S . -B build_native -G Ninja -DCMAKE_BUILD_TYPE=Release -DGE_VIDEO=ON -DGE_PORT_TESTS=ON
cmake --build build_native --parallel
ctest --test-dir build_native --output-on-failure
```

Building does not require the cart; **playing** does.

Full instructions: [BUILD.md](BUILD.md) for every platform and the CMake options,
[PORT.md](PORT.md) for the files next to the executable and the settings files,
[CLI.md](CLI.md) for every command-line flag, and [SETTINGS.md](SETTINGS.md) for what the settings
mean.

### Skip the boot sequence

```bash
ProjectMidas -skip              # skip the logos and legal screen, land on Select File
ProjectMidas -skip 2            # skip two of them, stop on the Rare logo
ProjectMidas -skip -stage dam   # straight into a mission, no menus
```

## Credits and origins

This project stands on the shoulders of giants.

- **fgsfdsfgs** for sliced bread.
- **Murkantor and the Project Midas team** for the phenomenal base C port.
- **The Perfect Dark decomp team** for the architectural inspiration.
- **Dab, Catherine, Wren (Wreck?) and the broader community** for pushing the boundaries of what the
  engine can do.
- **JillyJane, Shipoflove, Odeyseis, alkamaas, savantique and Torn** for the work they do for the
  scene this project exists to serve.
- **Balthazar Soulstealer (Nitrous Icefangs), Graslu, Kai, Wreck, Myles, Jeanaru and RedVox57** for
  making the community this fork grew out of, and for keeping the GoldenEye and Perfect Dark scenes
  alive.
- **The OmniDyne architects** for the shared format mapping.
- **Nintendo, Rare, James Bond etc**

Credit here follows contribution, and the list is not a closed set. If you have worked on
something this project uses and are not named, say so in an issue or on the Discord — being left
off is an oversight, not a judgement. If you are named here in error, or under a name you no longer
use, say so and it will be corrected.

## Licence and fork status

This is a fork of Project Midas, distributed under the **same MIT licence**, retained in
[LICENSE](LICENSE): *Copyright (c) 2026 murkantor and Project Midas contributors.* That notice stays,
as the licence requires. What this fork adds is under the same terms.

## AI disclaimer

Claude (as Codewhale) was used in the making of this project. If you do not agree with that, please
do not download either the executables or the source.

---

*Built with an agent. Reviewed by a human. Tested against reality.*

*Many projects. One shared force.*
