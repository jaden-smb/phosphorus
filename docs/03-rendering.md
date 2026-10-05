# Phosphorus Engine — Rendering

> `engine/render/` — one public API, four backends: GBA PPU, PSP GU, PC GL, and a
> software rasterizer (the golden reference every other backend is diffed against; see
> `docs/graphics-engine.md` §5 for the as-built version of this document). The PC tier's
> design **is** the GL 1.1 immediate-mode port of the software rasterizer — a modern-GL
> (3.3 core) / Vulkan backend was considered early on and has been **dropped as a
> non-goal** (§6): at this engine's scene sizes draw-call count is never the bottleneck,
> and a second PC GPU path would only widen the maintenance matrix.
> The hard part: the GBA has **no framebuffer-blit model** in its fast path — it has a
> tile/sprite *PPU* with 128 hardware sprites and 4 background layers. The API is
> therefore shaped around **sprites and tilemaps**, which map *up* to GL/GU trivially
> but cannot be faked *down* onto the PPU. We design to the constraint, not around it.

## 1. The unified API (`phx/render/renderer.h`)

The renderer is **retained for tilemaps, immediate for sprites** — the combination
that every 2D console actually used.

```cpp
namespace phx {

struct DrawSprite {            // POD, batched per frame
    TextureId tex;
    int16_t   sx, sy, sw, sh;  // source rect in atlas (texels)
    vec2      pos;             // world position (top-left)
    uint16_t  flags;           // flip-x/y, priority/layer, blend
    uint8_t   palette;         // GBA/paletted; ignored on RGBA backends
    uint8_t   z;               // intra-layer sort key
};

struct Camera2D { vec2 pos; scalar zoom; int16_t shake; };

class Renderer {
public:
    static Result<Renderer*> create(phx_gfx*, ArenaAllocator&, const phx_caps&);

    void begin_frame(const Camera2D&);   // camera zoom + deterministic shake applied here
    // tilemaps: retained handle, uploaded once, scrolled cheaply
    TilemapId upload_tilemap(const TilemapDesc&);
    void      set_tilemap_scroll(TilemapId, vec2 px);
    // per-layer camera factor (1 = world, ½ = half-speed background, 0 = fixed sky);
    // implemented in the front end over the scroll seam with Q16 integer math, so every
    // backend inherits it tier-exactly — on GBA it is the free per-BG HOFS/VOFS trick (§4)
    void      set_tilemap_parallax(TilemapId, uint8_t layer, scalar fx, scalar fy);
    void      draw_tilemap(TilemapId, uint8_t layer);
    // sprites: immediate, batched & sorted by (layer, z, tex)
    void      draw_sprite(const DrawSprite&);
    // text & ui quads go through the same sprite batcher (font = atlas)
    void      end_frame();     // flush → backend submit → present

    TextureId load_texture(const TextureDesc&);   // from resource cache blob
    void      unload_texture(TextureId);          // slot recycling (drives the LRU cache)
    const RenderStats& stats() const;
};
} // namespace phx
```

Gameplay calls `draw_sprite`/`draw_tilemap`. It never sees GL/GU/PPU. The backend
decides how those become reality.

## 2. Backend mapping table

All three columns below are **as implemented**. The PC backend is a GL 1.1 immediate-mode
port of the software rasterizer's geometry — `glBegin(GL_QUADS)` per sprite/tile, no
instancing, no shaders, no VAOs — pixel-verified against the software golden
(`make gl-verify`), and that's the bar that matters (§6 on why there is deliberately no
modern-GL/Vulkan path).

| API concept       | GBA PPU backend                 | PSP GU backend            | PC GL backend            |
|-------------------|----------------------------------|---------------------------|--------------------------|
| `Tilemap` layer   | hardware text BG, visible window streamed into a screenblock | one textured quad per non-empty cell, GU display list | `glBegin(GL_QUADS)` per cell |
| `set_scroll`      | `REG_BGxHOFS/VOFS` write (free!) | quad dest offsets         | quad dest offsets        |
| `draw_sprite`     | OAM entry (≤128) + OBJ tile run  | `sceGuDrawArray(GU_SPRITES)` quad | textured quad     |
| `Texture`         | **4bpp paletted tiles + palettes, baked at pack time** (`--target 0`, docs/06 §4); upload claims palette banks + remaps nibbles. RGBA8 uploads still quantize at upload (tests, un-bakeable art) | **swizzled RGBA8, baked at pack time** (`--target 1`); the GU samples the blob zero-copy with the swizzle bit set | RGBA8 via `glTexImage2D` |
| sort & batch      | priority bits + OAM order        | sort then one display list | sort, one draw per quad |
| blend             | alpha-test cutout (PPU semantics) | alpha-test cutout (GU)   | fixed-function `glBlendFunc` |

The **sprite ceiling is the API's honest limit**: `phx_caps::max_sprites` is 128 on
GBA. `draw_sprite` past the ceiling on GBA drops the lowest-priority sprite and
increments `RenderStats::dropped`. On PC the ceiling is effectively unbounded. The
*game* reads caps to budget; the *renderer* enforces gracefully.

## 3. The frame pipeline (backend-agnostic front end)

```
 begin_frame(cam)
   └─ stash camera, reset sprite batch on frame stack (StackAllocator)
 draw_tilemap(id, layer)         ──┐
 draw_sprite(...) × N             ─┤   recorded into a transient command list
 draw_sprite(...) (text/UI)       ──┘   (no GPU work yet)
 end_frame()
   ├─ sort sprites: key = (layer<<24 | z<<16 | tex)   radix on console, std::sort PC
   ├─ backend->submit(cmdlist, cam)   ← the ONLY virtual-ish call per frame
   └─ platform->present()             ← vblank/swap
```

