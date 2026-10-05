# Phosphorus Graphics Engine — The Complete Guide

> `engine/render/` — one 2D-intent API, four interchangeable backends, one golden reference.
> This is the practical companion to [docs/03-rendering.md](03-rendering.md) (the original
> design doc): everything you need to **run** the graphics engine, understand **how it works
> as implemented**, and know **what to do** when using or extending it.

---

## 1. What it is

The graphics engine renders 2D scenes — tilemaps, sprites, parallax backdrops, text — through
a single API that compiles, unchanged, to four very different machines:

| Backend | File | Target | Render tier |
|---|---|---|---|
| **Software** | `engine/render/src/soft/soft_renderer.cpp` | any (CPU framebuffer) | reference |
| **GBA PPU** | `engine/render/src/gba/gba_ppu.cpp` + `ppu_model.h` | GBA Mode-0 tiles + OAM | 0 |
| **PSP GU** | `engine/render/src/gu/gu_backend.cpp` + `gu_model.h` | sceGu sprite quads | 1 |
| **OpenGL** | `engine/render/src/gl/gl_backend.cpp` | PC GPU (GL 1.1, no loader libs) | 2 |

Exactly **one backend is linked per build** — there is no runtime backend switch and no
`#ifdef PLATFORM` in the front end. The API is deliberately abstracted at the *2D-intent*
level (sprites, tilemaps, parallax, palettes, camera) because that is the highest level
honestly common to a GBA PPU and a modern GPU. See docs/03 §9 for why this beats a generic
RHI: the GBA has no programmable pipeline to abstract over.

The **software backend is the golden reference**. Every other backend is validated against
its output — headlessly (the PPU/GU model tests), on a real GPU (`make gl-verify`,
pixel-exact `glReadPixels` diff), and on emulated silicon (mGBA VRAM/OAM inspection,
PPSSPP eDRAM readback).

---

## 2. How to run it

### Headless (no display, no SDL — works anywhere `g++` does)

```bash
make render      # software-rasterize a test scene, assert pixels, write build/render_out.ppm
make ppu         # GBA PPU backend: RGBA8 → 4bpp tiles + OAM → compose; assert vs soft golden
make gu          # PSP GU backend: sprite-quad model → compose; asserts bit-identical to soft
make texcache    # drive the Renderer through the budget-bounded LRU texture cache
make determinism # both scalar tiers (float / fixed16) must produce a byte-identical frame
```

Each target builds and runs one self-checking binary; expected output is `... PASS`.
`make render` also writes `build/render_out.ppm` — open it with any image viewer to *see*
the rendered frame.

### Windowed (needs SDL2; `make gl` also needs libGL)

```bash
make sdl         # the full example game, software-rendered into a real SDL window
make gl          # the same game, GPU-rendered through the OpenGL backend
make tmap        # phxtmap — the tilemap editor, rendered by this same engine
```

Controls in the game: arrows/WASD move, Z/A jumps, Enter = Start (menu), Select toggles the
frame-profiler overlay. For an unattended smoke run of any windowed binary:

```bash
PHX_MAX_FRAMES=120 ./build/platformer_sdl    # runs 120 frames, exits cleanly
```

### Verifying the desktop backends against the golden reference

```bash
make sdl-verify  # render the test scene through a real SDL window, read back, diff vs soft
make gl-verify   # render through the real GPU, glReadPixels, diff vs soft — pixel-exact
```

### On console targets (needs devkitARM / pspsdk)

```bash
make gba-platformer       # full game, software rasterizer, as a .gba ROM
make gba-platformer-ppu   # full game on real PPU hardware (Mode-0 BGs + OAM)
make psp / make psp-gu    # PSP EBOOT: software / native sceGu display list
```

Load the ROM in mGBA or the EBOOT in PPSSPP. The backends are verified by mGBA GDB-stub VRAM/OAM
inspection and a PPSSPP on-device readback (`GU_VERIFY_PASS`) against the software golden.

---

## 3. How it works

### 3.1 Two-piece architecture: front end + backend seam

