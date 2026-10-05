# Contributing to Phosphorus

Thanks for your interest in Phosphorus (`phx`)! This document covers everything you need to build
the engine, run its gates, and land a change. Design background lives in the numbered docs under
[`docs/`](docs/) (start with [`docs/00-architecture.md`](docs/00-architecture.md));
[`STRUCTURE.md`](STRUCTURE.md) is the annotated folder tree.

## Prerequisites

A host build needs only **`g++` (C++17) + GNU `make` + `python3`** — no CMake, no third-party
libraries. Optional, depending on what you touch:

| For | You need |
|---|---|
| Windowed examples, GUI editors, device verify | SDL2 (`libsdl2-dev`) |
| Windows cross build (`make win`) | MinGW-w64 (`g++-mingw-w64-x86-64`), Wine to run it |
| GBA ROMs (`make gba-*`) | devkitPro/devkitARM |
| PSP EBOOTs (`make psp-*`) | pspsdk (pspdev) |
| The multi-target CMake tree, install/packaging | CMake ≥ 3.20 |

## Build and test

```bash
make check          # THE gate: every suite + the dependency-graph check. Green before any PR.
make test           # just the unit binary (build/phx_tests)
make physics        # ...or any single suite by its target name (see `check:` in the Makefile)
```

Before calling substantial work done, also run the release gates CI enforces:

```bash
make determinism    # same results + byte-identical frame on the float AND fixed-point tiers
make sanitize       # the full suite under ASan+UBSan
make release        # the full suite with PHX_BUILD_RELEASE=1 (asserts compiled out)
```

Run `make determinism` for anything numeric or gameplay-affecting, and `make sanitize` before
large changes land. If you touched shared engine code or a backend, cross-compile the console
targets too (`make gba-platformer-ppu`, `make psp-platformer`, `make win`) — `make check` does
not cover them, and GBA (ARM7TDMI, no FPU, tight ROM budget) breaks first.

### The two scalar tiers

`phx::scalar` is `float` on PC/PSP but `fixed16` (Q16.16) on GBA — same source. The default host
build is the float tier; exercise the fixed-point tier with:

```bash
make test TIER=gba_sim
```

Both tiers must produce **identical** results (`make determinism` is the gate). For tier-exact
math, use the Q16 integer idiom (`s_to_q16`/`s_from_q16` — see zoom/parallax in
`engine/render/src/renderer.cpp`), never float arithmetic.

## The rules that keep it portable

These are enforced or gated, not aspirational:

1. **Acyclic, strictly layered modules** — core → memory/platform → render/input/audio/
   resource/ecs → scene/physics/anim/ui → runtime. `tools/common/depcheck.py` breaks the build
   on a violation (`make depcheck`, part of `check`).
2. **Include only another module's `include/phx/<mod>/`, never its `src/`.**
3. **No `#ifdef PLATFORM` in gameplay or systems code, ever.** Platform divergence lives behind
   the C seam in `engine/platform/src/{null,sdl,gba,psp}/` and the per-tier render backends.
   Gameplay code never includes an OS/SDK header.
4. **One backend per module per build** — the build system links exactly one.
5. **Zero hot-path heap allocation.** One root arena at boot; allocators on top. No
   `new`/`malloc` on the frame path.
6. **`PHX_TARGET_GBA` ≠ `PHX_GBA_HW`.** The former selects the fixed-point scalar tier (also set
   by the host `TIER=gba_sim` build); the latter marks real hardware (MMIO/VRAM/OAM). Guard
   register code with `PHX_GBA_HW` or the host sim build fails to link.
7. **Zero warnings** at `-Wall -Wextra -Wpedantic` — including MinGW (`make win`) and
   ASan+UBSan (`make sanitize`). Engine code is exception-free and STL-free; host **tools**
   (`tools/`) may use the STL.

## Tests

The in-house harness is `tests/phx_test.h` (`PHX_TEST(name)` + `CHECK*` macros). The folder
naming is load-bearing (see [`tests/README.md`](tests/README.md)):

- `tests/unit/test_*.cpp` — unit files, **no `main()`**, all linked into one `build/phx_tests`
  binary. Register new files in `TEST_SRC` in the `Makefile` **and** the `phx_tests` list in
  `tests/CMakeLists.txt`.
- `tests/suites/*_test.cpp` — integration binaries, each with its own `main()` and Make target.
  A new suite needs a Make target, an entry on `check:`, and an `_itests` entry in
  `tests/CMakeLists.txt`.
- `tests/verify/*_verify.cpp` — real-device (SDL/GL/audio) verifiers, not part of `check`.

New engine behavior comes with tests; a bug fix comes with the test that would have caught it.

## Pull requests

- Keep PRs focused — one logical change. Match the surrounding code's style and comment density.
- `make check` green is the floor; CI additionally runs `determinism`, `sanitize`, `release`,
  the MinGW/Wine build, and the GBA/PSP cross builds with ROM size gates.
- Don't bump the version in PRs — versions are bumped by the release process (see
  [`RELEASING.md`](RELEASING.md)), and user-visible changes get a line under `[Unreleased]` in
  [`CHANGELOG.md`](CHANGELOG.md).

## Versioning and releases

The engine version lives in one place: `engine/core/include/phx/core/version.h`. Releases are
cut by tagging `vX.Y.Z`, which builds and attaches every platform artifact automatically —
the full checklist is in [`RELEASING.md`](RELEASING.md).
