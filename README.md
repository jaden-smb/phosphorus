<div align="center">

# Phosphorus Engine (`phx`)

**A lightweight, modular, retro-inspired 2D/2.5D game engine.**
One codebase → **Game Boy Advance · PSP · Windows · Linux**.

*From-scratch C++17. No dependencies on the game path. Built for homebrew, indie work,
and low-resource targets — prioritizing portability, predictable performance, and
readable code over feature count.*

</div>

---

## What it is

Phosphorus runs the **same gameplay code** on a 256 KB Game Boy Advance and a multi-gigabyte PC by
designing around the ~17,000× RAM gap instead of ignoring it. Gameplay code never includes a
platform header; everything machine-specific lives behind one C-ABI seam and a per-target
render backend.

```
        GAME  ─────────────────────────────────────────────────────┐
        ├─ ecs · scene · physics · anim · ui · resource (systems)   │
        ├─ core (loop · memory · log · config · time · profile)     │  portable C++17
        ╞═════════════════════ C-ABI seam ═════════════════════════╡
        ├─ platform: sdl (win/linux) │ gba (mmio) │ psp (sceXxx)    │  one backend linked
        └───────────────────────────────────────────────────────────┘
```

| | |
|---|---|
| **Math** | `phx::scalar` is `float` on PC/PSP and `fixed16` (Q16.16) on GBA — same source, no FPU required. Both tiers are held **byte-identical** by a determinism gate. |
| **Memory** | One root arena at boot; arena/stack/pool/object allocators on top. **Zero hot-path heap allocation**; fragmentation is structurally impossible. |
| **Render** | One 2D-intent API (sprites · tilemaps · parallax · zoom · shake · palettes) compiling to **GBA PPU**, **PSP GU**, and **PC OpenGL**, all diffed against a software golden reference. |
| **ECS** | Sparse-set, bounded, cache-friendly: 512 entities on GBA → 65,536 on PC. |
| **Assets** | Baked offline into `.phxp` bundles, read zero-copy in place (load-once image / ROM pointer) or LZSS-decompressed once, and **encoded per target** (tier 0 resamples audio to the GBA's rate at bake time). |
| **Tools** | CLI converters + the `phxpack` assembler + two GUI editors **built on the engine itself**. Editors emit open author formats (Tiled `.tmj`, JSON), never engine blobs. |
| **Build** | One CMake tree, or a plain `Makefile` needing only `g++` + `make` → ELF · `.exe` · `.gba` ROM · PSP `EBOOT.PBP`. |

## The game

**[Emberwing — Cinder Hollow](examples/emberwing/)** is the engine's proof: a complete,
original pixel-art platformer level — 320×20 tiles, three enemy types, geysers, lava,
checkpoints, chiptune score — shipping from *one* source tree as a native GBA ROM (PPU
hardware: four streamed backgrounds + OBJs), a PSP EBOOT, a Windows exe, and a Linux binary.
All of its art, audio and level data is authored on the host and baked through the same
pipeline the CLI tools use; the gameplay code contains no platform `#ifdef` and no float
literals.

```bash
make emberwing-sdl        # play it in a window (software renderer + real audio device)
make emberwing            # the same playthrough, headless + verified (runs in `make check`)
make gba-emberwing-ppu    # devkitARM -> the shipping GBA ROM
make psp-emberwing        # pspsdk    -> build/psp/emberwing/EBOOT.PBP
```

`examples/platformer/` is the smaller reference game (the MVP gate): data-driven level, spawns,
animation and SFX, save/load, enemies — also on all four targets.

## Quick start

Requires only `g++` (C++17) and `make`:

```bash
make check                # THE gate: unit + integration suites + asset pipeline + dependency law
make test TIER=gba_sim    # the same suite on the GBA fixed-point tier (scalar = fixed16)
make determinism          # prove both scalar tiers render byte-identical frames
make sanitize             # the full suite under ASan + UBSan
```

Expected: `PASS 120336 checks across 101 cases`, a `... PASS` line per suite, and `depcheck: OK`.

With SDL2 (and libGL) you also get windowed play and the desktop verifiers — `make sdl`, `make
gl`, `make sdl-verify`, `make gl-verify`, `make audio-verify` — plus **Phosphorus Studio**, `make
studio && ./build/phxstudio`: the editor for the engine, built on the engine. It has a code editor, a
sprite/pixel editor (GBA colour checks, frames, animated clips), a tilemap editor (real tileset art,
collision, spawns, parallax) and a data-table editor. It also shows the module graph and capability
tiers, previews every baked asset as each console would render it, and runs every game, gate and
suite with a click ([instructions](tools/phxstudio/instructions.md)). It works on **game projects**
(a folder with a `phxproject.json`; File > New project makes a playable one). A project can edit only
its own folder and read the engine's public API, and it builds with `make play PROJECT=path`.
`--engine-dev` opens the engine itself. The map and table editors also ship standalone:
`make tmap`, `make entity`. Every suite is its own target (`make physics`,
`make ppu`, `make resource`, …); see the [Makefile](Makefile), and the `instructions.md` in each
tool folder. With doxygen installed, `make docs` generates the browsable API reference (public
headers + this manual) into `build/docs/html`.

## Cross-compiling

With devkitARM / pspsdk / MinGW-w64 installed, the host `Makefile` cross-compiles the **same
engine** — no CMake needed:

```bash
make gba-emberwing-ppu / gba-platformer-ppu   # devkitARM -> .gba ROM (native PPU)
make psp-emberwing     / psp-gu               # pspsdk    -> EBOOT.PBP (native sceGu)
make win                                      # MinGW-w64 -> every host binary as a static .exe
make size-gate                                # GBA ROM / IWRAM / EWRAM budget gate
```

GBA has no filesystem, so the `.phxp` bundle is baked on the host and linked into the ROM. CMake
remains the canonical multi-target packaging path:

```bash
cmake -S . -B build/linux   -DPHX_TARGET=linux   -DCMAKE_BUILD_TYPE=Release
cmake -S . -B build/gba     -DPHX_TARGET=gba     -DCMAKE_TOOLCHAIN_FILE=cmake/gba.toolchain.cmake
cmake -S . -B build/psp     -DPHX_TARGET=psp     -DCMAKE_TOOLCHAIN_FILE=cmake/psp.toolchain.cmake
cmake -S . -B build/windows -DPHX_TARGET=windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw.toolchain.cmake
```

## The rules that keep it portable

The engine is an **acyclic, strictly-layered** dependency graph, enforced at build time by
`tools/common/depcheck.py` — a violation is a build break.

**L0 core** (types, math, fixed-point, caps, log, config, time) → **L1** memory, platform →
**L2** render, input, audio, resource, ecs → **L3** scene, physics, anim, ui → **L4** runtime
(the fixed-step App loop). Each layer may depend only on lower ones.

1. Include only another module's `include/phx/<mod>/`, never its `src/`.
2. **One backend per module per build.** There is no `#ifdef PLATFORM` in gameplay or systems
   code, ever — platform divergence lives only in `engine/platform/src/{null,sdl,gba,psp}/` and
   `engine/render/src/{soft,gl,gu,gba}/`.

## Repository layout

```
phosphorus/
├── docs/       ← the technical design documentation (start at 00)
├── engine/     ← the engine modules (core, memory, platform, render, ecs, ...)
├── tools/      ← host-only asset pipeline + GUI editors (instructions.md in each)
├── examples/   ← Emberwing (the game) + the reference platformer + per-target smokes
├── tests/      ← unit · integration · device-verify suites (see tests/README.md)
└── cmake/      ← toolchains (gba, psp, mingw) + module helpers
```

## Documentation

| Doc | | Doc | |
|-----|---|-----|---|
| [00](docs/00-architecture.md) | **Master architecture** — seams, cross-cutting decisions | [06](docs/06-resources.md) | Resources — `.phxp` format, caching, streaming |
| [01](docs/01-core.md) | Core — app loop, memory root, log, config, math | [07](docs/07-build-system.md) | Build system — CMake, toolchains, four targets |
| [02](docs/02-platform-layer.md) | Platform layer — the C seam + backends | [08](docs/08-tooling.md) | Tooling — phxpack, converters, editors |
| [03](docs/03-rendering.md) | Rendering — unified API, GL/GU/PPU backends | [09](docs/09-roadmap.md) | Roadmap — milestones and future scope |
| [04](docs/04-ecs.md) | ECS — sparse-set design and tradeoffs | [10](docs/10-gameplay-systems.md) | Gameplay systems — input, audio, scene, physics, anim, UI |
| [05](docs/05-memory.md) | Memory — the four allocators | — | [STRUCTURE.md](STRUCTURE.md) — annotated folder tree |

## Status

Every seam of the design is implemented and **verified at runtime on a real or emulated target**:
render (software golden · OpenGL on a real GPU · GBA PPU, VRAM/OAM-verified on mGBA · PSP GU,
pixel-verified on PPSSPP), audio output (SDL device · GBA Direct Sound · PSP sceAudio), and
persistent saves (host file · GBA battery SRAM · PSP `sceIo`). The Windows build runs its complete
test suite and the game under Wine.

CI holds five gates: `check` · `determinism` (both scalar tiers byte-identical) · `sanitize`
(ASan+UBSan) · the GBA ROM size budget · the Windows suite under Wine.

**Not yet done:** validation on physical hardware (a real GBA cart, including SRAM persistence
across power cycles, which emulators can't prove — and a physical PSP). Post-1.0 scope — 2.5D,
tracker music, particles, a job system, scripting, DS/Vita/Android — is in the
[roadmap](docs/09-roadmap.md).

## Releases & contributing

Releases are tagged `vX.Y.Z` and ship prebuilt artifacts on the
[releases page](https://github.com/jaden-smb/phoenix/releases): the asset-pipeline CLIs for
Linux/Windows, a CMake SDK (`find_package(phosphorus)`), and ready-to-run GBA ROMs + PSP EBOOTs.
The same bundles build locally via `make dist` / `dist-win` / `dist-gba` / `dist-psp`. The
version is single-sourced from [`phx/core/version.h`](engine/core/include/phx/core/version.h);
history is in [CHANGELOG.md](CHANGELOG.md).

Contributions welcome — [CONTRIBUTING.md](CONTRIBUTING.md) covers the build, the test gates,
and the portability rules; [RELEASING.md](RELEASING.md) covers cutting a release.

## License

[MIT](LICENSE).
