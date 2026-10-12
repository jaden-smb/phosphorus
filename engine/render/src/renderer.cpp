// engine/render/src/renderer.cpp — the backend-agnostic render front end. Records sprite
// draws into a fixed per-frame list (sized to caps.max_sprites — the honest ceiling),
// sorts by (layer, z, tex), and dispatches to the one linked backend. Tilemaps are drawn
// immediately as background; sprites are drawn on top after sorting.
#include "phx/render/renderer.h"
#include "phx/core/hot.h"
#include "backend.h"

namespace phx {

namespace {
// Sort key: layer (high) -> z -> texture, so batches of the same texture stay adjacent.
inline uint64_t sort_key(const DrawSprite& s) {
    return (uint64_t(s.layer) << 40) | (uint64_t(s.z) << 32) | uint64_t(s.tex);
}
} // namespace

Result<Renderer*> Renderer::create(phx_gfx* gfx, ArenaAllocator& arena, const Caps& caps) {
    Renderer* r = arena.make<Renderer>();
    if (!r) return Result<Renderer*>::fail(Status::OutOfMemory);

    r->arena_ = &arena;
    r->be_    = phx_make_render_backend(gfx, arena, caps);
    if (!r->be_) return Result<Renderer*>::fail(Status::Unsupported);

    r->cap_     = caps.max_sprites ? caps.max_sprites : 256;
    r->sprites_ = arena.alloc_array<DrawSprite>(r->cap_);
    if (!r->sprites_) return Result<Renderer*>::fail(Status::OutOfMemory);

    return Result<Renderer*>::good(r);
}

void Renderer::begin_frame(const Camera2D& cam) {
    cam_   = cam;
    // Screen shake: a deterministic per-frame camera jitter of magnitude cam.shake (pixels),
    // applied here in the front end so every backend (which already honours camera pos) shakes
    // identically. shake==0 is an exact no-op; the offset cycles a fixed pattern keyed by an
    // internal frame counter, so runs are reproducible (no RNG, no nondeterminism across tiers).
    if (cam.shake > 0) {
        static const int8_t ox[8] = { 1, -1,  0,  1, -1,  1,  0, -1 };
        static const int8_t oy[8] = { 0,  1, -1, -1,  1,  0,  1, -1 };
        const int s = cam.shake, k = int(frame_ & 7);
        cam_.pos.x += s_from_int(ox[k] * s);
        cam_.pos.y += s_from_int(oy[k] * s);
    }
    ++frame_;
    count_ = 0;
    stats_ = RenderStats{};
    be_->stats() = RenderStats{};
    be_->begin(cam_);                 // the (possibly shaken) camera the backends actually use
}

TextureId Renderer::load_texture(const TextureDesc& d) {
    TextureId id = be_->upload_tex(d);
    if (id != kNoTexture) ++live_tex_;
    return id;
}
void Renderer::unload_texture(TextureId id) {
    if (id == kNoTexture) return;
    if (be_->free_tex(id) && live_tex_) --live_tex_;
}
TilemapId Renderer::upload_tilemap(const TilemapDesc& d){ return be_->upload_map(d); }
void Renderer::refresh_tilemap(TilemapId id){ be_->invalidate_map(id); }

void Renderer::set_tilemap_scroll(TilemapId id, vec2 px) {
    if (id < kMaxTilemaps) scroll_[id].base = px;   // remembered so parallax composes with it
    be_->set_scroll(id, px);
}

void Renderer::set_tilemap_parallax(TilemapId id, uint8_t layer, scalar fx, scalar fy) {
    if (id >= kMaxTilemaps || layer >= kParallaxLayers) return;
    scroll_[id].fx_q16[layer] = s_to_q16(fx);
    scroll_[id].fy_q16[layer] = s_to_q16(fy);
}

void Renderer::draw_tilemap(TilemapId id, uint8_t layer) {
    // Parallax: a layer with factor f draws as if scrolled by base + (f−1)·camera, so the
    // backend's usual (tile − scroll − cam) lands on (tile − base − f·cam). f==1 restores
    // the plain base scroll (the backend's per-map scroll is re-sent before each layer draw,
    // so differently-factored layers of one map compose correctly). Q16 integer math
    // end-to-end: both scalar tiers compute the identical whole-pixel offset, and the
    // camera here is the post-shake one, so backgrounds shake at their own depth too.
    if (id < kMaxTilemaps) {
        const MapScroll& m = scroll_[id];
        const uint8_t   l = layer < kParallaxLayers ? layer : uint8_t(kParallaxLayers - 1);
        const int32_t  fx = layer < kParallaxLayers ? m.fx_q16[l] : kQ16One;
        const int32_t  fy = layer < kParallaxLayers ? m.fy_q16[l] : kQ16One;
        if (fx != kQ16One || fy != kQ16One) {
            const int64_t cx = s_to_q16(cam_.pos.x), cy = s_to_q16(cam_.pos.y);
            const int32_t px = int32_t((s_to_q16(m.base.x) + (fx - kQ16One) * cx / kQ16One) >> 16);
            const int32_t py = int32_t((s_to_q16(m.base.y) + (fy - kQ16One) * cy / kQ16One) >> 16);
            be_->set_scroll(id, vec2{ s_from_int(px), s_from_int(py) });
        } else {
            be_->set_scroll(id, m.base);   // restore: an earlier parallax layer may have shifted it
        }
    }
    be_->draw_tilemap(id, layer);
}

void Renderer::draw_sprite(const DrawSprite& s) {
    if (count_ >= cap_) { ++stats_.sprites_dropped; return; }   // honest ceiling, graceful
    sprites_[count_++] = s;
}

PHX_HOT_CODE void Renderer::end_frame() {
    // STABLE insertion sort by (layer, z, tex). Replaces qsort for two reasons: games
    // submit sprites nearly layer-ordered already, so this is ~O(n) with no per-compare
    // callback (qsort's indirect compare + byte-wise swaps were measurable on GBA); and
    // qsort is unstable with a libc-specific tie order — equal-key sprites must draw in
    // submission order on every platform or golden frames diverge across toolchains.
    for (uint32_t i = 1; i < count_; ++i) {
        const DrawSprite s   = sprites_[i];
        const uint64_t   key = sort_key(s);
        uint32_t j = i;
        for (; j > 0 && sort_key(sprites_[j - 1]) > key; --j) sprites_[j] = sprites_[j - 1];
        sprites_[j] = s;
    }

    be_->submit_sprites(sprites_, count_);
    be_->end();

    // merge front-end + backend stats for the public view
    RenderStats bs = be_->stats();
    bs.sprites_submitted = count_;
    bs.sprites_dropped  += stats_.sprites_dropped;
    stats_ = bs;
}

} // namespace phx
