# Phosphorus Engine — Master Architecture Document

> **Document status:** Initial Technical Design (TDD v0.1)
> **Audience:** Engine engineers, gameplay programmers, tools developers, porters
> **Scope:** Whole-engine architecture, cross-cutting decisions, and the contracts
> that every module must honor.

---

## 1. Executive Summary

Phosphorus Engine (codename **`phx`**) is a lightweight, modular, data-oriented game
engine for **2D and 2.5D** games targeting four wildly different machines from a
single codebase:

| Platform | CPU                       | RAM (usable)        | GPU model            | Toolchain          |
|----------|---------------------------|---------------------|----------------------|--------------------|
| GBA      | ARM7TDMI @ 16.78 MHz      | 256 KB + 32 KB IWRAM| Tile/sprite PPU      | devkitARM          |
| PSP      | MIPS Allegrex @ 222–333 MHz| 32 MB (24 MB usable)| Graphics Engine (GU)| pspsdk / devkitPSP |
| Linux    | x86-64 / ARM64            | GBs                 | OpenGL 1.1¹          | GCC / Clang        |
| Windows  | x86-64                    | GBs                 | OpenGL 1.1¹          | MSVC / Clang / GCC |

¹ The PC tier's design is deliberate maximum compatibility: immediate-mode
`glBegin`/`glVertex` against a GL 1.1 context, with the software rasterizer staying the
golden reference every backend is diffed against. A GL 3.3 core / optional-Vulkan backend
was an early sketch and is **dropped as a non-goal** (docs/03 §6 records the decision):
2D scenes this size never make PC draw-call count the bottleneck, and a second PC GPU
path would widen the maintenance matrix for the one tier with performance to spare.

The central engineering tension is the **17,000× RAM gap** between the GBA and a
modern PC. Phosphorus resolves this not by lowest-common-denominator design, but by a
**capability-tiered architecture**: a small mandatory core that runs everywhere,
and feature modules that scale their backends and budgets to the host. The public
API is identical on every platform; only the implementation and the *configured
budgets* differ.

### Design pillars (in priority order)

1. **Portability** — gameplay code never `#include`s a platform header.
2. **Performance under constraint** — predictable frame time, no hidden allocations.
3. **Simplicity** — a programmer can read any single module in one sitting.
4. **Extensibility** — new platforms and systems plug in without core edits.
5. **Feature count** — explicitly *last*. We ship a sharp small toolset, not a
   bloated one.

---

## 2. Architectural Style

Phosphorus is a **layered, data-oriented engine** with a **stable C-ABI platform
seam** and a **C++17 systems layer** above it.

```
        ┌─────────────────────────────────────────────────────────────┐
        │                        GAME / EXAMPLES                        │
        │      (platformer, uses ONLY phx public headers)               │
        ╞═════════════════════════════════════════════════════════════╡
        │                       PHX SYSTEMS LAYER  (C++17)              │
        │  ecs · scene · physics · anim · ui · resource · render(hi)    │
        ╞═════════════════════════════════════════════════════════════╡
        │                       PHX CORE  (C++17, freestanding-ish)     │
        │  memory allocators · log · config · time · math · profile     │
        ╞═══════════════════════════ C ABI SEAM ══════════════════════╡
        │                    PLATFORM LAYER  (C interface)              │
        │  window · gfx · input+pointer · audio · file · save · clock   │
        ├──────────┬──────────┬───────────────┬───────────────────────┤
        │  null    │  sdl     │  gba (mmio)   │  psp (sceXxx)           │
        └──────────┴──────────┴───────────────┴───────────────────────┘
```

(The App fixed-step loop itself lives one layer **above** the systems, in the
`runtime` module — bundling it into core would create a module cycle, since the
loop needs memory+platform while both depend on core's types. Core stays a
closed, zero-out-edge foundation; `depcheck` enforces it.)

**Why a C-ABI seam and not C++ polymorphism at the bottom?**
- C++ vtables are fine on PC/PSP but the GBA backend is essentially register
  pokes and DMA; a flat C struct-of-function-pointers (`phx_platform`) is cheaper,
  trivially `link-time` selectable, and keeps the GBA build free of exceptions/RTTI.
- A single translation unit per platform implements the seam. The linker, driven
  by CMake, picks exactly one. There is **no runtime platform branching** in
  hot paths.

**Why data-oriented above the seam?**
- Cache behavior dominates on PSP; cache *size* dominates on GBA. Structuring game
  state as tightly packed arrays (SoA where it pays) gives us deterministic memory
  layout we can fit into IWRAM/EWRAM budgets and stream predictably.

---

## 3. Module Map & Dependency Rules

Each layer may depend only on layers **below** it (this is the exact layering
`tools/common/depcheck.py` enforces):

