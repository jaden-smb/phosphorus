# AGENTS.md

This file provides guidance to AI coding agents (Claude Code, Codex) when working with code in this repository.

## What this is

Phosphorus (`phx`) is a from-scratch C++17 2D/2.5D retro game engine that runs the **same gameplay
code** on Game Boy Advance, PSP, Windows, and Linux. The whole design is organized around closing a
~17,000× RAM gap between a 256 KB GBA and a multi-gigabyte PC. Read `README.md` and the numbered
docs in `docs/` (start at `docs/00-architecture.md`) for the full picture; `STRUCTURE.md` is the
annotated folder tree.

## Build & test (host)

The host `Makefile` builds + tests the engine with just `g++` + `make` (no cmake needed). CMake is
the canonical *cross-platform* path (see below), but day-to-day work uses the Makefile.

```bash
make check          # the full gate: all test suites + depcheck. Run this before considering work done.
make test           # just the unit-test binary (build/phx_tests)
make determinism    # M7 gate: 11 suites + the rendered frame byte-identical across BOTH scalar tiers
make sanitize       # M7 gate: the full check suite under ASan+UBSan (own build root build/asan)
make release        # M7 gate: the full check suite built+run with PHX_BUILD_RELEASE=1 (own build root build/release)
make clean
make depcheck       # enforce the acyclic module dependency law (also part of `check`)
make version        # print the engine version (single-sourced from phx/core/version.h)
make docs           # Doxygen API reference -> build/docs/html (public headers + docs/; needs doxygen)
make dist           # package the host tools tarball into build/dist/ (dist-win/-gba/-psp for the rest)
```