The command list lives on the **double-buffered frame stack** (see memory doc), so
the PSP GE can chew on frame N-1's display list while we build frame N — no copy, no
malloc.

### Backend interface (internal, not public)

```cpp
struct IRenderBackend {                       // resolved at create(), not per-call
    void (*submit)(void* self, const FrameCmds&, const Camera2D&);
    TextureId (*upload_tex)(void* self, const TextureDesc&);
    TilemapId (*upload_map)(void* self, const TilemapDesc&);
    void (*destroy)(void* self);
    void* self;
};
```

One indirect call per frame, not per sprite. On GBA `submit` is the OAM/VRAM/DMA
update; on PSP it builds one GE display list; on PC it fills instance buffers and
issues a handful of draw calls.

## 4. GBA backend specifics (`src/gba/`)

The PPU is the engine here. Notes that shape the design:

- **Video modes:** Mode 0 (4 text BGs, tiled) is the workhorse for 2D; Mode 4
  (bitmap, paletted) available for effects/intros. Phosphorus defaults to Mode 0.
- **Tiles:** 8×8, 4bpp (16 colors/palette) → 32 bytes/tile. Tilemaps reference tile
  indices; `upload_tilemap` writes the screen-base map and char-base tiles to VRAM.
- **Scroll is free:** `set_tilemap_scroll` is two register writes — parallax via 4 BG
  layers at different scroll rates costs nothing. This is the GBA's superpower and the
  API exposes it directly.
- **Sprites (OBJ):** 128 OAM entries; `draw_sprite` allocates an OAM slot, points it
  at VRAM OBJ tiles, sets attributes (pos/flip/priority/palette). VRAM OBJ tile budget
  (≈32 KB) is the real ceiling, tracked by the backend.
- **DMA** copies tiles/palette during VBlank to avoid tearing.

> 2.5D on GBA: Mode 7-style affine BG (one affine background, `REG_BG2PA..PD`) gives
> the "pseudo-3D floor" look (F-Zero/racers). `draw_tilemap` with an affine layer +
> per-scanline matrix (HBlank IRQ) is a supported, opt-in path, not the default.

## 5. PSP backend specifics (`src/gu/`)

- `sceGuInit`, double-buffered VRAM, depth/dither off for pure 2D (2.5D enables them).
- Textures are **swizzled RGBA8, baked at pack time** (`phxpack --target 1`, docs/06 §4):
  the bake reorders texels into the GU's 16-byte × 8-row block layout and the backend
  binds the blob zero-copy with `sceGuTexMode(..., swizzle=1)` — better GE texture-cache
  behaviour for free. Swizzling is a pure reorder, so the GU stays **bit-identical to the
  software golden** (the host `gu_compose` model samples the swizzled layout directly).
  Sizes the GU can't swizzle (width % 4 or height % 8) honestly stay linear RGBA8, which
  the backend also accepts.
- One display list per frame built into the frame stack; `sceGuFinish`/`sceGuSync`.
- 2.5D: the same sprite quads but with real Z and a perspective/ortho `sceGumMatrix`,
  enabling billboards, layered parallax with depth, and simple textured 3D props.

## 6. PC backend specifics (`src/gl/`)

`engine/render/src/gl/gl_backend.cpp` is a deliberate **GL 1.1 immediate-mode** port of
the software rasterizer's geometry — `glBegin(GL_QUADS)` / `glTexCoord2f` / `glVertex2f`
per sprite or tile, nearest filtering, fixed-function alpha blend. No GL loader
(glad/glew) — it links straight against libGL, which is enough for a 1.1 context on every
desktop GL driver, current or ancient. Chosen for maximum compatibility over throughput,
since this engine's 2D scene sizes never come close to making draw-call count the
bottleneck. Pixel-verified against the software golden on a real GPU (`make gl-verify`).

**Modern-GL / Vulkan is a non-goal, dropped from the design.** An early sketch called for
a GL 3.3 core backend (instanced sprite quads) with an optional Vulkan path. It was never
built, nothing has needed it, and a second PC GPU path would double the PC render surface
to optimize the one tier that has performance to spare. The decision is recorded here so
it stays made; if a future project actually saturates the GL 1.1 path, that project can
reopen it with numbers (`docs/09` lists it under non-goals).

The **software backend** (`src/soft/`) runs headless CI / extreme low-end and is the
golden reference every backend (including this GL 1.1 one) is diffed against.

## 7. Text & UI share the sprite path

There is no separate text renderer. A bitmap font is a texture atlas; glyphs are
sprites; the UI module emits `DrawSprite`s. This keeps the backend count of code
paths minimal — crucial for four platforms — and means GBA text is just OBJ/BG tiles,
which is how GBA games actually do text.

## 8. Conformance & golden images

The render suite (`tests/render_test.cpp` + the ppu/gu suites; `make render ppu gu`,
and `make gl-verify` on a real GPU) renders a fixed scene (tilemap + sprites + text) through
the **software backend** and diffs against a checked-in golden PNG. GL/GU backends are
spot-checked on hardware/emulator (mGBA, PPSSPP) in CI with perceptual tolerance. This
catches "the GBA flips Y but GL doesn't" classes of bugs that otherwise ship.

## 9. Why not a generic command-buffer/RHI like bgfx?

A full RHI assumes a programmable pipeline. The GBA has none. Forcing the PPU into an
RHI abstraction would either (a) cripple the PC backend to PPU semantics, or (b) make
the GBA backend a lie. Phosphorus instead abstracts at the **2D-intent layer** (sprites,
tilemaps, parallax, palettes) — the highest level that is *honestly* common to all
four machines. That is the single most important rendering decision in the engine.
