# Phosphorus Engine — Feasibility Study: A Graphical Interface (Editor) for the Engine

> **Status:** analysis + implementation plan (2026-09-11), written against the tree at
> `v0.1.0` + the unreleased `tinyllm` work.
>
> **Implementation status (2026-09-23).** The plan was followed. What exists today:
>
> - **Phase 1**: `phx/platform/desktop.h`, implemented by `sdl` and a scripted `null` queue.
> - **Phase 2**: `tools/common/twk.h`.
> - **Phase 4**: the Studio Editor workspace with code, sprite/pixel, tilemap and data-table
>   panels. `phxtmap` / `phxentity` are now those panels in a one-document shell.
> - **Phase 7**: the Run view.
> - **Test gate**: the `editors` suite on `make check`.
>
> Also built (2026-09-23, **Phase 3 in part**): `phxproject.json` projects with a hard access
> boundary (a project writes only its own folder; the engine's public API is read-only),
> New-project templates, and generic `make game | game-assets | play PROJECT=` rules.
>
> What is not done:
>
> - **The rest of Phase 3**: no in-process bake (projects bake through `make game-assets`) and no
>   sources glob list.
> - **Phase 5**: per-tier previews exist in the Assets view, but the editors paint RGBA.
> - **Phase 6**: no in-process play mode. Games launch as child processes, and the map editor can
>   **play from here**. Pause, single-step and a live entity inspector live in the running game
>   instead (`phx/runtime/devtools.h`). There is no input replay yet. (Phase 5's spawn art is
>   done: the map editor draws spawns as their prefab sprites.)
> - **Phase 8** is done differently from this plan. `PHX_COMPONENT` (`phx/ecs/reflect.h`)
>   registers a game's components; `make game` exports them to `build/components.json`; the
>   table editor ticks them onto prefab records; and the level loader builds them from those
>   columns. There is no live entity inspector during play (that needs Phase 6's in-process play).
> - **The CI / CMake part of Phase 9**: the editors are Makefile-only.
>
> Usage: `tools/phxstudio/instructions.md`.
> **Question answered:** *Is it feasible to build a graphical interface for this engine, and if
> so, how — step by step?*
> **Short answer:** **Yes.** It is not only feasible, the repository already contains two working
> proofs of the exact approach (`tools/phxtmap`, `tools/phxentity`). The remaining work is
> scoping and engineering, not research. This document explains why, what is missing, which of
> three architectures to pick, and a phase-by-phase plan with concrete files, APIs, tests and
> effort estimates.

---

## 0. Verdict in one page

| Question | Answer |
|---|---|
| Can a GUI be built on top of this engine? | **Yes.** Two GUI editors already run on the engine's own App loop, SDL window, software renderer and immediate-mode UI. |
| Does the architecture fight it? | **No.** The layering, the C-ABI platform seam and the "tools are host-only, STL allowed" rule are exactly what an editor needs. Nothing in the dependency law has to be bent. |
| What kind of GUI? | Three readings, all feasible: (1) in-game GUI — **already exists** (`phx::ui`, runs on all four targets); (2) a standalone desktop **editor/"studio"** — the real question, feasible; (3) a web/Qt/Electron front end — possible but the wrong fit for this project. |
| What is missing? | Desktop-only input (mouse buttons/wheel, key events, text input, resize, clipboard), a pointer-aware widget layer for tools, a project file, a unified studio shell, play-in-editor, and (optionally) component reflection for an inspector. All are additive. |
| Recommended architecture | **Option A — "engine-native studio"**: keep dogfooding the engine (as the two existing editors do), add a small desktop extension to the SDL backend, and build a tool-side widget kit in `tools/common/`. Dear ImGui (Option B) is the documented escape hatch if the bespoke widget kit becomes the bottleneck. |
| Unique advantage | The software renderer is the **golden reference** every hardware backend is diffed against, and it samples the per-target baked texture formats (RGBA8, GU-swizzled, GBA 4bpp palettes) directly. An editor built on it gets a **pixel-exact, per-console WYSIWYG preview** for free — something Unity/Godot-style editors cannot offer a GBA developer. |
| Effort (single engineer) | MVP studio ≈ **8–10 eng-weeks**; full v1 with play mode, build panel and reflection-driven inspector ≈ **13–17 eng-weeks**. |

---

## 1. What was analysed

Every claim below is grounded in the tree, not the design docs alone (several docs describe
design intent that differs from what is built; where that matters it is called out).

- Architecture and rules: `README.md`, `STRUCTURE.md`, `CLAUDE.md`, `docs/00-architecture.md`,
  `docs/02-platform-layer.md`, `docs/03-rendering.md` §6, `docs/08-tooling.md`,
  `docs/09-roadmap.md`, `docs/10-gameplay-systems.md` §6, `docs/graphics-engine.md`.
- The seam and desktop backend: `engine/platform/include/phx/platform/platform.h`,
  `gfx_soft.h`, `engine/platform/src/sdl/sdl_platform.cpp` (375 lines),
  `engine/platform/src/null/null_platform.cpp`.
- The loop and services an editor sits on: `engine/runtime/include/phx/runtime/app.h`,
  `engine/runtime/src/app.cpp`, `engine/render/include/phx/render/renderer.h`,
  `engine/render/src/backend.h`, `engine/render/src/soft/soft_renderer.cpp`,
  `engine/render/src/gl/gl_backend.cpp`, `engine/ui/include/phx/ui/ui.h`, `engine/ui/src/ui.cpp`,
  `engine/input/include/phx/input/input.h`, `engine/core/include/phx/core/config.h`,
  `caps.h`, `engine/ecs/include/phx/ecs/world.h`, `engine/scene/include/phx/scene/scene.h`,
  `engine/resource/include/phx/resource/cache.h`, `engine/memory/include/phx/memory/*`.
- The existing GUI tools: `tools/phxtmap/{main.cpp,editor.h,instructions.md}`,
  `tools/phxentity/{main.cpp,editor.h,instructions.md}`, the shared `tools/common/debug_font.h`.
- The pipeline as a library: `tools/phxpack/{builders.h,bundle_writer.h,bundle_reader.h,json.h,
  tiled.h,png.h,wav.h,tex_encode.h,lock.h}`.
- Build wiring: `Makefile` (`sdl`, `gl`, `tmap`, `entity`, `win` targets and their source
  lists), root `CMakeLists.txt` (tools section), `tools/phxpack/CMakeLists.txt`,
  `.github/workflows/ci.yml`.
- Tests: `tests/suites/pipeline_test.cpp` (the headless editor document-model tests),
  `tests/README.md` conventions.
- Local environment: SDL2 2.32.10 is installed (`sdl2-config`), so `make tmap` / `make entity`
  build and run here today.

---

## 2. The engine as it stands — what matters for a GUI

### 2.1 The layering (and why an editor fits *beside* it, not inside it)

```
 L4  runtime           App: fixed-step loop, owns MemoryRoot / World / Renderer / InputState
 L3  scene physics anim ui
 L2  render input audio resource ecs
 L1  memory platform   <- the C-ABI seam (phx_platform), one backend linked: null | sdl | gba | psp
 L0  core
 ────────────────────────────────────────────────────────────────────────────────────────────
 tools/   host-only C++17, STL allowed, never ships to console. May include ANY engine public
          header. Engine may include NOTHING from tools/ (depcheck enforces engine-internal
          layering; tools are outside the graph).
```

An editor is a **tool**. Tools already sit outside the enforced graph, may use the STL, and may
use every engine public header. That is the single most important fact for feasibility: a GUI
editor does not require any exception to the two inviolable rules ("include only
`include/phx/<mod>/`", "one backend per module per build"), because it lives in the layer that
was designed to consume the engine.

### 2.2 The platform seam — what the desktop backend gives a GUI today

`phx_platform` is a flat struct of C function pointers. The SDL backend implements it for
Windows/Linux and, relevant to a GUI:

| Capability | Status in `sdl_platform.cpp` | Note |
|---|---|---|
| Window | one window, `SDL_WINDOW_SHOWN`, **fixed 3× integer upscale** (`kScale = 3`), **not resizable** | logical size comes from `Config.width/height` |
| Software framebuffer | RGBA8 `phx_soft_fb`, handed to the soft renderer via `phx_gfx_soft_lock()` **every frame** (`SoftBackend::begin` re-locks) | re-locking per frame means a resizable framebuffer needs no renderer change |
| GL path | `PHX_HAVE_GL` swaps in a GL 1.1 context + the GL backend | GL backend accepts **RGBA8 textures only** (`gl_backend.cpp:61`) |
| Keyboard | 12 canonical buttons (arrows/WASD, Z X C V Q E, Enter, Tab/RShift), Esc = quit | **no scancode stream, no text input** |
| Mouse | `pointer_x/y` in framebuffer coordinates + **left button only** | **no right/middle, no wheel, no drag-and-drop** |
| Game controller | full mapping | irrelevant for an editor, harmless |
| Clock | `SDL_GetPerformanceCounter` | fine |
| Files | `open/map/close` load-once heap image; `save/load` key→file | fine for a tool |
| Audio | `phx_sdl_audio_start(rate, fill, user)` **desktop-only extension exported by the SDL TU** | precedent: seam extensions are allowed as extra `extern "C"` symbols consumed only by desktop boot code |
| Readback | `phx_sdl_readback()` for the verifiers | same precedent |

The **precedent** in the last two rows is the key architectural fact for Phase 1 below: the
project already accepts "desktop-only `extern "C"` functions exported by `sdl_platform.cpp`,
declared by the consumer, absent on consoles". A GUI extension follows the same pattern.

The **null** backend (what all tests run on) already supports a *scripted pointer*, which is why
the editors' document models are unit-tested headlessly and why a bounded windowed run
(`PHX_MAX_FRAMES=N`) exists for smoke tests.

### 2.3 The renderer — one 2D-intent API, and the golden reference

`Renderer` (front end, 130 lines) records sprites, sorts by `(layer, z, tex)`, computes zoom /
shake / per-layer parallax in Q16 integer math, then calls one linked `IRenderBackend`. Relevant
properties:

- **Software backend is the golden reference.** `make ppu` / `make gu` / `make gl-verify` assert
  the hardware backends compose *the same pixels*. Anything drawn in an editor through the soft
  backend **is what the GBA/PSP/PC will show**.
- The soft backend **samples every baked texture format directly**: RGBA8, RGBA8_SWZ (PSP) and
  PAL4_TILES (GBA 4bpp + palettes). An editor can therefore show the *tier-0 quantized* art by
  running the bake encoder in memory and uploading the result — the same quantizer that runs on
  the PPU upload path (`tools/phxpack/tex_encode.h`).
- Retained tilemaps are read **live** from the caller's index buffer on the soft backend
  (`upload_tilemap` keeps the pointer; `phxtmap` exploits this: paint → memcpy → visible next
  frame, no re-upload).
- Sprites carry `dw/dh` (scaling), `tint`, `layer`, `z`, arbitrary source rect. That is enough to
  build every widget a tool needs (panels, text, icons, clipped images) without a new primitive.
- Limits to design around: no scissor/viewport, no render-to-texture, 256 texture slots and 32
  tilemap slots in the soft backend, `caps.max_sprites` = 16 384 on PC (plenty for a UI), one
  framebuffer per App.

### 2.4 `phx::ui` — the in-game GUI that already exists (reading 1 of the question)

If "graphical interface" means *menus, HUD, dialogue inside a game*, the answer is simply that it
is already built and shipping on all four targets: `UI::text / image / rect / bar / button /
dialogue / profile_overlay`, immediate-mode, zero per-frame allocation, focus-ring navigation
for consoles (Up/Down + A), a typewriter dialogue box with word wrap, and the frame profiler
overlay. It is deliberately **console-sized**: bitmap 8×8 fonts, no pointer hit-testing in
`button()`, no text fields, no scrolling, no clipping. That is correct for a GBA and must stay
that way; the editor's widgets should be layered *on top* of these primitives in `tools/`, not
added to `engine/ui` (see §5).

### 2.5 The tool layer and the two existing GUI editors (the proof)

Both editors are structured identically and this structure is the template for everything below:

```
 tools/phxtmap/                       tools/phxentity/
 ├── editor.h   TmapDoc (309 lines)   ├── editor.h   BinDoc (222 lines)
 │   document model: load/edit/undo/  │   document model: typed record table,
 │   save Tiled .tmj; headless-tested │   clamp-stepping, add/remove field/record
 ├── main.cpp   GUI shell (447 lines) ├── main.cpp   GUI shell (212 lines)
 │   a phx::Game: on_start uploads    │   same App / SDL / soft renderer / phx::ui
 │   textures+tilemap, on_fixed_update│
 │   reads InputState (pointer+keys), │
 │   on_render draws map + palette +  │
 │   status via phx::ui               │
 └── instructions.md                  └── instructions.md
```

What they prove, concretely:

1. A `phx::Game` subclass **is** a desktop application: `App::run()` gives it a window, a
   60 Hz loop, a renderer, and pointer + keyboard state with no platform code in the tool.
2. Mouse hit-testing against engine-rendered content works (palette strip, tile canvas,
   marquee drag, spawn markers).
3. Edits stream zero-copy into the renderer (live painting).
4. Document models are pure C++ over the *same* importer the bake uses (`tiled.h`, `json.h`),
   so "what the editor saves, the bake accepts" is asserted in `make pipeline`.
5. Editors emit **author formats** (`.tmj`, phxbin JSON), never engine blobs — the bake stays
   the single path. The shared prefab schema (a `str` `type` column) already links the two
   editors and the game with no source edits.
6. The Windows build packages every host binary as a static `.exe` (`make win`), so an editor
   built this way ships to Windows with no extra work.

What they *don't* have (and a studio must add): a real widget vocabulary (they use raw key
chords: `X+C`, `X+Tab`), text entry (string cells are read-only), real tileset art (procedural
swatches), multiple panels, resizable windows, project awareness, play mode.

### 2.6 The asset pipeline is already a library

`tools/phxpack/builders.h` is header-only: `build_png / build_tmj / build_wav / build_sprite /
build_bin / build_from_source(BundleWriter&, path)`; `BundleWriter` does the per-target encode
(`--target 0|1|2`); `lock.h` gives incremental rebakes; `bundle_reader.h` reads a bundle back
into `ReadAsset`s. An editor can **bake in-process** with a function call, then mount the result
with `ResourceCache::mount()` / `unmount()` for a live preview through the real runtime path.

### 2.7 Tests and gates an editor plugs into

- `make pipeline` already runs `TmapDoc`/`BinDoc` round-trips headlessly.
- `PHX_MAX_FRAMES=N ./build/phxtmap` is the bounded windowed smoke.
- `make check` / `determinism` / `sanitize` / `release` / `size-gate` / `win-verify` are the
  gates; a studio adds headless document/layout tests to `pipeline` and, optionally, a bounded
  smoke under Xvfb in CI (CI currently does not build the SDL tools at all — see §7 Phase 9).

---

## 3. What "a graphical interface for the engine" can mean

| Reading | Meaning | Feasibility | Where it stands |
|---|---|---|---|
| **R1. In-game GUI** | menus/HUD/dialogue drawn by games on GBA/PSP/PC | Done | `engine/ui` (§2.4). Extending it (e.g. pointer support for PC-only games, more widgets) is a small, ordinary engine task. |
| **R2. Desktop editor / "Phosphorus Studio"** | a windowed authoring application: project browser, level editor with live viewport, tileset/sprite/animation editing, prefab & entity inspector, audio preview, bake/build/deploy to all targets, play-in-editor | **Feasible; the subject of this document** | Two single-purpose editors exist (§2.5). |
| **R3. External-toolkit front end** | Qt / Electron / web UI driving the CLI tools, or a Dear ImGui app embedding the engine | Feasible, but adds the first third-party UI dependency and a second UI technology | Nothing exists; §5 evaluates it as Options B/C. |

The rest of the document is about **R2**, since R1 is done and R3 is a variant of R2's design
choice.

---

## 4. Feasibility: evidence and gaps

### 4.1 Already in place (no work required)

| Need of an editor | Provided by | Evidence |
|---|---|---|
| A window + loop + input on Win/Linux with zero platform code in the tool | `App` + SDL backend | `phxtmap`, `phxentity` |
| Mouse position and left click, in framebuffer coordinates | `phx_input_raw.pointer_*` → `InputState.pointer` | `phxtmap` hit-tests tiles |
| Drawing panels, text, icons, images with tint/scale/layering | `phx::UI` primitives over `DrawSprite` | both editors |
| Live, zero-copy editing of a tilemap | soft backend reads the index buffer live | `phxtmap::reflatten()` |
| Exact preview of what consoles render | soft backend = golden reference; samples PAL4/SWZ | `make ppu`, `make gu`, `make gl-verify` |
| Reading/writing the author formats | `tiled.h`, `json.h`, `TmapDoc`, `BinDoc` | pipeline suite |
| Baking in-process, per target, incrementally | `builders.h`, `BundleWriter`, `lock.h` | `phxpack` CLI is a thin wrapper over these |
| Loading a baked bundle through the real runtime path | `ResourceCache::mount/unmount` | resource suite |
| Headless testing of editor logic | null platform (virtual clock, scripted pointer) | pipeline suite, `PHX_MAX_FRAMES` |
| Windows packaging | `make win` (static PE32+) | every host binary already ships this way |
| Animation preview | `anim::Animator` + `SpriteView` | sprite suite |
| Audio preview on a real device | `phx_sdl_audio_start` + `AudioMixer` | `make audio-verify`, `desktop_main.cpp` |
| Deterministic sim (record/replay for free) | fixed-step `App`, `phx_input_raw` snapshot per frame | determinism gate |

### 4.2 Gaps (all additive; none touches gameplay code or console builds)

| # | Gap | Where it is closed | Size |
|---|---|---|---|
| G1 | No mouse buttons beyond left, no wheel, no key-event stream, no text input, no window resize, no clipboard, no drag-and-drop of files | a **desktop extension** to the SDL backend (`phx_desktop_*`), mirrored by a scripted implementation in the null backend for tests | small (~250 lines) |
| G2 | `phx::ui` has no pointer-aware widgets, text fields, scrolling, clipping, layout, IDs, tooltips, menus | a **tool-side widget kit** in `tools/common/` built on `UI::rect/text/image` | medium (~1.5–2.5 k lines) |
| G3 | No notion of a *project* (which sources, which targets, which bundle, which game binary) | `phxproject.json` + `ProjectDoc` (author format, headless-tested) | small |
| G4 | Editors are separate binaries with disjoint UX; no real tileset art; string cells read-only | one **studio shell** with panels reusing `TmapDoc`/`BinDoc` verbatim | medium-large |
| G5 | Renderer has no viewport/scissor; one framebuffer | not needed for v1: draw the level full-screen and **overdraw** the chrome on higher layers (exactly what `phxtmap` does with its palette strip); optional later: a front-end `set_viewport()` | none / small |
| G6 | No play-in-editor | v1: child process (the game binary or mGBA/PPSSPP with the fresh bundle); v2: host the game's `Game` in-process | small / medium |
| G7 | No build/deploy UI | run the existing `make` targets and `size_gate.py` from a panel, capture output | small |
| G8 | Inspector has no component schema; `PHX_REFLECT` is "planned" in docs/08 §8 | optional engine addition: a constexpr field table per component | medium |
| G9 | GUI tools are Makefile-only; CMake and CI do not build them | `PHX_BUILD_EDITORS` CMake option + `find_package(SDL2)`; CI job under Xvfb | small |
| G10 | 8×8 debug font only | bake a proportional bitmap font through `phxsprite`; `BitmapFont` already supports any cell size and `advance` | small |
| G11 | Tiled custom properties on objects are not round-tripped by `tiled.h` (`TiledSpawn` = name/type/x/y/w/h) | extend the importer/exporter with a `properties` list — needed for per-spawn overrides in the inspector | small |

Nothing in this list requires changing the C seam's *game-facing* struct (`phx_platform`,
`phx_input_raw`), the render backend seam, the ECS, or any console backend. The GBA ROM is
unaffected byte-for-byte by every phase except the optional G8, which is designed to be zero-cost
there.

---

## 5. Architecture options and the recommendation

### Option A — Engine-native studio (dogfood, extended) — **recommended**

The studio is one more `phx::Game` in `tools/phxstudio/`, linked exactly like `phxtmap`
(`APP_SRC` minus null + `sdl_platform.cpp`). It gains desktop input via a small seam extension
and its widgets from a tool-side kit built on `phx::ui` primitives.

- **Pros:** zero new dependencies (matches pillar 1 and the recorded decision in docs/08 §7 that
  no external toolkit was needed); the viewport *is* the golden renderer (pixel-exact per-tier
  preview, including GBA palette quantization); one UI technology across games and tools; ships
  as a static Windows exe via the existing `make win`; the editors' bugs and the engine's bugs
  are the same bugs (the roadmap's stated mitigation for "editor maintenance burden").
- **Cons:** the widget kit is bespoke (text fields, scrolling, docking must be written); bitmap
  fonts at an integer scale rather than native-looking text; no native file dialogs (an in-tool
  file browser over `std::filesystem` replaces them); software rasterization cost grows with
  window size (see §9).

### Option B — Dear ImGui shell embedding the engine

The tool owns an SDL2+GL window with ImGui; the engine runs **headless** in-process on the
**null** platform, and the viewport is the null backend's software framebuffer uploaded to a GL
texture and shown as an `ImGui::Image`.

- **Pros:** mature widgets (docking, tables, text input, multi-window) in days, not weeks.
- **Cons:** first third-party UI dependency (allowed by the "tools are host-only" rule but a
  policy change); two UI technologies to maintain; the null backend's virtual clock and scripted
  input mean play-mode inside the ImGui viewport needs a custom platform backend or a child
  process anyway; loses "the editor is a Phosphorus app" (the project's explicit dogfooding value).
- **When to choose it:** if after Phase 2 the bespoke widget kit is clearly the schedule risk.
  The design below keeps document models, project model, bake, viewport composition and play
  mode **independent of the widget layer**, so switching the shell to ImGui later discards only
  `tools/common/twk.h` and the panel drawing code, not the editor logic.

### Option C — Out-of-process front end (Qt / Electron / web)

A separate GUI process drives the CLI converters and reads PPM/PNG frames rendered headlessly.

- **Verdict:** technically possible (every converter is a CLI; `make render` writes a `.ppm`),
  but it contradicts the project's pillars (feature count last, no dependencies, one codebase),
  cannot give a live viewport without an IPC frame stream, and would be the only component not
  written in the engine's language. Not recommended; listed for completeness.

**Recommendation: Option A**, with the document/project/bake/viewport/play layers designed to be
shell-agnostic so Option B remains a two-week pivot rather than a rewrite.

---

## 6. Target design — "Phosphorus Studio" (`phxstudio`)

### 6.1 What the user sees

```
 ┌────────────────────────────────────────────────────────────────────────────────────┐
 │ File  Edit  View  Build  Play  Help                       [PC ▾] [GBA] [PSP]   ●    │  menu bar + tier preview switch
 ├───────────────┬───────────────────────────────────────────┬────────────────────────┤
 │ PROJECT       │  LEVEL: cinder_hollow.tmj        zoom 2x   │ INSPECTOR              │
 │ ▸ art/        │  ┌──────────────────────────────────────┐ │ spawn "geyser3"        │
 │   hero.sprdef │  │                                      │ │  type   [geyser   ▾]   │
 │   tiles.png   │  │      live viewport (soft renderer,   │ │  x      [ 412 ] y [96] │
 │ ▸ maps/       │  │      real tileset, real parallax,    │ │  props  period: 90     │
 │   level1.tmj  │  │      GBA-quantized when [GBA] is on) │ │ ─────────────────────  │
 │ ▸ audio/      │  │                                      │ │ LAYERS                 │
 │   jump.wav    │  └──────────────────────────────────────┘ │  ▣ sky   (par 0.25)    │
 │ ▸ data/       │  TILESET ▣▣▣▣▣▣▣▣▣▣▣▣▣▣▣▣  tool: PAINT▾  │  ▣ hills (par 0.5)     │
 │   prefabs.json│  collision: none|solid|oneway|hazard      │  ▣ main  (gameplay) ◄  │
 ├───────────────┴───────────────────────────────────────────┴────────────────────────┤
 │ CONSOLE  bake: 14 assets, 61 KB (tier 0)  ·  size-gate: ROM 412/4096 KB  IWRAM 19/32 │
 └────────────────────────────────────────────────────────────────────────────────────┘
```

Panels (each a small class with `update(ctx)` / `draw(ctx)` over a document):
Project browser · Level editor (tiles, collision flags, spawns, layers, parallax) · Tileset &
palette · Sprite/animation editor · Prefab/record table · Inspector · Audio preview · Console
/ log · Build & deploy · Play toolbar.

### 6.2 Data flow (author formats in, bundles out — unchanged pipeline)

```
        author files (open formats)              tools/phxpack (library)          runtime
 ┌─────────────────────────────┐   in-process   ┌──────────────────┐  mount   ┌───────────────┐
 │ .tmj  .sprdef/.json  .png   │ ─────────────▶ │ BundleWriter     │ ───────▶ │ ResourceCache │
 │ .wav  prefabs.json          │  build_*()     │ per-target encode│  (live   │ Renderer(soft)│
 │ phxproject.json (new)       │                │ lock.h increment.│  preview)│ = golden ref  │
 └──────────────▲──────────────┘                └────────┬─────────┘          └───────────────┘
                │ save (TmapDoc / BinDoc / SprDoc / ProjectDoc)        │ .phxp
        ┌───────┴───────┐                                              ▼
        │ phxstudio GUI │                                    make gba-*/psp-*/win  (child process)
        └───────────────┘                                    mGBA / PPSSPP / game binary (play)
```

The studio never writes an engine blob directly; it saves author files and calls the same bake
the CLI uses. That preserves the pipeline guarantees in docs/08 §9 (determinism, offline
validation, round-trip tests) with no new code paths.

### 6.3 File layout

```
 engine/platform/include/phx/platform/desktop.h    NEW  C declarations of the desktop extension (no SDL types)
 engine/platform/src/sdl/sdl_platform.cpp          MOD  implement phx_desktop_* (events, resize, clipboard)
 engine/platform/src/null/null_platform.cpp        MOD  scripted phx_desktop_* for headless tests
 tools/common/twk.h                                 NEW  tool widget kit (pointer-aware IMGUI over phx::ui)
 tools/common/font_ui.h                             NEW  a baked proportional bitmap font (optional, G10)
 tools/phxstudio/
   project.h        NEW  ProjectDoc: phxproject.json load/save/validate, source list, targets
   sprdoc.h         NEW  SprDoc: .sprdef/.json sprite+clip document (wraps builders.h SprDef)
   bake.h           NEW  in-process bake (BundleWriter + lock.h), per target, with progress + log capture
   viewport.h       NEW  viewport composition: camera, overdraw chrome, tier preview textures
   panels/*.h       NEW  one file per panel (level, tileset, sprite, table, inspector, audio, build, console)
   play.h           NEW  play mode: child-process launcher (v1) / in-process host (v2)
   main.cpp         NEW  the shell: a phx::Game composing panels; arg parsing
   instructions.md  NEW  usage, formats, controls (the repo convention)
 tools/phxtmap, tools/phxentity                     KEEP  document models are reused verbatim by the studio;
                                                          the shells may stay as thin single-panel launchers
 tests/suites/pipeline_test.cpp                     MOD  ProjectDoc / SprDoc / twk layout tests
 Makefile                                           MOD  `make studio`, `studio-smoke`; add to `win`
 CMakeLists.txt                                     MOD  option(PHX_BUILD_EDITORS) + find_package(SDL2)
 .github/workflows/ci.yml                           MOD  editors job (xvfb-run bounded smoke)
 docs/08-tooling.md                                 MOD  §10 Phosphorus Studio
```

---

## 7. Step-by-step implementation plan

Each phase ends with a runnable artifact and a gate. Phases 1–5 are the MVP; 6–9 complete v1.

### Phase 0 — Scope, conventions, skeleton (½ week)

1. Create `tools/phxstudio/` with `main.cpp` copied from `tools/phxtmap/main.cpp`, renamed
   `StudioGame`, and an `instructions.md` stub.
2. Add `STUDIO_SRC`/`make studio` to the `Makefile` next to `tmap:` (same recipe: `APP_SRC`
   minus null + `sdl_platform.cpp` + `tools/phxstudio/main.cpp`, `-DPHX_HAVE_SDL`).
3. Decide and write down (in `instructions.md`) the three non-negotiables inherited from the
   existing editors: **document model ≠ shell**, **author formats only**, **no OS/SDK header
   in the tool** (everything platform-specific goes through `phx/platform/desktop.h`).
4. Gate: `make studio && PHX_MAX_FRAMES=60 ./build/phxstudio` boots and exits 0.

### Phase 1 — Desktop extension to the platform seam (1 week)

Goal: give tools mouse buttons/wheel, key events, text input, resize, clipboard, file drop —
without touching the game-facing seam or any console backend.

1. New header `engine/platform/include/phx/platform/desktop.h` (C, includes only `platform.h`):

```c
typedef enum phx_desktop_ev_kind {
    PHX_DEV_NONE = 0, PHX_DEV_KEY_DOWN, PHX_DEV_KEY_UP, PHX_DEV_TEXT,
    PHX_DEV_MOUSE_DOWN, PHX_DEV_MOUSE_UP, PHX_DEV_WHEEL, PHX_DEV_RESIZE, PHX_DEV_DROP_FILE
} phx_desktop_ev_kind;

typedef struct phx_desktop_event {
    uint8_t  kind;         /* phx_desktop_ev_kind */
    uint8_t  button;       /* 1=left 2=middle 3=right */
    uint16_t mods;         /* shift/ctrl/alt bitmask */
    int32_t  key;          /* portable key code (own enum, NOT SDL scancodes) */
    int16_t  x, y;         /* framebuffer coords (mouse), or new logical size (resize) */
    int16_t  wheel_x, wheel_y;
    char     text[16];     /* UTF-8, PHX_DEV_TEXT / PHX_DEV_DROP_FILE (path via phx_desktop_drop_path) */
} phx_desktop_event;

int         phx_desktop_available(void);                 /* 0 on backends without a desktop */
int         phx_desktop_poll(phx_desktop_event* out);    /* 1 = event written, 0 = queue empty */
void        phx_desktop_set_scale(int integer_scale);    /* 1..4; window = logical * scale */
int         phx_desktop_logical_size(int* w, int* h);    /* current framebuffer size (after resize) */
void        phx_desktop_text_input(int enable);          /* SDL_StartTextInput / Stop */
const char* phx_desktop_clipboard_get(void);
void        phx_desktop_clipboard_set(const char* utf8);
const char* phx_desktop_drop_path(void);                 /* path of the last PHX_DEV_DROP_FILE */
```

2. Implement in `sdl_platform.cpp`: make the window `SDL_WINDOW_RESIZABLE`; in
   `sdl_pump_events` translate `SDL_KEYDOWN/UP`, `SDL_TEXTINPUT`, `SDL_MOUSEBUTTONDOWN/UP`,
   `SDL_MOUSEWHEEL`, `SDL_WINDOWEVENT_SIZE_CHANGED`, `SDL_DROPFILE` into a fixed-capacity ring
   (no heap; 256 events). On resize: reallocate `g.fb.pixels` to `(w/scale, h/scale)`, recreate
   the streaming texture, call `SDL_RenderSetLogicalSize`. The soft backend re-locks the
   framebuffer every `begin()`, so **no renderer change** is needed. Add right/middle buttons to
   the mouse mask (kept out of `phx_input_raw` so the game seam is untouched).
3. Implement in `null_platform.cpp`: the same functions over a **scripted queue**
   (`phx_null_desktop_push(event)`), so studio logic is testable headlessly — the same trick the
   null backend uses for the pointer.
4. Consoles: nothing. Only `sdl` and `null` are ever linked into a tool. (If a game wants to
   probe at runtime, `phx_desktop_available()` may be provided as a weak stub returning 0 by
   `platform.h`'s consumers; not required.)
5. The SDL backend currently treats `Esc` as quit. Add `phx_desktop_set_quit_on_escape(int)`
   to the extension so a tool can turn that off and let text fields and modals receive
   Escape; games keep the default.
6. Tests: a unit test in `tests/unit/` that pushes scripted events into the null backend and
   asserts ordering and coordinates; `make check` stays green; `make depcheck` unaffected (the
   header includes only `platform.h`).
7. Gate: `make studio` shows a resizable window; right-click, wheel and typed text arrive as
   events (print them to the console for now).

### Phase 2 — Tool widget kit `tools/common/twk.h` (2–3 weeks)

Goal: a pointer-aware immediate-mode widget layer good enough for an editor, implemented only
with `UI::rect / text / image`, so it runs on the soft **and** GL backends and never touches
`engine/ui`.

1. Core: `struct Twk { UI& ui; const InputState& in; DesktopFrame& dev; ... }` holding the
   frame's desktop events, hot/active widget IDs (`fnv1a(label) ^ call-site counter`, the engine
   already has `fnv1a`), a **clip-rect stack** and a **layout cursor**.
2. Clipping without a scissor: `rect()` intersects with the clip rect; `text()` drops glyphs
   outside it; `image()` trims the *source* rect proportionally (the sprite path supports any
   `sx/sy/sw/sh` + `dw/dh`). This is enough for scroll regions and tables.
3. Widgets, in order of need: `panel`, `label`, `button` (hover/press/click by pointer),
   `toggle`, `int_field` / `float_field` (drag-to-scrub + typed), `text_field` (caret,
   selection, clipboard, `PHX_DEV_TEXT`), `dropdown`, `list`/`tree` (with scroll), `tabs`,
   `splitter` (drag to resize panels), `menu_bar` + `menu`, `tooltip`, `modal`, `color_swatch`,
   `hotkey(mods, key)`.
4. Layout: a simple row/column cursor with padding, plus absolute placement; no constraint
   solver. Persist splitter positions in `~/.config/phxstudio/layout.json` (STL is allowed).
5. Fonts: keep `tools/common/debug_font.h` (8×8) as the fallback; add a baked 6×10 or
   proportional font (G10) via `phxsprite` at build time or a generated header like
   `debug_font.h`. `BitmapFont.advance` handles the rest.
6. Tests: layout and hit-test math are pure functions — cover them in `pipeline` with the null
   backend's scripted pointer/desktop events (click a button, type into a field, scroll a
   list; assert model changes). No display required.
7. Gate: a "widget gallery" mode (`phxstudio --gallery`) exercising every widget; bounded smoke
   in CI.

### Phase 3 — Project model (1 week)

1. `phxproject.json` (author format; example):

```json
{ "name": "emberwing",
  "sources": ["art/*.png", "art/*.sprdef", "maps/*.tmj", "audio/*.wav", "data/prefabs.json"],
  "prefabs": "data/prefabs.json",
  "targets": { "pc": "build/emberwing.phxp", "gba": "build/emberwing.gba.phxp", "psp": "build/emberwing.psp.phxp" },
  "run":     { "pc": "./build/emberwing_sdl", "gba": "make gba-emberwing-ppu", "psp": "make psp-emberwing" },
  "emulators": { "gba": "mgba-qt", "psp": "flatpak run org.ppsspp.PPSSPP" } }
```

2. `tools/phxstudio/project.h`: `ProjectDoc::load/save/validate`, glob expansion
   (`std::filesystem`), asset-kind detection by extension (the same table
   `build_from_source` uses), dirty tracking. Headless tests in `pipeline`.
3. `tools/phxstudio/bake.h`: `bake_project(const ProjectDoc&, uint8_t tier, Log&)` → runs
   `build_from_source` for each source into a `BundleWriter` for that tier, honours `lock.h`
   incremental rebakes, captures per-asset warnings (e.g. the tier-0 ">15 colours in a tile"
   report) into the console panel. Bakes are pure functions of their inputs (docs/08 §9), so
   they may run on a `std::thread` inside the tool with a progress bar — the engine's
   single-threaded contract concerns engine state, which the bake never touches.
4. Gate: `phxstudio --bake examples/emberwing/phxproject.json --target 0` from the CLI produces
   a bundle byte-identical to `phxpack` (assert in `make pipeline`).

### Phase 4 — Studio shell and panels (3–4 weeks)

1. `main.cpp`: `StudioGame : phx::Game` owning a `ProjectDoc`, the open documents
   (`TmapDoc`, `BinDoc`, `SprDoc`), the widget kit, the panel set, undo routing and hotkeys.
   `on_fixed_update` drains desktop events → widgets → document edits; `on_render` draws the
   viewport then the chrome.
2. **Level panel**: port `phxtmap`'s tools (paint/fill/rect/pick, collision cycling, entity
   mode, layers, parallax) onto real widgets; replace the procedural swatch atlas with the
   project's **real tileset PNG** decoded via `tools/phxpack/png.h` and uploaded with
   `load_texture` (RGBA8) — the palette strip becomes a scrollable tileset view.
3. **Tileset panel**: per-GID collision flags with real art; palette inspection for tier 0.
4. **Prefab/table panel**: `phxentity` grid with editable cells (typed values via
   `int_field`/`text_field`, so string columns finally become editable — fixing the
   documented "author strings in the JSON" limitation), add/remove field.
5. **Inspector**: the selected spawn's `name/type/x/y/w/h`; extend `tiled.h`/`TmapDoc` to
   round-trip Tiled object `properties` (G11) so per-spawn overrides are authored here and
   survive Tiled.
6. **Sprite panel**: `SprDoc` over `builders.h`'s `SprDef` (frame size, clips, fps, loop),
   live playback through `anim::Animator` using the real renderer.
7. **Audio panel**: play a WAV via `phx_sdl_audio_start` + `AudioMixer` (copy the 20-line glue
   from `examples/emberwing/src/desktop_main.cpp`), with the tier-0 resample toggled to hear the
   18 157 Hz GBA version.
8. **Console panel**: hook `log_set_sink` and the bake log; filter by level.
9. Undo: `TmapDoc` already has bounded snapshot undo; give `BinDoc`/`SprDoc` the same shape;
   the shell owns one undo stack per document, `Ctrl+Z / Ctrl+Y` hotkeys.
10. Gate: open `examples/emberwing`'s sources, edit a tile, a prefab value and a clip, save,
    rebake, and `make emberwing` (the headless verified playthrough) still passes.

### Phase 5 — Live WYSIWYG viewport with per-tier preview (1–2 weeks)

1. **Composition without a scissor**: draw the tilemap layers full-screen with the camera offset
   so the level lands under the viewport panel, then draw every panel on `layer ≥ 200` with
   opaque backgrounds (this is precisely `phxtmap`'s palette-strip technique). Cull editor
   overlays (spawn markers, marquee) to the viewport rect in the tool. Optional later step: a
   front-end `Renderer::set_viewport(rect)` implemented in `renderer.cpp` (soft/GL inherit it;
   PPU could map it to WIN0) — only if overdraw ever proves insufficient.
2. **Parallax and zoom for real**: call `set_tilemap_parallax` with the layer factors from the
   `.tmj` and `Camera2D.zoom` for the editor zoom — the preview then scrolls exactly as the game
   does on every backend, because both are front-end Q16 features.
3. **Tier preview switch** `[PC] [GBA] [PSP]`: run `tex_encode.h` in memory on the tileset and
   sprites for the chosen tier, upload the result as `PAL4_TILES` / `RGBA8_SWZ`, and re-point
   the tilemap's `tileset`. The soft backend samples those formats directly, so the viewport
   shows the **quantized GBA palette** (and the encoder's warnings mark the offending tiles in
   the tileset panel). This works only with the software backend — the GL backend accepts RGBA8
   only — so the studio links `soft` (as the editors do today), not `gl`.
4. **Spawn art**: convention, not engine change — a `sprite` string column in the prefab table
   names the sprite asset; the viewport draws that sprite's first frame at each spawn instead of
   a lettered box.
5. Gate: a pixel-diff test in `pipeline`: render a level through the studio's viewport code and
   through the game's own `on_render` on the null backend; the level pixels must match (the
   same golden-reference argument the hardware backends use).

### Phase 6 — Play mode (1 week for v1; +2 for v2)

- **v1 — child process (recommended first):** `play.h` bakes for the chosen target and launches
  the project's `run.<target>` command with the bundle path (`popen`, output into the console
  panel). For GBA/PSP it launches the configured emulator on the ROM/EBOOT; the mGBA/PPSSPP
  recipes already in `STATUS.md` apply. Robust, no arena or texture-slot lifetime concerns.
