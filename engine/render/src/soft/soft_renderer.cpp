// engine/render/src/soft/soft_renderer.cpp — CPU rasterizer (render tier: software).
// Draws sprites and tilemaps into the platform's RGBA8 framebuffer (phx_gfx_soft_lock).
// This is the reference backend: it has no GPU dependency, runs headless, and produces
// the golden images the GL/GU/GBA backends are validated against (docs/03 §8).
#include "backend.h"
#include "phx/platform/gfx_soft.h"

namespace phx {
namespace {

constexpr uint16_t kMaxTextures = 256;
constexpr uint16_t kMaxTilemaps = 32;

// A bound texture in any of the baked encodings (docs/06 §4). The reference backend samples
// every per-target format directly from the blob (zero-copy, like the RGBA8 path always did)
// so the GBA/PSP software tiers can run their tier's bundles without an expansion buffer.
// `data` doubles as the slot-in-use marker; the PAL4 pointers alias the same payload.
struct Tex {
    const uint8_t*  data;          // blob payload (null = slot free)
    int32_t         w, h;
    PixelFormat     fmt;
    const uint16_t* pal;           // PAL4_TILES only: palettes / palette-per-tile / 4bpp tiles
    const uint8_t*  pal_of_tile;
    const uint8_t*  tiles;
};

// Texel fetch across formats. Returns an RGBA8 texel; 0 (alpha 0) = transparent.
inline uint32_t tex_fetch(const Tex& t, int x, int y) {
    switch (t.fmt) {
    case PixelFormat::RGBA8:
        return reinterpret_cast<const uint32_t*>(t.data)[y * t.w + x];
    case PixelFormat::RGBA8_SWZ:
        return reinterpret_cast<const uint32_t*>(t.data)[
                   swz_texel_index(uint32_t(x), uint32_t(y), uint32_t(t.w))];
    case PixelFormat::PAL4_TILES: {
        const uint32_t tile = uint32_t(y >> 3) * uint32_t(t.w >> 3) + uint32_t(x >> 3);
        const uint8_t  slot = pal4_texel(t.tiles, tile, uint32_t(x) & 7u, uint32_t(y) & 7u);
        if (slot == 0) return 0;   // transparent
        return bgr555_to_rgba8(t.pal[uint32_t(t.pal_of_tile[tile]) * 16u + slot]);
    }
    default:
        return 0;
    }
}
struct Map {
    const uint16_t* idx;
    int32_t  w, h;
    uint8_t  layers, tw, th;
    TextureId tileset;
    int32_t  scroll_x, scroll_y;   // pixels
};

class SoftBackend final : public IRenderBackend {
public:
    void init(phx_gfx* gfx, ArenaAllocator& a, const Caps&) {
        gfx_  = gfx;
        tex_  = a.alloc_array<Tex>(kMaxTextures);
        free_ = a.alloc_array<TextureId>(kMaxTextures);
        maps_ = a.alloc_array<Map>(kMaxTilemaps);
    }

    TextureId upload_tex(const TextureDesc& d) override {
        if (!d.pixels) return kNoTexture;
        Tex t{};
        t.data = static_cast<const uint8_t*>(d.pixels);
        t.w = int32_t(d.width); t.h = int32_t(d.height);
        t.fmt = d.format;
        switch (d.format) {
        case PixelFormat::RGBA8:
            break;
        case PixelFormat::RGBA8_SWZ:
            if (!swz_size_ok(d.width, d.height)) return kNoTexture;
            break;
        case PixelFormat::PAL4_TILES: {
            if (d.width == 0 || d.height == 0)            return kNoTexture;
            if ((d.width % 8) || (d.height % 8))          return kNoTexture;
            const TexturePal4Header& ph = *reinterpret_cast<const TexturePal4Header*>(t.data);
            if (ph.pal_count == 0)                        return kNoTexture;
            const uint32_t n_tiles = pal4_tile_count(d.width, d.height);
            t.pal         = reinterpret_cast<const uint16_t*>(t.data + pal4_palettes_off());
            t.pal_of_tile = t.data + pal4_tile_pal_off(ph.pal_count);
            t.tiles       = t.data + pal4_tile_data_off(ph.pal_count, n_tiles);
            break;
        }
        default:
            return kNoTexture;                             // PAL8: reserved, no consumer
        }
        TextureId id;
        if (free_n_ > 0)              id = free_[--free_n_];   // reuse a recycled slot
        else if (tex_count_ < kMaxTextures) id = tex_count_++; // grow the high-water mark
        else return kNoTexture;                                // genuinely full
        tex_[id] = t;
        return id;
    }