```
 game / UI / editors
        │  draw_sprite() · draw_tilemap() · set_tilemap_parallax() · camera
        ▼
 Renderer (front end, renderer.cpp — backend-agnostic, ~125 lines)
   · records sprites into a fixed per-frame array (sized to caps.max_sprites)
   · applies camera shake + computes parallax scroll (Q16 integer math)
   · sorts sprites by (layer, z, tex) with qsort — batches stay texture-adjacent
        │  IRenderBackend (engine/render/src/backend.h — internal, virtual)
        ▼
 ONE linked backend: soft | gl | gu | gba
   · owns texture/tilemap slots, translates draws to its native model
   · writes the platform framebuffer (soft/ppu/gu compose) or drives the GPU (gl)
        │
        ▼
 platform present()  (SDL texture stream / GL swap / GBA VBlank / PSP sceGu)
```

The seam is `IRenderBackend` (`upload_tex`, `free_tex`, `upload_map`, `set_scroll`, `begin`,
`draw_tilemap`, `submit_sprites`, `end`, `stats`). Each backend translation unit defines
`phx_make_render_backend()`; the build links exactly one. Adding intent-level features in
the **front end** means every backend inherits them for free — this is how zoom, shake, and
parallax were added without touching backend seams (see §3.4).

### 3.2 The frame pipeline

```cpp
r->begin_frame(cam);        // stash (possibly shaken) camera, reset sprite list, backend clear
r->draw_tilemap(map, 0);    // backgrounds draw immediately, in call order
r->draw_tilemap(map, 1);
r->draw_sprite(s);          // sprites are RECORDED (no work yet), up to caps.max_sprites
...
r->end_frame();             // qsort by (layer<<24 | z<<16 | tex) → backend submit → present
```

Draw order contract: **tilemap layers first, in the order you call them; sprites always on
top**, ordered by `layer`, then `z`, then texture. Past the sprite ceiling, `draw_sprite`
drops the sprite and counts it in `stats().sprites_dropped` — the 128-sprite GBA OAM limit
is an *honest API limit*, enforced gracefully on every tier, not a GBA-only surprise.

### 3.3 The resource model

- **Textures** (`TextureDesc`) carry decoded pixels in one of the baked encodings
  (`PixelFormat`, docs/06 §4): plain RGBA8, tier-0 `PAL4_TILES` (4bpp paletted tiles —
  the PPU consumes them natively, the soft tier samples them through their palettes), or
  tier-1 swizzled RGBA8 (the GU binds it zero-copy with the swizzle bit). The software
  tier points at your pixels **zero-copy** (they must outlive the texture); GL uploads
  RGBA8 via `glTexImage2D`; the PPU backend still quantizes plain RGBA8 at upload.
  `load_texture`/`unload_texture` recycle slots (256 max per backend) — this is what the
  budget-bounded LRU `TextureCache` (`phx/render/texture_cache.h`) drives: key it with an
  asset NameHash, hand it pixels only on a miss, and it evicts least-recently-used entries
  to stay under a byte budget. On GBA everything fits and nothing evicts.
- **Tilemaps** (`TilemapDesc`) are retained: `uint16` indices (**0 = empty, value = tile
  index + 1**), laid out `[layer][y*width + x]`, referencing a tileset atlas packed
  left-to-right, top-to-bottom. Upload once, then scrolling is two integers per frame —
  free on GBA (per-BG HOFS/VOFS registers), cheap everywhere.

### 3.4 Camera: pan, zoom, shake — and why the math looks odd

`Camera2D { pos, zoom, shake }`. These are **tier-exact, cross-backend features implemented
in the front end** and they follow the engine's determinism law (same pixels on the float
tier and the GBA fixed16 tier):

- **Pan** — every backend subtracts camera pos from world positions.
- **Zoom** — a single Q16.16 factor via `s_to_q16()` (`core/math.h`). Backends map each
  camera-relative coordinate with `zsc(v) = (int64(v) * zoom_q16) >> 16` and compute dest
  rects as the **difference of two scaled edges** (`zsc(x0+w) − zsc(x0)`), so adjacent tiles
  share exact edges (no seams) and `zoom == 1` is a bit-exact identity. The **GBA PPU
  ignores zoom** and renders 1:1 — a text BG plus plain OBJs cannot scale; free zoom needs
  the opt-in affine path (docs/03 §4), which is out of MVP scope.
