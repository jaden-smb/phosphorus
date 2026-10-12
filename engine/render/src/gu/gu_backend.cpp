// engine/render/src/gu/gu_backend.cpp — the PSP-native render backend (render tier 1, GU).
// Records the frame as PSP Graphics Unit sprite quads (textured, nearest-sampled, alpha-
// tested, vertex-colour modulated) and rasterizes them via `gu_compose` (gu_model.h) — the
// exact pixels the GU would scan out. Because the PSP GU is a full-colour textured-triangle
// GPU, this is a high-fidelity backend: its output is bit-identical to the software
// reference (the GBA PPU, by contrast, must quantize to 4bpp/15-bit). On real hardware
// (PHX_TARGET_PSP) the same quads become a sceGuDrawArray(GU_SPRITES,...) display list;
// here they compose on the CPU so the whole path is verifiable headlessly.
//
// Exactly one render backend TU is linked; this one defines phx_make_render_backend()
// (selected at link time instead of soft/gl/gba).
#include "backend.h"
#include "phx/platform/gfx_soft.h"
#include "gu_model.h"

#if defined(PHX_TARGET_PSP)
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>
extern "C" void phx_psp_set_direct(int on);
// One screenful of GU command/vertex list (sceGuGetMemory carves vertices from here).
static unsigned int __attribute__((aligned(16))) g_gu_list[64 * 1024];
// PSP eDRAM framebuffer geometry. Single-buffered at eDRAM offset 0 so an on-PSP readback (the
// sceGu path's verification) can compare the rendered frame against the gu_compose golden model.
namespace { constexpr int kGuBufW = 512, kGuScrW = 480, kGuScrH = 272; }
// GU_SPRITES vertex: texel UVs (like the pspsdk blit sample) + 8888 vertex colour (tint) +
// 16-bit 2D screen coords. Field order must be texture, colour, vertex.
struct GuVtx { unsigned short u, v; unsigned int color; short x, y, z; };
#endif

namespace phx {
namespace {

using namespace phx::gu;

constexpr uint16_t kMaxTextures = 256;
constexpr uint16_t kMaxTilemaps = 32;
constexpr uint32_t kMaxQuads    = 8192;   // tiles + sprites recorded per frame

struct Map {
    const uint16_t* idx;
    int32_t  w, h;
    uint8_t  layers, tw, th;
    TextureId tileset;
    int32_t  scroll_x, scroll_y;
};

class GuBackend final : public IRenderBackend {
public:
    void init(phx_gfx* gfx, ArenaAllocator& a, const Caps&) {
        gfx_   = gfx;
        tex_   = a.alloc_array<GuTexRef>(kMaxTextures);
        free_  = a.alloc_array<TextureId>(kMaxTextures);
        maps_  = a.alloc_array<Map>(kMaxTilemaps);
        quads_ = a.alloc_array<GuQuad>(kMaxQuads);
    }

    // Accepts linear RGBA8 and the tier-1 bake's swizzled RGBA8 (same texels, GU block
    // order — sampled zero-copy on hardware with the swizzle bit set; see gu_model.h).
    TextureId upload_tex(const TextureDesc& d) override {
        const bool swz = d.format == PixelFormat::RGBA8_SWZ;
        if (d.format != PixelFormat::RGBA8 && !swz)          return kNoTexture;
        if (swz && !swz_size_ok(d.width, d.height))          return kNoTexture;
        TextureId id;
        if (free_n_ > 0)                    id = free_[--free_n_];
        else if (tex_count_ < kMaxTextures) id = tex_count_++;
        else                                return kNoTexture;
        tex_[id] = GuTexRef{ static_cast<const uint32_t*>(d.pixels),
                             int32_t(d.width), int32_t(d.height), uint8_t(swz ? 1 : 0) };
        return id;
    }

    bool free_tex(TextureId id) override {
        if (id >= tex_count_ || tex_[id].px == nullptr) return false;
        tex_[id].px = nullptr;
        free_[free_n_++] = id;
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
        cam_x_   = s_to_int(cam.pos.x);
        cam_y_   = s_to_int(cam.pos.y);
        zq_      = s_to_q16(cam.zoom);
        if (zq_ <= 0) zq_ = 1 << 16;
        quad_n_  = 0;
        stats_   = RenderStats{};
    }