```
    L4  ┌─────────────────────────────────────────────┐
        │                  runtime                     │  composition root: the App loop
    L3  ├─────────────────────────────────────────────┤
        │      scene · physics · anim · ui             │  gameplay systems
    L2  ├─────────────────────────────────────────────┤
        │  render · input · audio · resource · ecs     │  services
    L1  ├─────────────────────────────────────────────┤
        │            memory · platform                 │  allocators; the C-ABI seam
    L0  ├─────────────────────────────────────────────┤
        │                   core                       │  CLOSED foundation (zero out-edges)
        └─────────────────────────────────────────────┘
```

### The dependency law (enforced, not aspirational)

1. **Acyclic.** Dependencies point downward only. `core` is a **closed foundation**
   with zero outgoing module edges — it may not include anything, and nothing below
   it exists. (This is why the App loop lives in `runtime`, not core.)
2. **`platform` depends only on `core`'s types** (`phx/core/types.h`, itself
   dependency-free); OS/SDK headers appear nowhere else.
3. **No module includes a sibling's `src/`** — only its public `include/phx/<mod>/`.
4. **Gameplay systems** (scene/physics/anim/ui) may depend on the L2 services;
   they may **not** depend on each other except through the ECS data.
5. Enforced in CI by `tools/common/depcheck.py`, which parses the include graph
   and fails the build on a back-edge or a layering violation.

A dependency violation is a **build break**, treated like a compile error.

---

## 4. Capability Tiers

Rather than `#ifdef PLATFORM` scattered through gameplay, every platform declares a
**capability descriptor** at compile time. Modules read it to size buffers and
select code paths.

```c
// phx/core/caps.h  — pure compile-time constants, one header per platform tier
typedef struct phx_caps {
    uint32_t total_main_ram;     // bytes the engine may claim
    uint32_t scratch_ram;        // fast/near memory (IWRAM on GBA), 0 if N/A
    uint16_t max_entities;       // ECS hard ceiling for this tier
    uint16_t max_sprites;        // per-frame sprite ceiling (128 on GBA OAM!)
    uint8_t  has_float_hw;       // 0 on GBA (software fixed-point), 1 elsewhere
    uint8_t  has_filesystem;     // 0 on GBA (ROM-mapped), 1 elsewhere
    uint8_t  render_tier;        // 0=tile/sprite PPU, 1=fixed GU, 2=programmable
    uint8_t  audio_channels;     // mixer voice count
} phx_caps;
```

| Capability       | GBA            | PSP        | PC (GL)   |
|------------------|----------------|------------|-----------|
| `render_tier`    | 0 (PPU)        | 1 (GU)     | 2 (shaders)|
| `has_float_hw`   | 0 (Q16.16 fixed)| 1         | 1         |
| `max_entities`   | 512            | 8192       | 65536     |
| `max_sprites`    | 128 (HW OAM)   | 1024       | unbounded |
| `has_filesystem` | 0 (ROM)        | 1 (UMD/MS) | 1         |
| `audio_channels` | 2 (DirectSound)| 8          | 32        |

Gameplay code that wants "the player sprite" works identically everywhere; code that
wants "all 2000 particles" reads `phx_caps::max_sprites` and clamps. This makes the
GBA constraints *visible in source* instead of discovered at runtime.

---

## 5. The Math & Fixed-Point Decision

The GBA has **no FPU**. The PSP FPU is fast but the VFPU is the real win. PCs have
SSE. We refuse to pay software-float cost on GBA while leaving PSP/PC performance on
the table.

**Decision:** Phosphorus math is built on a `scalar` typedef selected per tier:

```cpp
// phx/core/math.h
#if PHX_CAPS_HAS_FLOAT_HW
    using scalar = float;
#else
    using scalar = fixed16;   // Q16.16, defined in phx/core/fixed.h
#endif
```

`fixed16` is a value type with overloaded operators that compile to ARM `MUL`/shift
sequences. All engine math (`vec2`, `mat3`, `aabb`) is templated/typedef'd over
`scalar`, so a single `vec2` codebase serves both. Gameplay uses `phx::scalar`,
never raw `float`, so a game written on PC compiles unchanged for GBA.

> **Risk:** silent precision divergence between fixed and float builds causing
> different gameplay (e.g., jump heights). **Mitigation:** a determinism test suite
> (`make determinism`) runs the same suites on both scalar types and asserts
> bounded divergence; physics tuning constants live in a fixed-friendly range.

See `docs/01-core.md` §Math for the full fixed-point design.

---

## 6. Memory Philosophy

> **The single most important rule in Phosphorus: after `phx_init()` returns, the
> engine performs zero general-purpose heap allocations on the hot path.**

There is no `new`/`malloc` in per-frame code. Instead:

