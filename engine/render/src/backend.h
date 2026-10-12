// engine/render/src/backend.h — INTERNAL render backend interface. The front end
// (renderer.cpp) is backend-agnostic: it records & sorts draws, then calls one backend.
// Exactly one backend translation unit is linked (soft / gl / gu / gba) and defines
// phx_make_render_backend(). Not a public header.
#ifndef PHX_RENDER_BACKEND_INTERNAL_H
#define PHX_RENDER_BACKEND_INTERNAL_H

#include "phx/render/renderer.h"

struct phx_gfx;

namespace phx {

struct IRenderBackend {
    virtual ~IRenderBackend() = default;

    virtual TextureId upload_tex(const TextureDesc&)   = 0;
    virtual bool      free_tex(TextureId)              = 0;   // true only when a live resource was released
    virtual TilemapId upload_map(const TilemapDesc&)   = 0;
    virtual void      set_scroll(TilemapId, vec2 px)   = 0;
    // Mark a retained map's cells as changed since upload. Backends that read the caller's index
    // buffer live (software) need nothing; backends that cache the streamed window (GBA PPU) must
    // re-stream on the next draw. Default no-op so a backend opts in only if it caches. This is
    // what makes a per-frame-rebuilt tilemap (e.g. a spectrogram) show its new contents.
    virtual void      invalidate_map(TilemapId)        {}

    virtual void begin(const Camera2D&)                = 0;   // lock target, clear
    virtual void draw_tilemap(TilemapId, uint8_t layer)= 0;   // background, drawn in call order
    virtual void submit_sprites(const DrawSprite*, uint32_t n) = 0;  // sorted, on top
    virtual void end()                                 = 0;   // present / unlock

    virtual RenderStats& stats()                       = 0;
};

// Defined by the single linked backend (e.g. soft_renderer.cpp). Returns nullptr on failure.
IRenderBackend* phx_make_render_backend(phx_gfx*, ArenaAllocator&, const Caps&);

} // namespace phx
#endif // PHX_RENDER_BACKEND_INTERNAL_H