- **Shake** — a deterministic jitter of magnitude `shake` pixels, applied in
  `begin_frame` by cycling a fixed 8-entry offset table keyed on an internal frame counter.
  No RNG, so replays and both scalar tiers reproduce it exactly; `shake == 0` is a no-op.

**The rule this encodes:** cross-tier-exact math is done in Q16 *integer* arithmetic
(`s_to_q16` / `s_from_q16`), never in float, so `float` and `fixed16` builds compute the
identical int32. Follow it for anything new that affects pixels (the `determinism` gate will
catch you if you don't).

### 3.5 Per-layer parallax

```cpp
r->set_tilemap_parallax(map, layer, fx, fy);   // factor per axis, layers 0..3
```

`1` = moves with the world (default), `0` = screen-fixed (sky), `½` = classic half-speed
backdrop. Implemented in the front end: a layer with factor *f* draws as if scrolled by
`base + (f−1)·camera`, recomputed in Q16 before each `draw_tilemap` call, so differently
factored layers of one map compose correctly and backgrounds shake at their own depth. On
GBA this compiles to exactly the free per-BG scroll-register trick. Only layers 0–3 have
factor slots (the GBA 4-BG ceiling); higher layers draw at 1:1.

In the example platformer, parallax factors come from the Tiled map (per-layer custom
properties) via the resource pipeline — nothing is hardcoded.

### 3.6 Capability tiers size everything

`phx::caps()` (`core/caps.h`) is compile-time truth, selected by the one `PHX_TARGET_*`
define:

| | GBA (tier 0) | PSP (tier 1) | PC (tier 2) |
|---|---|---|---|
| `max_sprites` | **128** (OAM) | 1024 | 16384 |
| `max_entities` | 512 | 8192 | 65536 |
| RAM budget | 224 KB EWRAM | 24 MB | 256 MB |

`Renderer::create` sizes its per-frame sprite array from `caps().max_sprites`, out of the
caller's arena — **zero heap allocation on the frame path**, ever. Game code reads caps to
budget; the renderer enforces the ceiling.

---

## 4. What to do — using the API

Minimal complete program (condensed from `tests/render_test.cpp`, which runs headless via
the null platform):

```cpp
#include "phx/platform/platform.h"
#include "phx/render/renderer.h"
#include "phx/core/caps.h"
using namespace phx;

// 1. platform (the one linked backend: null = headless CPU framebuffer)
const phx_platform* plat = phx_platform_get();
phx_platform_desc desc{}; desc.title = "demo"; desc.width = 64; desc.height = 48;
plat->init(&desc);

// 2. renderer, carved from an arena (no hidden allocation)
static uint8_t buf[8 << 20];
ArenaAllocator arena; arena.init(buf, sizeof(buf));
Renderer* r = Renderer::create(plat->gfx(), arena, caps()).unwrap();

// 3. upload a tileset texture + a tilemap that references it
TextureDesc tsd{}; tsd.pixels = tileset_rgba8; tsd.width = 16; tsd.height = 8;
TextureId ts = r->load_texture(tsd);
static const uint16_t idx[4 * 3] = { 1,2,1,2, 2,1,2,1, 1,2,1,2 };  // 0 = empty!
TilemapDesc md{}; md.indices = idx; md.width = 4; md.height = 3;
md.tile_w = 8; md.tile_h = 8; md.tileset = ts;
TilemapId map = r->upload_tilemap(md);

// 4. one frame
Camera2D cam{};                       // optionally: cam.zoom, cam.shake, cam.pos
r->begin_frame(cam);
r->draw_tilemap(map, 0);
DrawSprite s{}; s.tex = hero_tex; s.sx = 0; s.sy = 0; s.sw = 8; s.sh = 8;
s.pos = vec2{ s_from_int(20), s_from_int(20) }; s.layer = 1;
r->draw_sprite(s);
r->end_frame();
```

In a real game you never do steps 1–2 yourself: the **App loop owns the Renderer**
(`app.render()`, see `engine/runtime/`), and assets arrive decoded from a `.phxp` bundle
through the `ResourceCache`, not from inline arrays. `examples/platformer/src/systems.cpp`
(`camera_system`, `draw_world_sprites`, the render phase around line 280) is the canonical
consumer to copy from.

Practical notes:

- Positions are `phx::scalar` — construct with `s_from_int(px)`; never assume `float`.
- `DrawSprite.dw/dh = 0` means "no scaling" (dest = source size); UI bars/panels set them.
- `flags` supports `kFlipX`, `kFlipY`, `kBlend`; `tint` is a per-channel multiply
  (white = unchanged). Blend modes are limited on GBA — don't design around rich blending.
- **Text is not special**: a bitmap font is a texture atlas and glyphs are sprites. The UI
  module emits `DrawSprite`s; there is no separate text renderer on purpose (docs/03 §7).
- Watch `r->stats()` (`sprites_submitted/dropped`, `tiles_drawn`, `batches`) — the in-game
  profiler overlay (Select) surfaces frame phases; dropped sprites mean you blew the tier
  budget.

---

## 5. Backend specifics (as implemented)

### Software (`src/soft/`) — the golden reference
CPU rasterizer into the platform's RGBA8 framebuffer (`phx_gfx_soft_lock`). No GPU, runs
headless; produces the reference output every other backend is diffed against. 256 texture
slots with free-list recycling, 32 tilemap slots. Zero-copy source pixels; samples every
baked texture encoding in place (RGBA8, tier-0 `PAL4_TILES` through its palettes, tier-1
swizzled RGBA8 — docs/06 §4), so the GBA/PSP *software* tiers run their consoles' bundles
without an expansion buffer.

### OpenGL (`src/gl/`)
A deliberate GL 1.1 immediate-mode port of the software rasterizer's *geometry* — textured,
tinted quads, ortho pixel projection, nearest filtering, alpha blend. No glad/glew/GLAD —
just libGL. Selected by `PHX_HAVE_SDL` + `PHX_HAVE_GL`; the SDL platform then creates a GL
context and swaps buffers. Pixel-verified against the soft golden on a real GPU
(`make gl-verify`). A modern-GL (3.3 core) / Vulkan backend is a **decided non-goal** —
docs/03 §6 records the decision; maximum compatibility won for the one tier with
performance to spare.

### GBA PPU (`src/gba/`)
Speaks the hardware's actual language: **4bpp paletted 8×8 tiles + 16-color BGR555 palette
banks**. Tier-0 bundles arrive pre-quantized (`PAL4_TILES`, baked by
`tools/phxpack/tex_encode.h` with the same quantizer — upload just claims palette banks
and remaps nibbles); RGBA8 uploads (tests, un-bakeable art) are quantized at upload, and
both paths compose the identical frame (`make ppu` asserts it). Tilemaps become Mode-0
text-BG screen entries (32×32 cells = one screenblock, 256-tile char store = charblock
budget) and sprites become OAM entries (≤128). `ppu_compose` (`ppu_model.h`) then produces the exact frame the
PPU would scan out — **on the CPU, on any host** — so every hardware constraint (palette
overflow, unaligned art, sprite ceiling) surfaces in a headless test. On real hardware
(`PHX_GBA_HW`), `submit_hardware()` DMAs the same tile/map/OAM/palette data into
VRAM/OAM/PALRAM and the silicon scans it out; the model remains its golden oracle, verified
byte-for-byte over the mGBA GDB stub. Zoom is ignored (1:1); scroll and 4-layer parallax
are register-cheap.

### PSP GU (`src/gu/`)
Records sprite quads (`gu_model.h`) and composes them **bit-identically to the software
reference** (`make gu` asserts this). On hardware, `submit_gu()` emits a real
`sceGuDrawArray(GU_SPRITES, …)` display list — verified on PPSSPP by an on-PSP eDRAM
readback matching the model (`GU_VERIFY_PASS`).

---

## 6. Testing & verification — what proves it works

| Gate | Command | What it proves |
|---|---|---|
| Soft golden | `make render` | reference scene rasterizes to asserted pixels (incl. zoom/shake cases) |
| PPU model | `make ppu` | quantize→tiles→OAM→compose matches expectations on both scalar tiers |
| GU model | `make gu` | GU compose bit-identical to the soft reference |
| Texture LRU | `make texcache` | budget eviction, slot recycling across 1000 cycles, oversize reject |
| Cross-tier | `make determinism` | float and fixed16 builds render a **byte-identical frame** |
| Real SDL/GL | `make sdl-verify` / `gl-verify` | real window / real GPU output matches the soft golden |
| Real consoles | `make gba-platformer-ppu`, `make psp-gu` | VRAM/OAM bytes (mGBA) and eDRAM pixels (PPSSPP) match the models |
| Memory safety | `make sanitize` | the whole suite, ASan+UBSan clean |

Run `make render` (or the relevant suite) plus `make determinism` after **any** change that
touches pixels or coordinates; run `make check` before calling work done. The unit harness
is `tests/phx_test.h`; render integration tests are standalone binaries
(`tests/render_test.cpp`, `ppu_test.cpp`, `gu_test.cpp`, `window_verify.cpp`).

---

## 7. Extending the graphics engine

**Adding a visual feature** (order matters):
1. If it can be expressed as camera/scroll/sprite-list math, put it in the **front end**
   (`renderer.cpp`) in Q16 integer form — all four backends inherit it, both tiers stay
   identical. Zoom, shake, and parallax are the worked examples.
2. Only if a backend must participate, extend `IRenderBackend` — then implement it in **all
   four** backends (or document an honest degradation, like PPU-ignores-zoom).
3. Add cases to the soft golden first; the other backends are then verified against it.
4. Run `make render ppu gu`, `make determinism`, `make check`; `make gl-verify` if you have
   a display; rebuild the console targets if you touched a backend they link.

**Adding a backend** (e.g. Vulkan, DS — see docs/09 §4): one new folder
`engine/render/src/<name>/` defining `phx_make_render_backend()`; link it *instead of* the
others (Makefile source-list swap / `phx_add_module(... BACKENDS ...)` in CMake). Start
from the nearest tier's backend, and validate against the soft golden before anything else.

**Rules that are enforced, not advisory:**
- No platform/OS/SDK headers outside backend folders; no `#ifdef PLATFORM` in shared render
  code. `make depcheck` breaks the build on layering violations (render is L2: it may use
  core/memory/platform only).
- Guard *hardware* register/MMIO code with `PHX_GBA_HW`, never `PHX_TARGET_GBA` — the
  latter is also set by the host `TIER=gba_sim` build, which must stay linkable without
  hardware symbols.
- No `new`/`malloc` on the frame path — allocate from the arena at create time.
- Tier-exact math is integer Q16; left-shifting negatives is UB (use multiplication — the
  sanitize gate checks).
- Zero warnings under `-Wall -Wextra -Wpedantic`, including MinGW and ASan+UBSan builds.

---

## 8. Known limits & future scope

- GBA PPU: no zoom (affine BG path is designed, opt-in, unimplemented — docs/03 §4), one
  shared 16-color palette per atlas, 256-tile char store, 32×32-cell BG, ≤128 sprites.
- Front-end ceilings: 32 tilemaps, 4 parallax-factor layers per map, 256 texture slots per
  backend, `caps().max_sprites` per frame.
- The sprite sort is `qsort` on every tier (the docs/03 radix-on-console idea was not
  needed at current scene sizes).
- Roadmap (docs/09): 2.5D (depth/billboards/affine GBA) in v0.2, palette/blend FX in v0.3,
  job-system render pass in v0.4, DS/Vita backends in v0.6. Modern-GL/Vulkan is a decided
  non-goal (docs/03 §6).

## 9. Related reading

- [docs/03-rendering.md](03-rendering.md) — the original design rationale (RHI trade-off,
  backend mapping table, GBA/PSP hardware notes)
- [docs/00-architecture.md](00-architecture.md) — layering, seams, determinism law
- [docs/05-memory.md](05-memory.md) — the arena model the renderer allocates from
- [docs/06-resources.md](06-resources.md) — how baked textures/tilemaps reach the renderer
