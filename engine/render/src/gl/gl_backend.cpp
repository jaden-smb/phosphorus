// engine/render/src/gl/gl_backend.cpp — the desktop GPU render backend (render tier 2).
// It implements the SAME IRenderBackend seam as the software rasterizer, so the front end,
// the example, and every gameplay system run UNCHANGED on the GPU. The geometry is a direct
// port of soft_renderer's: tilemaps and sprites become textured, tinted quads.
//
// Deliberately GL 1.1 immediate mode (glBegin/glEnd): it is exported straight from libGL with
// NO loader (glad/glew) and NO extra dependency, compiles against the stock <GL/gl.h>, and is
// exactly how 2000s-era 2D games drew — fitting the engine's philosophy and the 2D workload.
// The software backend remains the golden reference the GL output is diffed against (docs/03).
//
// Compiled ONLY when PHX_HAVE_GL is defined, and linked INSTEAD OF soft_renderer.cpp (both
// define phx_make_render_backend). The platform supplies an active GL context + buffer swap;
// the logical framebuffer size is read through the same phx_gfx_soft_lock seam (pixels=null).
#if defined(PHX_HAVE_GL)

#include "backend.h"
#include "phx/platform/gfx_soft.h"

#include <GL/gl.h>

namespace phx {
namespace {

constexpr uint16_t kMaxTextures = 256;
constexpr uint16_t kMaxTilemaps = 32;

struct Tex { GLuint id; int32_t w, h; };
struct Map {
    const uint16_t* idx;
    int32_t  w, h;
    uint8_t  layers, tw, th;
    TextureId tileset;
    int32_t  scroll_x, scroll_y;
};

class GLBackend final : public IRenderBackend {
public:
    void init(phx_gfx* gfx, ArenaAllocator& a, const Caps&) {
        gfx_  = gfx;
        tex_  = a.alloc_array<Tex>(kMaxTextures);
        free_ = a.alloc_array<TextureId>(kMaxTextures);
        maps_ = a.alloc_array<Map>(kMaxTilemaps);

        // logical size comes through the soft-fb seam (pixels=null on the GL platform)
        phx_soft_fb fb = phx_gfx_soft_lock(gfx_);
        log_w_ = fb.w; log_h_ = fb.h;

        // drawable (window) size = the default viewport set when the context was created
        GLint vp[4] = { 0, 0, log_w_, log_h_ };
        glGetIntegerv(GL_VIEWPORT, vp);
        draw_w_ = vp[2] > 0 ? vp[2] : log_w_;
        draw_h_ = vp[3] > 0 ? vp[3] : log_h_;

        glEnable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }

    TextureId upload_tex(const TextureDesc& d) override {
        if (d.format != PixelFormat::RGBA8) return kNoTexture;
        TextureId tid;
        if (free_n_ > 0)              tid = free_[--free_n_];   // reuse a recycled slot
        else if (tex_count_ < kMaxTextures) tid = tex_count_++;
        else return kNoTexture;
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // phx Rgba bytes are R,G,B,A -> GL_RGBA / GL_UNSIGNED_BYTE, no conversion.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, d.width, d.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, d.pixels);
        tex_[tid] = Tex{ id, int32_t(d.width), int32_t(d.height) };
        return tid;
    }

    bool free_tex(TextureId tid) override {
        if (tid >= tex_count_ || tex_[tid].id == 0) return false;    // out of range / double free
        glDeleteTextures(1, &tex_[tid].id);
        tex_[tid].id = 0;                                      // mark slot empty
        free_[free_n_++] = tid;                                // recycle the id
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
        cam_x_ = s_to_int(cam.pos.x);
        cam_y_ = s_to_int(cam.pos.y);
        zq_    = s_to_q16(cam.zoom);
        if (zq_ <= 0) zq_ = 1 << 16;
        stats_ = RenderStats{};

        glViewport(0, 0, draw_w_, draw_h_);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, double(log_w_), double(log_h_), 0.0, -1.0, 1.0);  // top-left origin, y-down
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glClearColor(30.0f / 255, 30.0f / 255, 46.0f / 255, 1.0f);     // match soft backend
        glClear(GL_COLOR_BUFFER_BIT);
    }

