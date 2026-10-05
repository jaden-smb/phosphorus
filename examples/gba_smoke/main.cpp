// examples/gba_smoke/main.cpp — the first program that runs the Phosphorus engine on REAL Game Boy
// Advance hardware. It boots the GBA platform backend (Mode 3), builds a tileset + sprite in the
// same portable Renderer the host uses, and renders a checkerboard with a d-pad-movable sprite,
// VBlank-synced. Proves the portable C++17 engine (math/fixed-point, memory, render front end,
// software rasterizer) cross-compiles and runs on ARM7TDMI — the portability thesis on metal.
//
// Built by `make gba` (devkitARM); not part of the host `make check`.
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"
#include "phx/render/renderer.h"
#include "phx/input/input.h"
#include "phx/core/caps.h"

#include <stdlib.h>   // malloc — the engine arena lives in EWRAM heap (IWRAM is only 32 KB)

using namespace phx;

namespace {
constexpr int FBW = 120, FBH = 80;        // half-GBA; the backend 2x-scales to 240x160
constexpr int TILES_X = FBW / 8, TILES_Y = FBH / 8;   // 15 x 10
}

int main() {
    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{};
    desc.title = "phx-gba"; desc.width = FBW; desc.height = FBH;
    if (plat->init(&desc) != 0) return 1;

    const size_t kArena = 64 * 1024;
    void* arena_buf = malloc(kArena);            // EWRAM; one boot allocation
    if (!arena_buf) return 1;
    ArenaAllocator arena; arena.init(arena_buf, kArena);
    auto rr = Renderer::create(plat->gfx(), arena, caps());
    if (!rr.ok()) return 1;
    Renderer* r = rr.unwrap();

    // 16x8 tileset: tile0 blue | tile1 yellow; an 8x8 red sprite.
    static uint32_t tileset[16 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x) tileset[y * 16 + x] = (x < 8) ? rgba(40, 80, 200) : rgba(220, 200, 40);
    static uint32_t red[8 * 8]; for (auto& p : red) p = rgba(220, 40, 40);
    TextureDesc tsd{}; tsd.pixels = tileset; tsd.width = 16; tsd.height = 8;
    TextureId ts_tex = r->load_texture(tsd);
    TextureDesc spd{}; spd.pixels = red; spd.width = 8; spd.height = 8;
    TextureId red_tex = r->load_texture(spd);

    // checkerboard tilemap (cell = tile index + 1; alternating 1/2)
    static uint16_t indices[TILES_X * TILES_Y];
    for (int y = 0; y < TILES_Y; ++y)
        for (int x = 0; x < TILES_X; ++x) indices[y * TILES_X + x] = uint16_t(((x + y) & 1) ? 1 : 2);
    TilemapDesc md{}; md.indices = indices; md.width = TILES_X; md.height = TILES_Y;
    md.layers = 1; md.tile_w = 8; md.tile_h = 8; md.tileset = ts_tex;
    TilemapId map = r->upload_tilemap(md);

    InputState in;
    int px = FBW / 2 - 4, py = FBH / 2 - 4;          // sprite top-left
    for (;;) {
        phx_input_raw raw; plat->poll_input(&raw); in.update(raw);
        if (in.down(Button::Right) && px < FBW - 8) ++px;
        if (in.down(Button::Left)  && px > 0)       --px;
        if (in.down(Button::Down)  && py < FBH - 8) ++py;
        if (in.down(Button::Up)    && py > 0)       --py;

        r->begin_frame(Camera2D{});
        r->draw_tilemap(map, 0);
        DrawSprite s{}; s.tex = red_tex; s.sw = 8; s.sh = 8;
        s.pos = vec2{ s_from_int(px), s_from_int(py) }; s.layer = 1;
        r->draw_sprite(s);
        r->end_frame();
        plat->present();
    }
    // unreachable
}