- **v2 — in-process:** the studio hosts the game's `Game` object (e.g. `EmberwingGame`, which
  is already a library: `game.h` + `systems.cpp` + `scenes.cpp`) and forwards
  `on_fixed_update/on_render` while playing, overlaying a toolbar. Prerequisites: give the
  hosted game its **own sub-arena** carved once from `persistent()` and reset between sessions
  (the root arena is app-lifetime by contract; `ArenaAllocator` exposes `mark()/reset_to()` but
  the ECS World and renderer slots are not rewindable piecemeal), and route its texture loads
  through `TextureCache` so slots are released on stop. Because the loop is fixed-step and
  deterministic, recording `phx_input_raw` per frame gives **replay** for free.
- Gate: Play → the game runs from the just-saved level; Stop returns to the editor with the
  document untouched.

### Phase 7 — Build & deploy panel (1 week)

1. Buttons per target invoking the existing Make targets (`gba-emberwing-ppu`, `psp-emberwing`,
   `win`, `emberwing-sdl`) via `popen`, streaming output into the console.
2. Parse `tools/common/size_gate.py` output into ROM/IWRAM/EWRAM bars; the budgets are the
   `caps.h` constants (224 KB EWRAM, 24 KB IWRAM scratch) plus the ROM cap the gate uses.