- One **root arena** is carved from `phx_caps::total_main_ram` at boot.
- Subsystems receive **sub-allocators** (pools, stacks, arenas) sliced from the root.
- Per-frame transient data uses a **double-buffered frame stack** that is reset
  (pointer bump to zero) every frame — free is O(1) and fragmentation is impossible.

One honest exception today: the **platform seam** sits outside the arena. Backends
`malloc` their init-time state (the software framebuffer at `init()`, desktop file
images at mount) — load/init-time only, never per-frame, but not counted by the root
arena. `phx_platform_desc.root_arena` exists to close this gap and is currently
passed as null and unused.

```
 Root Arena (e.g. 24 MB on PSP, 224 KB on GBA EWRAM)
 ┌───────────────────────────────────────────────────────────────┐
 │ persistent pool │ resource cache │ ecs pools │ frame stack A/B  │
 │  (lifetime=app) │  (LRU evict)   │ (typed)   │ (reset each frame)│
 └───────────────────────────────────────────────────────────────┘
```

Allocator catalog (full design in `docs/05-memory.md`):

| Allocator        | Use case                              | Free cost | Order |
|------------------|---------------------------------------|-----------|-------|
| `ArenaAllocator` | linear bump, freed as a whole         | O(1) bulk | any   |
| `StackAllocator` | scoped LIFO (frame/scene scratch)     | O(1)      | LIFO  |
| `PoolAllocator`  | fixed-size objects (entities, nodes)  | O(1)      | any   |
| `ObjectPool<T>`  | typed recycling of game objects       | O(1)      | any   |

This is the classic 1990s-console discipline (think Naughty Dog's PS1 arenas) and is
what lets the *same* gameplay code fit a 256 KB GBA and a 24 MB PSP.

---

## 7. The Main Loop & Time

A fixed-timestep simulation with a decoupled, capped render. Identical on all
platforms; the platform layer only supplies `vsync` and a monotonic clock.

```cpp
// simplified — full version in engine/core/src/app.cpp
void phx::App::run() {
    const scalar dt = scalar(1) / cfg.sim_hz;        // e.g. 1/60
    uint64_t accumulator = 0;
    uint64_t prev = plat->clock_ns();

    while (!quit_) {
        uint64_t now = plat->clock_ns();
        accumulator += min(now - prev, kMaxFrameNs);  // clamp spiral-of-death
        prev = now;

        input_.poll(plat);                            // edge/held state snapshot

        while (accumulator >= sim_step_ns_) {         // fixed-step sim
            for (System* s : sim_systems_) s->tick(world_, dt);
            accumulator -= sim_step_ns_;
        }

        scalar alpha = scalar(accumulator) / scalar(sim_step_ns_);
        render_.draw(world_, alpha);                  // interpolated present
        frame_stack_.reset();                         // O(1) transient reclaim
        profiler_.mark_frame();
    }
}
```

- **Fixed sim step** → deterministic physics, replay-able input, stable netcode later.
- **Render interpolation (`alpha`)** → smooth motion on high-refresh PC without
  changing sim rate; on GBA the renderer ignores `alpha` (sim==display==60 Hz).
- **Spiral-of-death clamp** → a stalled frame never causes an unbounded catch-up.

---

## 8. Build Topology (one codebase → four binaries)

```
                        CMake (single source tree)
                                  │
        ┌─────────────┬───────────┼────────────┬──────────────┐
        ▼             ▼           ▼             ▼              ▼
  toolchain:      toolchain:   toolchain:    toolchain:   (host tools:
  host gcc/clang  host msvc    gba.cmake     psp.cmake     always host)
        │             │           │             │              │
        ▼             ▼           ▼             ▼              ▼
   linux ELF     win .exe     .gba ROM      EBOOT.PBP      phxpack, etc.
   + SDL2 plat   + SDL2 plat  + devkitARM   + pspsdk      (asset pipeline)
```

Each platform is one CMake **toolchain file** plus one `platform/src/<plat>/`
implementation of the C seam. The systems and gameplay code are compiled verbatim
for all four. Full strategy in `docs/07-build-system.md`.

---

## 9. Asset Pipeline (offline → packed → read in place)

Assets are **never parsed at runtime** in their authoring format. The host tools
convert PNG/WAV/JSON/Tiled `.tmj` into a packed binary that the engine loads once
into a stable heap image (PC) or links into the program image (GBA ROM, PSP EBOOT). (No XML input anywhere — Tiled maps are read as `.tmj`,
its JSON export, not `.tmx`.)

```
 author/         host tools (tools/)            runtime (engine/resource)
 ┌──────┐  phxsprite  ┌──────────┐  phxpack   ┌────────────┐  load in place
 │ .png │ ─────────▶  │ .phxspr  │ ─────────▶ │ assets.phxp│ ─────────────▶ zero-copy view
 │ .wav │  phxsnd     │ .phxsnd  │  (bundle)  │ (resident) │   (no parse)
 │ .tmj │  phxtile    │ .phxtmap │            └────────────┘
 │ .json│  phxbin     │ .phxbin  │
 └──────┘             └──────────┘
```

Bundle format `.phxp`: header → TOC (hashed names → offset/size/type) → blobs. Textures
and sounds are **per-target encoded at bake time**: `--target 0` (GBA) bakes textures as
4bpp paletted 8×8 tiles + palettes (the PPU's native layout; upload just claims palette
banks) and resamples sounds to the GBA's 18157 Hz device rate; `--target 1` (PSP) bakes
textures in the GU's swizzled block order (sampled zero-copy with the swizzle bit set);
`--target 2` (PC) keeps plain RGBA8/PCM. Art a tier can't express (e.g. a non-8px-aligned
texture on tier 0) honestly stays RGBA8 and the GBA backend falls back to its upload-time
quantizer. The TOC layout is identical across targets so the resource API is identical.
Full spec in `docs/06-resources.md` and `docs/08-tooling.md`.