    // Camera-relative pixel -> zoomed screen pixel (Q16.16). Identical math to the software
    // backend so the built quads' dest rects match exactly and gu_compose stays bit-identical.
    int zsc(int v) const { return int((int64_t(v) * zq_) >> 16); }

    // A tilemap layer -> one textured quad per non-empty cell (background, painter order).
    void draw_tilemap(TilemapId id, uint8_t layer) override {
        if (id >= map_count_ || layer >= maps_[id].layers) return;
        const Map& m = maps_[id];
        if (m.tileset >= tex_count_ || !tex_[m.tileset].px) return;
        const GuTexRef& ts = tex_[m.tileset];
        const int cols = ts.w / m.tw;
        if (cols <= 0) return;
        const uint16_t* cells = m.idx + size_t(layer) * size_t(m.w) * size_t(m.h);

        for (int ty = 0; ty < m.h; ++ty) {
            for (int tx = 0; tx < m.w; ++tx) {
                uint16_t cell = cells[ty * m.w + tx];
                if (cell == 0) continue;
                if (quad_n_ >= kMaxQuads) return;
                int t = cell - 1;
                GuQuad& q = quads_[quad_n_++];
                q.tex = m.tileset;
                q.sx  = int16_t((t % cols) * m.tw);
                q.sy  = int16_t((t / cols) * m.th);
                q.sw  = m.tw; q.sh = m.th;
                int wx = tx * m.tw - m.scroll_x - cam_x_;
                int wy = ty * m.th - m.scroll_y - cam_y_;
                q.dx  = int16_t(zsc(wx));
                q.dy  = int16_t(zsc(wy));
                q.dw  = int16_t(zsc(wx + m.tw) - zsc(wx));   // zoomed size (edge diff); == tw at 1:1
                q.dh  = int16_t(zsc(wy + m.th) - zsc(wy));
                q.flip = 0;
                q.tint = rgba(255, 255, 255);
                ++stats_.tiles_drawn;
            }
        }
    }

    // Sprites -> textured quads (on top). Full RGBA, flip, dest scaling, and tint all carry
    // straight onto GU sprite vertices — no hardware limitation to honour here.
    void submit_sprites(const DrawSprite* s, uint32_t n) override {
        for (uint32_t i = 0; i < n; ++i) {
            const DrawSprite& d = s[i];
            if (d.tex >= tex_count_ || tex_[d.tex].px == nullptr) continue;
            if (quad_n_ >= kMaxQuads) break;
            GuQuad& q = quads_[quad_n_++];
            q.tex = d.tex;
            q.sx  = d.sx; q.sy = d.sy; q.sw = d.sw; q.sh = d.sh;
            int wx = s_to_int(d.pos.x) - cam_x_;
            int wy = s_to_int(d.pos.y) - cam_y_;
            int ww = d.dw > 0 ? d.dw : d.sw;            // world dest size (UI scaling), pre-zoom
            int wh = d.dh > 0 ? d.dh : d.sh;
            q.dx  = int16_t(zsc(wx));
            q.dy  = int16_t(zsc(wy));
            q.dw  = int16_t(zsc(wx + ww) - zsc(wx));     // == ww at 1:1 (gu_compose treats >0 as dest)
            q.dh  = int16_t(zsc(wy + wh) - zsc(wy));
            q.flip = uint8_t(((d.flags & kFlipX) ? 1 : 0) | ((d.flags & kFlipY) ? 2 : 0));
            q.tint = d.tint;
        }
        ++stats_.batches;
    }

    void end() override {
#if defined(PHX_TARGET_PSP)
        // Real hardware: build the quads into a sceGu display list and let the GU rasterize them
        // into the eDRAM framebuffer. gu_compose (the #else branch) is the exact golden frame.
        submit_gu();
#else
        phx_soft_fb fb = phx_gfx_soft_lock(gfx_);
        if (!fb.pixels) return;
        GuState st{};
        st.tex = tex_; st.tex_count = tex_count_;
        st.quads = quads_; st.quad_count = quad_n_;
        st.clear = rgba(30, 30, 46);
        st.w = fb.w; st.h = fb.h;
        gu_compose(st, fb.pixels);
#endif
    }

