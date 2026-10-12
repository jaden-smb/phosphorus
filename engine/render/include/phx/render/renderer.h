// phx/render/renderer.h — ONE 2D-intent API, multiple backends (software / GL / GU / PPU).
// Abstracted at the highest level honestly common to all four machines: sprites,
// tilemaps, parallax, palettes. The PPU's 128-sprite ceiling is an honest API limit,
// not an afterthought. See docs/03-rendering.md.
#ifndef PHX_RENDER_RENDERER_H
#define PHX_RENDER_RENDERER_H

#include "phx/core/types.h"
#include "phx/core/math.h"
#include "phx/core/caps.h"
#include "phx/core/pixel.h"
#include "phx/memory/allocators.h"

struct phx_gfx; // opaque platform graphics device

namespace phx {

// Opaque texture handle; the PPU uses slot + generation to reject stale references.
using TextureId = uint32_t;
using TilemapId = uint16_t;
constexpr TextureId kNoTexture = 0xFFFFFFFFu;
constexpr TilemapId kNoTilemap = 0xFFFF;

enum SpriteFlags : uint16_t {
    kFlipX = 1 << 0,
    kFlipY = 1 << 1,
    kBlend = 1 << 2,         // additive/alpha (limited modes on GBA)
};

// Rgba / rgba() / PixelFormat now live in phx/core/pixel.h (shared with resource).

// POD draw record, batched per frame. Same struct on every backend.
struct DrawSprite {
    TextureId tex;
    int16_t   sx, sy, sw, sh;   // source rect in the texture/atlas (texels)
    vec2      pos;              // world position (top-left)
    uint16_t  flags = 0;        // SpriteFlags
    uint8_t   layer = 0;        // coarse ordering (background..foreground)
    uint8_t   z     = 0;        // intra-layer sort key
    int16_t   dw    = 0;        // dest size; 0 => use sw/sh (no scaling). UI bars/panels scale.
    int16_t   dh    = 0;
    Rgba      tint  = rgba(255, 255, 255);   // per-channel multiply; white = unchanged
};

struct Camera2D {
    vec2    pos   {};
    scalar  zoom  = s_from_int(1);   // view scale about the camera origin (1 = 1:1). Honoured by
                                     // soft/GL/GU; the GBA PPU renders 1:1 (free zoom needs the
                                     // opt-in affine path, docs/03 §4) and ignores it.
    int16_t shake = 0;               // screen-shake magnitude in pixels (deterministic jitter,
                                     // applied by the renderer front end; 0 = none).
};

// A texture is a decoded pixel rectangle. For RGBA8 the renderer points at `pixels`
// directly (zero-copy). The resource loader fills these from a baked blob header; tests
// supply them inline.
struct TextureDesc {
    const void* pixels = nullptr;
    uint32_t    size   = 0;
    uint16_t    width  = 0;
    uint16_t    height = 0;
    PixelFormat format = PixelFormat::RGBA8;
};

// A tilemap references a tileset texture and holds uint16 tile indices (0 = empty),
// laid out [layer][y*width + x]. Tiles are tile_w x tile_h; the tileset atlas packs them
// left-to-right, top-to-bottom.
struct TilemapDesc {
    const uint16_t* indices = nullptr;   // width*height*layers entries
    uint16_t    width  = 0;              // in tiles
    uint16_t    height = 0;
    uint8_t     layers = 1;             // <= 4 honored on GBA
    uint8_t     tile_w = 8;
    uint8_t     tile_h = 8;
    TextureId   tileset = kNoTexture;
};

struct RenderStats {
    uint32_t sprites_submitted = 0;
    uint32_t sprites_dropped   = 0;     // > caps.max_sprites (the honest PPU ceiling)
    uint32_t tiles_drawn       = 0;
    uint32_t batches           = 0;
};

struct IRenderBackend;   // internal (engine/render/src/backend.h)

class Renderer {
public:
    static Result<Renderer*> create(phx_gfx*, ArenaAllocator&, const Caps&);

    void begin_frame(const Camera2D&);

    // Tilemaps: retained for the renderer arena lifetime; caller keeps indices alive.
    // On the PPU, maps pin their tilesets even after unload_texture(). Scrolling is cheap.
    TilemapId upload_tilemap(const TilemapDesc&);
    void      set_tilemap_scroll(TilemapId, vec2 px);
    // Parallax: layer `layer` of this map scrolls at `factor` × the camera, per axis.
    // 1 = moves with the world (default), 0 = fixed to the screen (a sky layer), ½ = classic
    // half-speed background. Applied in the front end through the scroll seam, so every
    // backend inherits it — on GBA this is exactly the free per-BG HOFS/VOFS trick
    // (docs/03 §4). Layers 0..3 only (the GBA 4-BG ceiling); higher layers draw at 1:1.
    // All-integer Q16 math (like zoom), so both scalar tiers shift by the identical pixel.
    void      set_tilemap_parallax(TilemapId, uint8_t layer, scalar fx, scalar fy);
    void      draw_tilemap(TilemapId, uint8_t layer);
    // Signal that a retained tilemap's cell indices were mutated in place since upload (the
    // caller owns that buffer). The software backend reads it live so this is a no-op there;
    // the GBA PPU re-streams the cached BG window. Enables a per-frame-rebuilt map (spectrogram).
    void      refresh_tilemap(TilemapId);

    // Sprites: recorded, then sorted by (layer, z, tex) and drawn on top at end_frame.
    void      draw_sprite(const DrawSprite&);

    void      end_frame();            // sort -> backend submit -> present

    TextureId load_texture(const TextureDesc&);
    // Release uploader ownership. The PPU reclaims tiles and unused palette slots unless
    // a retained map pins this texture. PPU stale handles and double releases are no-ops;
    // kNoTexture is safe on every backend. TextureId is opaque: never index storage with it.
    void      unload_texture(TextureId);
    uint16_t  live_textures() const { return live_tex_; }   // live backend resources (including PPU map pins)
    const RenderStats& stats() const { return stats_; }
    const Camera2D&    camera() const { return cam_; }   // the camera of the last begin_frame

private:
    Renderer() = default;
    friend class ArenaAllocator;   // so arena.make<Renderer>() can construct it

    static constexpr uint16_t kMaxTilemaps   = 32;  // matches the backends' map-slot caps
    static constexpr uint8_t  kParallaxLayers = 4;  // per-layer factor slots (GBA 4-BG ceiling)
    static constexpr int32_t  kQ16One        = 1 << 16;
    // Per-map scroll base + per-layer parallax factors (Q16). Lives in the front end so the
    // backend seam stays unchanged — parallax compiles to plain set_scroll calls per draw.
    struct MapScroll {
        vec2    base{};
        int32_t fx_q16[kParallaxLayers] = { kQ16One, kQ16One, kQ16One, kQ16One };
        int32_t fy_q16[kParallaxLayers] = { kQ16One, kQ16One, kQ16One, kQ16One };
    };

    IRenderBackend* be_      = nullptr;
    ArenaAllocator* arena_   = nullptr;
    Camera2D        cam_     {};
    MapScroll       scroll_[kMaxTilemaps] {};
    DrawSprite*     sprites_ = nullptr;     // per-frame list (fixed, sized to caps.max_sprites)
    uint32_t        count_   = 0;
    uint32_t        cap_     = 0;
    uint16_t        live_tex_ = 0;          // count of outstanding textures (load - unload)
    uint32_t        frame_   = 0;           // frame counter (drives deterministic camera shake)
    RenderStats     stats_   {};
};

} // namespace phx
#endif // PHX_RENDER_RENDERER_H
