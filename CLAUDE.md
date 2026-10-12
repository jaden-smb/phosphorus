# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

@AGENTS.md

## Orientation (beyond AGENTS.md)

- **Where to start:** `docs/00-architecture.md` (seams, risk register), then the numbered doc for the
  module you touch (`01-core` … `10-gameplay-systems`); `STRUCTURE.md` is the per-file annotated tree.
  Every `tools/*/` folder has an `instructions.md` — read it before changing that tool.
- **Examples are verification targets, not samples:** `platformer` (the MVP gate), `emberwing` (the
  shipping GBA/PSP slice), `tinyllm` (float-free LLM inference on a GBA, bit-exact vs a Python
  mirror), `miracle-player` (music visualizer; `tools/phxviz` bakes the FFT track offline). The
  `gba_*` / `psp_*` dirs are one-concern console bring-up smokes. Gameplay-affecting engine changes
  usually need the platformer/emberwing suites green on **both** scalar tiers.
- **Choosing the gate:** touched gameplay/numeric code → `make check` + `make determinism`; touched a
  GBA/PSP backend → add `make size-gate` / the matching cross target if the SDK is installed; large
  or memory-sensitive change → `make sanitize`. CI (`.github/workflows/ci.yml`) runs host, cmake-studio,
  sanitize, release, windows (Wine), gba-size and psp jobs.
- **Adding a test/suite:** follow the `tests/README.md` naming rules (`test_*.cpp` has no `main()`,
  `*_test.cpp` does) and register in both `Makefile` and `tests/CMakeLists.txt`.
- **Host-only tooling may use the STL; engine modules may not** allocate on the frame path or include
  platform/SDK headers — check `make depcheck` after adding any `#include` across modules.
- User-visible changes get a line under `[Unreleased]` in `CHANGELOG.md`.

`STATUS.md` (local only, gitignored) is the detailed running engineering log (numbered entries);
consult it for context on past hardware-verification work and gotchas, e.g. mGBA/PPSSPP
verification recipes. Never commit it or cite it from a tracked file.