    bool free_tex(TextureId id) override {
        if (id >= tex_count_ || tex_[id].data == nullptr) return false; // out of range / double free
        tex_[id].data = nullptr;                                  // mark slot empty
        free_[free_n_++] = id;                                    // recycle the id
        return true;
    }

    TilemapId upload_map(const TilemapDesc& d) override {
        if (map_count_ >= kMaxTilemaps) return kNoTilemap;
        TilemapId id = map_count_++;
        maps_[id] = Map{ d.indices, int32_t(d.width), int32_t(d.height),
                         d.layers, d.tile_w, d.tile_h, d.tileset, 0, 0 };
        return id;
    }

    void set_scroll(TilemapId id, vec2 px) override {
        if (id >= map_count_) return;
        maps_[id].scroll_x = s_to_int(px.x);
        maps_[id].scroll_y = s_to_int(px.y);
    }

    void begin(const Camera2D& cam) override {
        fb_     = phx_gfx_soft_lock(gfx_);
        cam_x_  = s_to_int(cam.pos.x);
        cam_y_  = s_to_int(cam.pos.y);
        zq_     = s_to_q16(cam.zoom);
        if (zq_ <= 0) zq_ = 1 << 16;                   // zoom <= 0 is meaningless -> 1:1
        stats_  = RenderStats{};
        clear(rgba(30, 30, 46));                       // a calm slate background
    }

    // Scale a camera-relative pixel coordinate by the Q16.16 zoom. zoom==1 (zq_==1<<16) is an
    // EXACT identity for any int. Dest rects are taken as the difference of two scaled edges
    // (zsc(x0+w) - zsc(x0)) so adjacent tiles share an exact edge — no seams when zoomed.
    int zsc(int v) const { return int((int64_t(v) * zq_) >> 16); }

    void draw_tilemap(TilemapId id, uint8_t layer) override {
        if (id >= map_count_ || layer >= maps_[id].layers) return;
        const Map& m = maps_[id];
        if (m.tileset >= tex_count_) return;
        const Tex& ts = tex_[m.tileset];
        if (!ts.data) return;                          // tileset texture was freed
        const int cols = ts.w / m.tw;                  // tiles per atlas row
        if (cols <= 0) return;
        const uint16_t* layer_idx = m.idx + size_t(layer) * size_t(m.w) * size_t(m.h);

        for (int ty = 0; ty < m.h; ++ty) {
            for (int tx = 0; tx < m.w; ++tx) {
                uint16_t cell = layer_idx[ty * m.w + tx];
                if (cell == 0) continue;               // 0 = empty
                int t = cell - 1;
                int sx = (t % cols) * m.tw;
                int sy = (t / cols) * m.th;
                int wx = tx * m.tw - m.scroll_x - cam_x_;   // camera-relative world pixels
                int wy = ty * m.th - m.scroll_y - cam_y_;
                int dx = zsc(wx),          dy = zsc(wy);     // zoomed screen position
                int dw = zsc(wx + m.tw) - dx, dh = zsc(wy + m.th) - dy;   // zoomed size (edge diff)
                blit(dx, dy, ts, sx, sy, m.tw, m.th, dw, dh, false, false, rgba(255, 255, 255));
                ++stats_.tiles_drawn;
            }
        }
    }