    void draw_tilemap(TilemapId id, uint8_t layer) override {
        if (id >= map_count_ || layer >= maps_[id].layers) return;
        const Map& m = maps_[id];
        if (m.tileset >= tex_count_) return;
        const Tex& ts = tex_[m.tileset];
        if (ts.id == 0) return;                        // tileset texture was freed
        const int cols = ts.w / m.tw;
        if (cols <= 0) return;
        const uint16_t* li = m.idx + size_t(layer) * size_t(m.w) * size_t(m.h);
        for (int ty = 0; ty < m.h; ++ty)
            for (int tx = 0; tx < m.w; ++tx) {
                uint16_t cell = li[ty * m.w + tx];
                if (cell == 0) continue;
                int t = cell - 1;
                int sx = (t % cols) * m.tw, sy = (t / cols) * m.th;
                int wx = tx * m.tw - m.scroll_x - cam_x_;
                int wy = ty * m.th - m.scroll_y - cam_y_;
                int dx = zsc(wx), dy = zsc(wy);
                quad(ts, sx, sy, m.tw, m.th, dx, dy, zsc(wx + m.tw) - dx, zsc(wy + m.th) - dy,
                     false, false, rgba(255,255,255));
                ++stats_.tiles_drawn;
            }
    }

    void submit_sprites(const DrawSprite* s, uint32_t n) override {
        for (uint32_t i = 0; i < n; ++i) {
            const DrawSprite& d = s[i];
            if (d.tex >= tex_count_ || tex_[d.tex].id == 0) continue;
            int wx = s_to_int(d.pos.x) - cam_x_;
            int wy = s_to_int(d.pos.y) - cam_y_;
            int ww = d.dw > 0 ? d.dw : d.sw, wh = d.dh > 0 ? d.dh : d.sh;
            int dx = zsc(wx), dy = zsc(wy);
            quad(tex_[d.tex], d.sx, d.sy, d.sw, d.sh, dx, dy,
                 zsc(wx + ww) - dx, zsc(wy + wh) - dy,
                 (d.flags & kFlipX) != 0, (d.flags & kFlipY) != 0, d.tint);
        }
        ++stats_.batches;
    }

    void end() override { glFlush(); }   // buffer swap is the platform's present()

    RenderStats& stats() override { return stats_; }

private:
    // Camera-relative pixel -> zoomed screen pixel (Q16.16), same math as soft/GU so the GPU
    // fills the identical integer dest rects (the software backend stays the golden reference).
    int zsc(int v) const { return int((int64_t(v) * zq_) >> 16); }

    void quad(const Tex& t, int sx, int sy, int sw, int sh,
              int dx, int dy, int dw, int dh, bool fx, bool fy, Rgba tint) {
        if (t.w <= 0 || t.h <= 0) return;
        float u0 = float(sx) / t.w, u1 = float(sx + sw) / t.w;
        float v0 = float(sy) / t.h, v1 = float(sy + sh) / t.h;
        if (fx) { float tmp = u0; u0 = u1; u1 = tmp; }
        if (fy) { float tmp = v0; v0 = v1; v1 = tmp; }
        const float x0 = float(dx), x1 = float(dx + dw);
        const float y0 = float(dy), y1 = float(dy + dh);

        glBindTexture(GL_TEXTURE_2D, t.id);
        glColor4ub(rgba_r(tint), rgba_g(tint), rgba_b(tint), rgba_a(tint));
        glBegin(GL_QUADS);
            glTexCoord2f(u0, v0); glVertex2f(x0, y0);
            glTexCoord2f(u1, v0); glVertex2f(x1, y0);
            glTexCoord2f(u1, v1); glVertex2f(x1, y1);
            glTexCoord2f(u0, v1); glVertex2f(x0, y1);
        glEnd();
    }

    phx_gfx* gfx_ = nullptr;
    int32_t  log_w_ = 0, log_h_ = 0, draw_w_ = 0, draw_h_ = 0;
    int32_t  cam_x_ = 0, cam_y_ = 0;
    int32_t  zq_    = 1 << 16;     // camera zoom as Q16.16 (matches soft/GU dest rects)
    Tex*       tex_  = nullptr;  uint16_t tex_count_ = 0;
    TextureId* free_ = nullptr;  uint16_t free_n_   = 0;    // recycled-slot stack
    Map*     maps_ = nullptr;  uint16_t map_count_ = 0;
    RenderStats stats_ {};
};

} // namespace

IRenderBackend* phx_make_render_backend(phx_gfx* gfx, ArenaAllocator& arena, const Caps& caps) {
    GLBackend* be = arena.make<GLBackend>();
    if (!be) return nullptr;
    be->init(gfx, arena, caps);
    return be;
}

} // namespace phx

#endif // PHX_HAVE_GL