3. Detect toolchains at startup (`arm-none-eabi-g++`, `psp-g++`, `x86_64-w64-mingw32-g++`) and
   grey out targets that are absent, with the install hint from `CLAUDE.md`.
4. Gate: one click produces `build/gba/…gba` and opens it in mGBA.

### Phase 8 — Component reflection for the inspector (optional, 2 weeks)

The docs/08 §8 "planned" item. Minimal design that costs consoles nothing:

```cpp
// phx/ecs/reflect.h (engine, header-only)
struct FieldInfo { const char* name; uint8_t type; uint16_t offset; };
template <class C> struct Reflect;             // specialised by PHX_REFLECT
#define PHX_REFLECT(C, ...) template <> struct phx::Reflect<C> { static constexpr FieldInfo fields[] = { __VA_ARGS__ }; };
```

Games annotate their components once; the studio (host-only) lists fields, types and offsets
to render an inspector and to generate a prefab table schema automatically. On console builds
the tables are `constexpr` arrays referenced by no one and are dropped by the linker (verify
with `make size-gate`). This turns "prefab" from a record table into a named component list with
defaults, and `phxtmap`'s spawn `{type, x, y}` into `{prefab_hash, x, y, overrides}` as docs/08
already sketches.

### Phase 9 — Build system, CI, docs (1 week)