Versioning/release flow: the version lives ONLY in `engine/core/include/phx/core/version.h`
(Makefile + CMake parse it; `tests/unit/test_version.cpp` locks the composed forms). Releases
are cut by tagging `vX.Y.Z` (checklist in `RELEASING.md`; artifacts built by
`.github/workflows/release.yml`, which rejects a tag that doesn't match the header). User-visible
changes get a line under `[Unreleased]` in `CHANGELOG.md`. `CONTRIBUTING.md` is the public
contributor guide.

`make check` runs many separate suite binaries (`smoke render ppu gu playable physics anim scene ui
platformer emberwing emberwing-ppu audio texcache png sprite tiled resource phxpack pipeline editors tools`,
plus `level`: the engine level loader over baked map/prefabs/sprite, `behaviours`: the stock
behaviours played from prefab data alone, `flow`: the data-driven game flow, `dialogue`: .dlg conversations (the
runtime vs the Studio's simulator, a talk screen, a Talk NPC), `project-check`: the Studio's
new-project template baked and run headlessly on the PC, GBA and PSP profiles, and the example
projects' own `miracle-test` and `tinyllm-test`). Each is its own Make
target that builds and runs one binary — e.g. `make physics`, `make ppu`, `make pipeline`. **To run
a single suite, run its target.** There is no per-test-case filter; the unit harness
(`tests/phx_test.h`) runs every `PHX_TEST` registered in the binary. Expected output:
`PASS <N> checks across <M> cases` and `... PASS` lines per suite, plus `depcheck: OK`.
For numeric/gameplay-affecting changes, run `make determinism` too; run `make sanitize` before
calling large changes done (both are CI jobs).

### Scalar tiers (critical)

`phx::scalar` is `float` on PC/PSP but `fixed16` (Q16.16) on GBA — same source, no FPU on GBA. The
**default host build is the PC/float tier**. To exercise the GBA fixed-point path on the host:

```bash
make test TIER=gba_sim     # defines PHX_TARGET_GBA=1 -> scalar = fixed16
```

Host objects live in **per-(tier,release) directories** (`build/obj-pc-r0/`, `build/obj-gba_sim-r0/`,
...) with a config stamp that relinks binaries on switch — configs coexist and switching never
needs `make clean`. The determinism contract is that both tiers produce identical results; `make
determinism` enforces it as a gate. When adding tier-exact math, use the Q16 integer idiom
(`s_to_q16`/`s_from_q16`, see zoom/parallax in `render/src/renderer.cpp`), not float arithmetic.

### Debug vs. release (`PHX_BUILD_RELEASE`)

`PHX_ASSERT` (programmer-error trap, `phx/core/assert.h`) and the log floor (`phx/core/log.h`)
are gated on `PHX_BUILD_RELEASE` (or the standard `NDEBUG`). **Every GBA/PSP cross build defines
it unconditionally** (Makefile `GBA_FLAGS`/`PSP_FLAGS`; root `CMakeLists.txt`'s console block) —
those ship on real hardware/an emulator, never a dev host, so a debug assert trap is a console
hang, not a debugger breakpoint, and there is no "GBA debug build". Host builds default to debug
(current `make check` behavior); opt into release with `make check RELEASE=1` or the dedicated
`make release` gate (own build root `build/release`, catches e.g. a variable only referenced
inside `PHX_ASSERT` becoming an `-Wunused-variable` warning once the macro compiles away). CMake
host/Windows builds pick it up for free from `-DCMAKE_BUILD_TYPE=Release` (which already defines
`NDEBUG`).

### Desktop windowed / device verification (needs SDL2, a display)

```bash
make sdl / make gl              # build the windowed SW / OpenGL example (opens a window)
make sdl-verify / make gl-verify  # render through real SDL/GL, read back, diff vs software golden
make audio-verify               # open a real SDL audio device, confirm non-silent mixer output
make studio                     # Phosphorus Studio (build/phxstudio): a GAME PROJECT editor (--project DIR); --engine-dev = the whole checkout
make play PROJECT=dir           # build + bake + run a game project (make game / game-assets [TIER=0|1|2] = one step each)
make tmap / make entity         # the Studio's map / table editors as standalone windows (phxtmap / phxentity)
# CMake: -DPHX_USE_SDL=ON also builds phxstudio / phxtmap / phxentity (phxnew always)
```

`PHX_MAX_FRAMES=N ./build/<binary>` gives any windowed binary a bounded, clean-exit smoke run.

### Cross-compiling (needs devkitARM / pspsdk / MinGW-w64)

```bash
make gba-platformer       # devkitARM -> build/gba/phx-platformer.gba (full game, software render)
make gba-platformer-ppu   # the full game on GBA PPU hardware (Mode-0 tiles + OAM)
make gba-emberwing-ppu    # Emberwing as the SHIPPING GBA ROM (native PPU: 4 streamed BGs + OBJ)
make gba-emberwing        # Emberwing on the GBA software-render tier (slow; on-device reference)
make psp / make psp-gu    # pspsdk -> build/psp/EBOOT.PBP (software / sceGu hardware)
make psp-emberwing        # Emberwing as a PSP EBOOT (soft render + sceAudio thread)
make win                  # MinGW-w64 -> build/win/*.exe: EVERY host binary as static PE32+
make win-verify           # run the Windows unit-suite exe under Wine (native or flatpak)
make size-gate            # GBA ROM/IWRAM/EWRAM budget gate (MVP gate; CI job)
make gba-save / psp-save  # console save-path smoke ROM/EBOOT (verify on mGBA / PPSSPP)
make game-gba | game-psp PROJECT=path   # a game project (PHX_GAME, no main()) -> .gba / EBOOT.PBP
make game-export PROJECT=path EXPORT=pc|gba|psp   # -> <project>/dist/<name>-<target>/ + .zip for players
make project-budget PROJECT=path        # run it headlessly as PC/GBA/PSP -> build/budget-*.json (Studio: Budget)
make game-debug PROJECT=path            # play it under gdb: a crash prints every thread's backtrace
```

Game projects (folders with `phxproject.json`) name their Game with `PHX_GAME` (`phx/runtime/main.h`);
the engine owns `main()` per target (`engine/runtime/src/entry/{desktop,gba,psp}_main.cpp`, linked only
by the project rules) and applies each target's profile (GBA: 160 KB arena, 240x160; PSP: 4 MB).
`phx::Level` (`phx/runtime/level.h`) builds entities from a map's spawns + the `prefabs` table (columns
read by name via phxbin's schema trailer, `phx/resource/table.h`); per-spawn properties override columns;
`PHX_COMPONENT` (`phx/ecs/reflect.h`) makes a game's components data-buildable and exportable to the Studio;
the engine's stock behaviours (`phx/runtime/behaviours.h`) are such components, run by one `Behaviours` system.

GBA has no filesystem: `gba-platformer` bakes the `.phxp` bundle on the host (tier 0 — sounds are
resampled to the GBA device rate at bake time) and links it into the ROM with `bin2s`. Canonical
multi-target packaging is CMake:

```bash
cmake -S . -B build/linux   -DPHX_TARGET=linux -DCMAKE_BUILD_TYPE=Release
cmake -S . -B build/gba     -DPHX_TARGET=gba     -DCMAKE_TOOLCHAIN_FILE=cmake/gba.toolchain.cmake
cmake -S . -B build/psp     -DPHX_TARGET=psp     -DCMAKE_TOOLCHAIN_FILE=cmake/psp.toolchain.cmake
cmake -S . -B build/windows -DPHX_TARGET=windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw.toolchain.cmake
```

## Architecture & the rules that keep it portable

The engine is an **acyclic, strictly-layered** dependency graph, enforced at build time by
`tools/common/depcheck.py` (a violation is a build break). Layers (each may depend only on lower
layers), from `depcheck.py`:

- **L0 core** — closed foundation: types, assert, fixed-point, math, caps, pixel, log, config, time, profile
- **L1 memory, platform** — allocators; the C-ABI platform seam
- **L2 render, input, audio, resource, ecs** — services
- **L3 scene, physics, anim, ui** — gameplay systems
- **L4 runtime** — composition root: the App fixed-step main loop

Two inviolable rules (see `STRUCTURE.md`):

1. **Include only another module's `include/phx/<mod>/`, never its `src/`.** Dependencies point
   downward only (core never includes ecs).
2. **One backend per module per build.** Platform/render backends live in `src/<name>/` subfolders;
   `phx_add_module(... BACKENDS ...)` links exactly the one matching `PHX_TARGET`. **There is no
   `#ifdef PLATFORM` in gameplay or systems code, ever** — platform divergence lives only behind the
   C seam in `engine/platform/src/{null,sdl,gba,psp}/` and per-tier render backends in
   `engine/render/src/{soft,gl,gu,gba}/`.

Gameplay/systems code must never include a platform or OS/SDK header. `PHX_TARGET` maps to a
compile-time capability tier and render tier via `cmake/caps_select.cmake` → `phx/core/caps.h`
(tier 0 = GBA PPU, tier 1 = PSP GU, tier 2 = PC GL/VK).

**Two related defines that must not be conflated:** `PHX_TARGET_GBA` selects the *scalar tier*
(fixed16) and is ALSO set by the host `TIER=gba_sim` build; `PHX_GBA_HW` marks *real hardware*
(MMIO/VRAM/OAM paths) and is set only by the actual cross builds (Makefile `GBA_FLAGS`,
`cmake/gba.toolchain.cmake`). Guard hardware register code with `PHX_GBA_HW`, never
`PHX_TARGET_GBA` — otherwise the host gba_sim build tries to link platform hardware symbols.

### Key cross-cutting designs

- **Memory**: one root arena allocated at boot; arena/stack/pool/object allocators on top; **zero
  hot-path heap allocation**. Don't introduce `new`/`malloc` on the frame path. (Known exception:
  platform backends malloc init-time state — framebuffer, desktop file images — outside the arena;
  `phx_platform_desc.root_arena` is null/unused today. Init/load-time only, never per-frame.)
- **Render**: a single 2D-intent API (sprites/tilemaps/parallax/palettes/zoom/shake) compiles to
  GBA PPU, PSP GU, or PC GL. The **software backend (`render/src/soft/`) is the golden reference**
  that hardware backends are diffed against. Cross-tier-exact features (zoom, per-layer parallax)
  are implemented in the front end (`renderer.cpp`) with Q16 integer math so every backend inherits
  them and both scalar tiers produce identical pixels.
- **Assets**: baked offline into `.phxp` bundles, read zero-copy in place from a stable image —
  load-once heap buffer on PC (no OS mmap), linked-in data on GBA/PSP (or LZSS-decompressed
  once), **per-target encoded** (`--target 0|1|2`; e.g. tier 0 resamples sounds to the GBA device
  rate at bake time). Two-stage pipeline (docs/08): per-format **converters** (`phxsprite`/
  `phxtile`/`phxsnd`/`phxbin`) bake author sources into intermediate `.phx*` files, which the
  **`phxpack` assembler** merges (it can also bake sources directly). All share one bake path,
  `tools/phxpack/builders.h`. Sound effects (`.sfx`) and music (`.song`, a pattern tracker) are
  synthesized to PCM at bake time (`tools/phxpack/synth.h`), never at runtime; fonts (`.font` grid
  sheets, BMFont `.fnt`) bake to a glyph table (`tools/phxpack/font.h`) that `phx/runtime/font.h`
  loads into a proportional ui `BitmapFont`. Tools are
  **host-only** (STL allowed); engine code is not.
- **Phosphorus Studio + the GUI editors dogfood the engine** — same App loop / SDL window / soft
  renderer as the games, with the tool widget kit (`tools/common/twk.h`, an immediate-mode GUI over
  `phx::UI` rect/image) on top; no separate UI toolkit. Editor panels (`tools/phxstudio/ed_*.cpp`:
  code, sprite/pixel, tilemap, data table) sit behind a `Host` interface (`host.h`); the Studio
  hosts them in tabs, and `phxtmap` / `phxentity` are the map / table panels in a one-document
  shell (`solo.h`). Tools reach desktop-only input (keys, text, right/middle/wheel, clipboard,
  resize, confirm-quit) through **`phx/platform/desktop.h`**, implemented ONLY by the `sdl`
  backend and a scripted `null` queue — never extend `phx_input_raw`/`phx_platform` for tools.
  Editors **emit author formats the converters bake** (`.png`, `.sprdef`, `.tmj`, phxbin JSON),
  never engine blobs. Every document model is headless and unit-tested (`make editors`, `make
  pipeline`), and each saved form is re-read by the bake's own loaders there. **Project mode is
  a hard boundary**: with a `phxproject.json` project open, the Studio may WRITE only inside that
  folder and READ only the engine's public headers (`engine/*/include`) + docs. Every open, save and
  New-file target goes through `Host::access()` (`projectdoc.h: AccessPolicy`, tested in `make
  editors`), so a new code path that loads a file must check it too. `--engine-dev` lifts it. Every
  tool folder has an `instructions.md` with usage, formats, and controls. The soft renderer
  alpha-TESTS (no blending) and sorts sprites by (layer, z, texture) — see
  `tools/phxstudio/instructions.md` for the consequences (stippled overlays, one sub-layer per
  stacked texture). The Studio's text is JetBrains Mono, rasterized with stb_truetype and drawn at
  window resolution through the desktop seam's overlay (`phx_desktop_overlay_begin`) on the same
  6px/10px canvas grid as the 5x7 bitmap font it falls back to (`tools/common/instructions.md`).
- **The platformer** (`examples/platformer/`) is the MVP gate: a full game slice using **only**
  engine systems. Nothing gameplay-relevant is hardcoded — level/spawns come from a Tiled map
  (incl. per-layer parallax factors), hero anim from a Sprite asset, SFX from baked WAVs.
  Map convention: the **last tile layer is the gameplay/solid layer** (physics reads it; earlier
  layers are parallax backdrops). Select toggles the frame-profiler overlay in-game.

## Conventions

- C++17, `-Wall -Wextra -Wpedantic`, **zero warnings** is the standing bar — including under
  clang/MinGW (`make win`) and ASan+UBSan (`make sanitize`; left-shifting negatives is UB — use
  multiplication, see `fixed.h`).
- Engine modules follow an identical shape: public `include/phx/<mod>/` + private `src/` (with
  optional per-backend subfolders).
- Tests use the tiny in-house harness in `tests/phx_test.h` (`PHX_TEST(name)`, `CHECK*` macros) — no
  GoogleTest. `tests/` is foldered by kind and **the naming convention is load-bearing** (see
  `tests/README.md`): `tests/unit/test_*.cpp` are unit files with **no `main()`**, all linked into
  the one `build/phx_tests` binary; `tests/suites/*_test.cpp` are integration binaries that each
  have **their own `main()`** (one per `make` suite target); `tests/verify/*_verify.cpp` are the
  real-device (SDL/GL/audio) verifiers; `tests/fixtures/` is shared test data. New unit tests
  self-register — add the `.cpp` to `TEST_SRC` in the `Makefile` *and* the `phx_tests` list in
  `tests/CMakeLists.txt`; a new suite also needs a Make target, an entry on `check:`, and an
  `_itests` entry in `tests/CMakeLists.txt`. Test fixtures that carve from static pools must
  bound-check them (`std::abort()` on overflow) — ASan caught silent overflows from unchecked ones.
- The **null platform's virtual clock** advances one sim step per `pump_events()` (frame) plus 1 µs
  per `clock_ns()` read — frame pacing is independent of how often the loop reads the clock. Tests
  asserting exact fixed-step counts rely on this; don't switch it back to per-read stepping.
- Console gotchas that already bit once: a function-local `static` with a runtime initializer
  deadlocks on GBA because GCC's `__cxa_guard_acquire` hits devkitARM's newlib lock stubs — hence
  `-fno-threadsafe-statics` in the cross-build flags (see the comment above `GBA_FLAGS` in the
  `Makefile`); and PSP `sceIoOpen` on a relative save path fails with `SCE_KERNEL_ERROR_NOCWD` under
  PPSSPP, so a bare save key is anchored at `ms0:/PSP/SAVEDATA/PHX/` (`engine/platform/src/psp/psp_platform.cpp`).