---

## 10. Cross-Cutting Concerns

| Concern        | Decision                                                                 |
|----------------|--------------------------------------------------------------------------|
| Error handling | **No C++ exceptions** (off on GBA/PSP). Fallible APIs return `phx::Result`/status codes; programmer errors `PHX_ASSERT` (compiled out in release). |
| RTTI           | Disabled engine-wide. Type identity via compile-time `TypeId<T>()`.      |
| STL            | Allowed on PC for tools; engine runtime uses `phx::` containers backed by our allocators. No `std::vector` in hot paths on console. |
| Strings        | Asset/runtime keys are 32-bit FNV-1a hashes, not `std::string`. Human strings only in tools/UI text. |
| Threading      | Engine is single-threaded by contract (GBA/early PSP). Optional job system is a *later* PC/PSP-only module; gameplay must not assume it. |
| Logging        | `PHX_LOG_*` macros; sink is platform-provided (stdout / mGBA console / `pspDebugScreen`). Compiled to no-ops below a per-build level. |
| Endianness     | All targets little-endian; pack format stores LE; a guard asserts at load. |

---

## 11. Risk Register (engine-level)

| # | Risk                                              | Likelihood | Impact | Mitigation                                                                 |
|---|---------------------------------------------------|-----------|--------|---------------------------------------------------------------------------|
| R1| GBA RAM ceiling makes a real game impossible      | Med       | High   | Capability tiers + strict budgets enforced at boot; example game proves a vertical slice fits. |
| R2| Fixed/float gameplay divergence                   | Med       | Med    | Determinism test suite; `phx::scalar` everywhere; tuned constants.        |
| R3| Four backends rot at different rates              | High      | Med    | Per-seam verification every backend must pass (render/audio/save smokes on real or emulated targets) + CI builds every target. |
| R4| Renderer abstraction leaks PPU-isms into PC API   | Med       | Med    | API designed around *what* (draw sprite/tilemap) not *how*; PPU is the constraint that shapes the API, not an afterthought. |
| R5| Toolchain drift (devkitPro/pspsdk breakage)       | Med       | Med    | Pin toolchain versions in `cmake/`; CI builds all four targets.           |
| R6| Scope creep (the eternal engine killer)           | High      | High   | MVP gate in roadmap; "feature count is last pillar" enforced in review.   |
| R7| No FPU + no div on GBA stalls physics             | Low       | Med    | Fixed-point with reciprocal LUTs; physics budgeted to tile/AABB only.     |

---

## 12. Document Index

| Doc | Title                          | Covers                                              |
|-----|--------------------------------|-----------------------------------------------------|
| 00  | **Master Architecture** (this) | Whole-engine style, seams, cross-cutting decisions  |
| 01  | Core                           | Memory root, log, config, time, math/fixed, profile |
| 02  | Platform Layer                 | The C seam, per-platform backends                   |
| 03  | Rendering                      | Unified API, GL/GU/GBA backends, sprite/tile model  |
| 04  | ECS                            | Entity/component/system model, archetypes, tradeoffs|
| 05  | Memory                         | Arena/stack/pool/object allocators + diagrams       |
| 06  | Resources & Assets             | Pack format, caching, streaming, versioning         |
| 07  | Build System                   | CMake, toolchains, four-target strategy             |
| 08  | Tooling                        | phxpack, tilemap/entity editors, sprite/audio conv  |
| 09  | Roadmap                        | MVP, milestones, time estimates, future platforms   |

See also `docs/diagrams/` for standalone UML/module diagrams referenced throughout.