    RenderStats& stats() override { return stats_; }

private:
#if defined(PHX_TARGET_PSP)
    // Lazily init the GU once: single-buffered 8888 framebuffer at eDRAM offset 0, nearest
    // sampling, MODULATE-by-vertex-colour (the tint), and an alpha *test* (cutout) — no blend —
    // to match gu_compose exactly (it skips alpha-0 texels, otherwise writes the modulated texel).
    void gu_setup() {
        if (gu_ready_) return;
        sceGuInit();
        sceGuStart(GU_DIRECT, g_gu_list);
        sceGuDrawBuffer(GU_PSM_8888, (void*)0, kGuBufW);
        sceGuDispBuffer(kGuScrW, kGuScrH, (void*)0, kGuBufW);   // single buffer (disp == draw)
        sceGuOffset(2048 - (kGuScrW / 2), 2048 - (kGuScrH / 2));
        sceGuViewport(2048, 2048, kGuScrW, kGuScrH);
        sceGuScissor(0, 0, kGuScrW, kGuScrH);
        sceGuEnable(GU_SCISSOR_TEST);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuDisable(GU_BLEND);
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);   // per-texture swizzle bit re-set at each bind
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0, 0xff);                    // pass texels with alpha > 0
        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuDisplay(GU_TRUE);
        gu_ready_ = true;
    }

    void submit_gu() {
        gu_setup();
        sceGuStart(GU_DIRECT, g_gu_list);
        sceGuClearColor(static_cast<unsigned int>(rgba(30, 30, 46)));   // Rgba is ABGR == GU colour
        sceGuClear(GU_COLOR_BUFFER_BIT);
        for (uint32_t i = 0; i < quad_n_; ++i) {
            const GuQuad& q = quads_[i];
            if (q.tex >= tex_count_ || !tex_[q.tex].px) continue;
            const GuTexRef& t = tex_[q.tex];
            // Tier-1 baked textures are swizzled (GU block order, gu_model.h) — tell the GU,
            // which then reads the blob zero-copy with better texture-cache behaviour.
            sceGuTexMode(GU_PSM_8888, 0, 0, t.swz ? 1 : 0);
            sceGuTexImage(0, t.w, t.h, t.w, t.px);              // texel UVs index this (PoT) texture
            const int dw = q.dw > 0 ? q.dw : q.sw;
            const int dh = q.dh > 0 ? q.dh : q.sh;
            unsigned short u0 = q.sx, u1 = uint16_t(q.sx + q.sw);
            unsigned short v0 = q.sy, v1 = uint16_t(q.sy + q.sh);
            if (q.flip & 1) { unsigned short t2 = u0; u0 = u1; u1 = t2; }
            if (q.flip & 2) { unsigned short t2 = v0; v0 = v1; v1 = t2; }
            const unsigned int col = static_cast<unsigned int>(q.tint);
            GuVtx* v = static_cast<GuVtx*>(sceGuGetMemory(2 * sizeof(GuVtx)));
            v[0] = GuVtx{ u0, v0, col, q.dx, q.dy, 0 };
            v[1] = GuVtx{ u1, v1, col, int16_t(q.dx + dw), int16_t(q.dy + dh), 0 };
            sceGuDrawArray(GU_SPRITES,
                           GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                           2, 0, v);
        }
        sceKernelDcacheWritebackInvalidateAll();               // GU samples the RAM textures
        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();                           // single-buffered: no swap needed
    }
    bool gu_ready_ = false;
#endif // PHX_TARGET_PSP

    phx_gfx*   gfx_ = nullptr;
    GuTexRef*  tex_  = nullptr;  uint16_t tex_count_ = 0;
    TextureId* free_ = nullptr;  uint16_t free_n_   = 0;
    Map*       maps_ = nullptr;  uint16_t map_count_ = 0;
    GuQuad*    quads_ = nullptr; uint32_t quad_n_   = 0;
    int32_t    cam_x_ = 0, cam_y_ = 0;
    int32_t    zq_    = 1 << 16;     // camera zoom as Q16.16 (matches the software backend)
    RenderStats stats_{};
};

} // namespace

IRenderBackend* phx_make_render_backend(phx_gfx* gfx, ArenaAllocator& arena, const Caps& caps) {
    GuBackend* be = arena.make<GuBackend>();
    if (!be) return nullptr;
    be->init(gfx, arena, caps);
    return be;
}

} // namespace phx
