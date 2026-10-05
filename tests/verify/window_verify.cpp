// tests/window_verify.cpp — pixel-verifies a REAL window/GPU render backend against the
// software golden reference. It boots the actual SDL platform (and, with PHX_HAVE_GL, the
// OpenGL render backend), draws the exact render_test scene through the unified Renderer,
// reads the *presented* frame back at logical resolution (phx_sdl_readback), and asserts the
// same pixels render_test asserts on the software path. This is the on-hardware analogue of
// `make ppu`/`make gu`: the soft backend stays the oracle; here we confirm the windowed
// desktop backends actually scan out the same image on a real display + GPU.
//
// Built by `make sdl-verify` (software-present path) and `make gl-verify` (OpenGL path).
// Needs SDL2 (+ libGL for the GL path) and a display ($DISPLAY).
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"
#include "phx/render/renderer.h"
#include "phx/core/caps.h"

#include <cstdio>
#include <cstdlib>

using namespace phx;

// Implemented in the SDL backend (the only TU allowed to touch SDL/GL): read the presented
// frame back as logical-resolution phx Rgba. Call after end_frame(), before present().
extern "C" int phx_sdl_readback(uint32_t* out, int lw, int lh);

namespace {
constexpr int FBW = 64, FBH = 48;

const Rgba kClear  = rgba(30, 30, 46);
const Rgba kBlue   = rgba(40, 80, 200);
const Rgba kYellow = rgba(220, 200, 40);
const Rgba kRed    = rgba(220, 40, 40);

int g_checks = 0, g_fail = 0;

inline int ch(Rgba v, int i) { return int((v >> (i * 8)) & 0xFF); }

// Allow a tiny per-channel tolerance: the GPU path may round ±1 in 8-bit blend/sampling.
void expect_px(const uint32_t* fb, int x, int y, Rgba want, const char* what) {
    ++g_checks;
    Rgba got = fb[y * FBW + x];
    int d = 0;
    for (int i = 0; i < 3; ++i) { int e = ch(got, i) - ch(want, i); if (e < 0) e = -e; if (e > d) d = e; }
    if (d > 4) {
        ++g_fail;
        std::printf("    FAIL pixel(%d,%d) %s: got %08X want %08X (dmax=%d)\n", x, y, what, got, want, d);
    }
}
} // namespace

int main() {
    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{};
    desc.title = "phosphorus window_verify"; desc.width = FBW; desc.height = FBH; desc.vsync = 0;
    if (plat->init(&desc) != 0) { std::printf("platform init failed (no display?)\n"); return 1; }

    static uint8_t arena_buf[8 << 20];
    ArenaAllocator arena; arena.init(arena_buf, sizeof(arena_buf));
    auto rr = Renderer::create(plat->gfx(), arena, caps());
    if (!rr.ok()) { std::printf("Renderer::create failed\n"); plat->shutdown(); return 1; }
    Renderer* r = rr.unwrap();

    // Same scene as render_test: a blue|yellow tileset, a red sprite, a 4x3 checker tilemap.
    static uint32_t tileset[16 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x) tileset[y * 16 + x] = (x < 8) ? kBlue : kYellow;
    static uint32_t red[8 * 8];
    for (int i = 0; i < 64; ++i) red[i] = kRed;

    TextureDesc tsd{}; tsd.pixels = tileset; tsd.width = 16; tsd.height = 8;
    TextureId ts_tex = r->load_texture(tsd);
    TextureDesc spd{}; spd.pixels = red; spd.width = 8; spd.height = 8;
    TextureId red_tex = r->load_texture(spd);

    static const uint16_t indices[4 * 3] = { 1, 2, 1, 2,  2, 1, 2, 1,  1, 2, 1, 2 };
    TilemapDesc md{}; md.indices = indices; md.width = 4; md.height = 3; md.layers = 1;
    md.tile_w = 8; md.tile_h = 8; md.tileset = ts_tex;
    TilemapId map = r->upload_tilemap(md);

    Camera2D cam{};
    r->begin_frame(cam);
    r->draw_tilemap(map, 0);
    DrawSprite spr{}; spr.tex = red_tex; spr.sx = 0; spr.sy = 0; spr.sw = 8; spr.sh = 8;
    spr.pos = vec2{ s_from_int(20), s_from_int(20) }; spr.layer = 1; spr.z = 0;
    r->draw_sprite(spr);
    r->end_frame();

    // Read the real presented frame back and assert the golden pixels.
    static uint32_t shot[FBW * FBH];
    if (phx_sdl_readback(shot, FBW, FBH) != 0) {
        std::printf("readback failed\n"); plat->shutdown(); return 1;
    }
    plat->present();   // also show it on screen for the record

    expect_px(shot, 3,  3,  kBlue,   "tile(0,0) blue");
    expect_px(shot, 11, 3,  kYellow, "tile(1,0) yellow");
    expect_px(shot, 3,  11, kYellow, "tile(0,1) yellow");
    expect_px(shot, 23, 23, kRed,    "sprite over tile");
    expect_px(shot, 50, 40, kClear,  "background");

    // Camera zoom (2x) through the real backend: same scaled scene the soft/GU tests assert.
    r->begin_frame(Camera2D{ vec2{}, s_from_int(2), 0 });
    r->draw_tilemap(map, 0);
    r->draw_sprite(spr);
    r->end_frame();
    if (phx_sdl_readback(shot, FBW, FBH) != 0) { std::printf("readback failed\n"); plat->shutdown(); return 1; }
    plat->present();
    expect_px(shot, 3,  3,  kBlue,   "zoom2 tile(0,0) blue grown");
    expect_px(shot, 20, 3,  kYellow, "zoom2 tile(1,0) yellow at 2x offset");
    expect_px(shot, 44, 44, kRed,    "zoom2 sprite scaled to 16px");
    expect_px(shot, 23, 23, kBlue,   "zoom2: old sprite spot now background tile");

    plat->shutdown();

#if defined(PHX_HAVE_GL)
    const char* tag = "GL";
#else
    const char* tag = "SDL";
#endif
    std::printf("\nwindow_verify[%s]: %d checks, %d failures\n", tag, g_checks, g_fail);
    std::printf(g_fail == 0 ? "WINDOW PASS (%s backend pixel-matches the software golden)\n\n"
                            : "WINDOW FAIL\n\n", tag);
    return g_fail == 0 ? 0 : 1;
}