    void submit_sprites(const DrawSprite* s, uint32_t n) override {
        for (uint32_t i = 0; i < n; ++i) {
            const DrawSprite& d = s[i];
            if (d.tex >= tex_count_ || tex_[d.tex].data == nullptr) continue;
            int wx = s_to_int(d.pos.x) - cam_x_;
            int wy = s_to_int(d.pos.y) - cam_y_;
            int ww = d.dw > 0 ? d.dw : d.sw;            // world dest size (UI scaling), pre-zoom
            int wh = d.dh > 0 ? d.dh : d.sh;
            int dx = zsc(wx),         dy = zsc(wy);
            int dw = zsc(wx + ww) - dx, dh = zsc(wy + wh) - dy;
            blit(dx, dy, tex_[d.tex], d.sx, d.sy, d.sw, d.sh, dw, dh,
                 (d.flags & kFlipX) != 0, (d.flags & kFlipY) != 0, d.tint);
        }
        ++stats_.batches;
    }

    void end() override { /* present handled by platform; nothing to unlock for null/CPU */ }

    RenderStats& stats() override { return stats_; }

private:
    void clear(Rgba c) {
        const int total = fb_.w * fb_.h;
        for (int i = 0; i < total; ++i) fb_.pixels[i] = c;
    }

    // Nearest-sample blit of a source rect into a dest rect (dw x dh), with optional flip,
    // per-channel tint, alpha test, and clipping. dest==source size is the common 1:1 path.
    void blit(int dx, int dy, const Tex& t, int sx, int sy, int sw, int sh,
              int dw, int dh, bool fx, bool fy, Rgba tint) {
        if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
        const bool mod = (tint != rgba(255, 255, 255));
        for (int y = 0; y < dh; ++y) {
            int py = dy + y;
            if (py < 0 || py >= fb_.h) continue;
            int soy = (dh == sh) ? y : (y * sh / dh);     // nearest source row
            int srcY = fy ? (sy + sh - 1 - soy) : (sy + soy);
            if (srcY < 0 || srcY >= t.h) continue;
            for (int x = 0; x < dw; ++x) {
                int px = dx + x;
                if (px < 0 || px >= fb_.w) continue;
                int sox = (dw == sw) ? x : (x * sw / dw); // nearest source col
                int srcX = fx ? (sx + sw - 1 - sox) : (sx + sox);
                if (srcX < 0 || srcX >= t.w) continue;
                uint32_t texel = tex_fetch(t, srcX, srcY);
                if ((texel >> 24) == 0) continue;          // fully transparent -> skip
                if (mod) {
                    uint32_t r = uint32_t(rgba_r(texel)) * rgba_r(tint) / 255;
                    uint32_t g = uint32_t(rgba_g(texel)) * rgba_g(tint) / 255;
                    uint32_t b = uint32_t(rgba_b(texel)) * rgba_b(tint) / 255;
                    texel = rgba(uint8_t(r), uint8_t(g), uint8_t(b), rgba_a(texel));
                }
                fb_.pixels[py * fb_.w + px] = texel;
            }
        }
    }

    phx_gfx*    gfx_ = nullptr;
    phx_soft_fb fb_  { nullptr, 0, 0 };
    int32_t     cam_x_ = 0, cam_y_ = 0;
    int32_t     zq_    = 1 << 16;     // camera zoom as Q16.16 (1<<16 == 1.0 == 1:1)

    Tex*       tex_  = nullptr;  uint16_t tex_count_ = 0;   // high-water mark of slots ever used
    TextureId* free_ = nullptr;  uint16_t free_n_   = 0;    // recycled-slot stack
    Map*       maps_ = nullptr;  uint16_t map_count_ = 0;
    RenderStats stats_ {};
};

} // namespace

IRenderBackend* phx_make_render_backend(phx_gfx* gfx, ArenaAllocator& arena, const Caps& caps) {
    SoftBackend* be = arena.make<SoftBackend>();
    if (!be) return nullptr;
    be->init(gfx, arena, caps);
    return be;
}

} // namespace phx