1. CMake: `option(PHX_BUILD_EDITORS OFF)`; when on, `find_package(SDL2)` and add
   `phxstudio` (+ `phxtmap`, `phxentity`) — closes the "wiring them into CMake is still open"
   note in the root `CMakeLists.txt`.
2. `make win`: add `phxstudio` to the Windows static build (MinGW SDL2 needed; note it in
   `CLAUDE.md`/`CONTRIBUTING.md`).
3. CI: a new job that installs `libsdl2-dev` + `xvfb` and runs
   `xvfb-run make studio-smoke` (`PHX_MAX_FRAMES=120 ./build/phxstudio --gallery`), plus the
   headless document tests already inside `make check`.
4. Docs: `docs/08-tooling.md` §10, `tools/phxstudio/instructions.md`, a `CHANGELOG.md` line
   under `[Unreleased]`, update `STRUCTURE.md`'s `tools/` tree.
5. Gate: `make check`, `make determinism`, `make sanitize`, `make release`, `make size-gate`,
   `make win-verify` all green — the studio must not move a single byte in the GBA ROM.

---

## 8. Effort estimate

| Phase | Deliverable | Eng-weeks |
|---|---|---|
| 0 | skeleton + `make studio` | 0.5 |
| 1 | desktop seam extension (sdl + null) + tests | 1 |
| 2 | tool widget kit | 2–3 |
| 3 | project model + in-process bake | 1 |
| 4 | studio shell + panels (reusing `TmapDoc`/`BinDoc`) | 3–4 |
| 5 | live viewport + per-tier preview | 1–2 |
| **MVP** | **usable editor for an Emberwing-sized project** | **8.5–11.5** |
| 6 | play mode v1 (child process) | 1 |
| 7 | build & deploy panel | 1 |
| 8 | component reflection + inspector (optional) | 2 |
| 9 | CMake/CI/docs/Windows | 1 |
| **v1** | | **13.5–16.5** |
| 6b | play mode v2 (in-process, replay) | +2 |

For calibration: the whole engine's M6 milestone (both editors + profiler overlay + docs pass)
was budgeted at 5 eng-weeks in `docs/09`, and the two shipped editors total ~1 200 lines. The
studio is roughly a 6–8 k line tool.

---

## 9. Risks and mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| Bespoke widget kit becomes the schedule (the classic editor trap; R6 in docs/00) | Medium | Phase 2 has a fixed widget list; everything else is shell-agnostic; Option B (ImGui) is the documented pivot with a known integration path (null platform framebuffer → GL texture). |
| Seam contamination: SDL types leaking above the seam | Low | `desktop.h` is C with its own event/key enums; depcheck + the "no OS header in tools" rule in `instructions.md`; grep gate in CI (`grep -r '#include <SDL' tools/` must be empty). |
| Software rasterization cost at large windows | Medium | Keep an integer scale ≥ 2 (1080p window = 960×540 logical ≈ 0.5 Mpx; tile fill is per-pixel, sprites are few); profile with the built-in `profile_overlay`; only if needed, add a dirty-rect skip for unchanged tilemap layers in the soft backend, or link GL for the non-tier-preview mode. |
| GL backend cannot show PAL4/SWZ tier previews | Certain | Studio links the soft backend (as today's editors do). Adding PAL4 upload to GL is a small backend task if ever desired. |
| Text quality with bitmap fonts | Low | Bake a proportional font (G10); integer scaling keeps pixel art crisp, which suits a retro-engine tool. |
| Play-in-editor v2 arena/slot lifetime bugs | Medium | Ship v1 (child process) first; v2 only behind a sub-arena + `TextureCache` discipline, verified under `make sanitize`. |
| Editors not covered by CI (today) | Certain | Phase 9 Xvfb job + headless doc tests in `check`. |
| Tiled compatibility drift when adding object properties | Low | Extend `tiled.h` only with fields Tiled itself emits (`properties` arrays); round-trip test in `pipeline`. |

---

## 10. Guardrails — what *not* to do

- Do **not** add pointer widgets, text fields or clipping to `engine/ui`. It is sized for a
  128-sprite OAM; the editor's widget kit belongs in `tools/common/`.
- Do **not** put `#ifdef PHX_HAVE_SDL` or SDL includes in the studio; talk to
  `phx/platform/desktop.h` only, so the studio also builds against the null backend for tests.
- Do **not** write `.phxp`/`.phxspr`/`.phxtmap` from the editor except by calling the shared
  bake path; editors emit author formats (docs/08 §1).
- Do **not** extend `phx_input_raw`/`phx_platform` for editor needs; use the desktop extension
  so every console backend stays untouched and the GBA ROM stays byte-identical.
- Do **not** make the editor a second render technology; the viewport is the golden soft
  renderer — that is the product's differentiator.

---

## 11. Appendix — see it today

```bash
make tmap && ./build/phxtmap --out /tmp/x.tmj --size 32x20     # the GUI tilemap editor (SDL2 present here)
make entity && ./build/phxentity --new Enemy --fields hp:u16,atk:i8 --out /tmp/e.json
PHX_MAX_FRAMES=120 ./build/phxtmap /tmp/x.tmj                   # bounded headless-friendly smoke
make pipeline                                                   # the editors' document models, tested headlessly
make render && xdg-open build/render_out.ppm                    # what the golden software renderer draws
```

Reference points in the tree used by this plan:

- Seam extension precedent: `phx_sdl_audio_start`, `phx_sdl_readback` in
  `engine/platform/src/sdl/sdl_platform.cpp`, consumed by `examples/emberwing/src/desktop_main.cpp`.
- Overdraw-as-viewport precedent: the palette strip in `tools/phxtmap/main.cpp::on_render`.
- Live zero-copy tilemap editing: `tools/phxtmap/main.cpp::reflatten()` +
  `Renderer::upload_tilemap` (soft backend keeps the pointer).
- Per-tier texture encode callable in-process: `tools/phxpack/tex_encode.h`, exercised by `make ppu`.
- Bake as a library: `tools/phxpack/builders.h::build_from_source`, `bundle_writer.h`, `lock.h`.
- Headless editor tests: `tests/suites/pipeline_test.cpp` (`TmapDoc`, `BinDoc` sections).
