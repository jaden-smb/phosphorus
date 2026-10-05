// examples/psp_smoke/main.cpp — the Phosphorus engine running on PlayStation Portable hardware.
// Same portable Renderer + software rasterizer as the host and GBA builds; only the platform
// backend differs. Draws a checkerboard with a d-pad-movable sprite, VBlank-synced. Proves the
// one C++17 codebase reaches a third architecture (MIPS Allegrec). Built by `make psp`.
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"
#include "phx/render/renderer.h"
#include "phx/input/input.h"
#include "phx/core/caps.h"

#include <pspkernel.h>
#include <stdlib.h>

PSP_MODULE_INFO("PHX_SMOKE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
// Claim a real heap. Without this the pspsdk default newlib heap is small (~a few hundred KB):
// the 130 KB framebuffer malloc succeeds but the following 256 KB engine-arena malloc then
// returns NULL, the arena lands at address 0, and the Renderer is constructed at 0x0 — a null
// deref on real hardware / PPSSPP. 8 MB is ample (the PSP user partition is ~24 MB).
PSP_HEAP_SIZE_KB(8192);

// Standard HOME-button exit plumbing so the EBOOT behaves on real hardware / PPSSPP.
namespace {
int exit_cb(int, int, void*) { sceKernelExitGame(); return 0; }
int cb_thread(SceSize, void*) {
    int cb = sceKernelCreateCallback("exit", exit_cb, nullptr);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}
void setup_callbacks() {
    int th = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, nullptr);
    if (th >= 0) sceKernelStartThread(th, 0, nullptr);
}
}

using namespace phx;

namespace {
constexpr int FBW = 240, FBH = 136;       // half-PSP; the backend 2x-scales to 480x272
constexpr int TILES_X = FBW / 8, TILES_Y = FBH / 8;
}

int main() {
    setup_callbacks();

    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{}; desc.title = "phx-psp"; desc.width = FBW; desc.height = FBH;
    if (plat->init(&desc) != 0) { sceKernelExitGame(); return 0; }

    const size_t kArena = 256 * 1024;
    void* arena_buf = malloc(kArena);
    if (!arena_buf) { sceKernelExitGame(); return 0; }   // OOM: fail loud, never build an arena at NULL
    ArenaAllocator arena; arena.init(arena_buf, kArena);
    auto rr = Renderer::create(plat->gfx(), arena, caps());
    if (!rr.ok()) { sceKernelExitGame(); return 0; }
    Renderer* r = rr.unwrap();

    static uint32_t tileset[16 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x) tileset[y * 16 + x] = (x < 8) ? rgba(40, 80, 200) : rgba(220, 200, 40);
    static uint32_t red[8 * 8]; for (auto& p : red) p = rgba(220, 40, 40);
    TextureDesc tsd{}; tsd.pixels = tileset; tsd.width = 16; tsd.height = 8;
    TextureId ts_tex = r->load_texture(tsd);
    TextureDesc spd{}; spd.pixels = red; spd.width = 8; spd.height = 8;
    TextureId red_tex = r->load_texture(spd);

    static uint16_t indices[TILES_X * TILES_Y];
    for (int y = 0; y < TILES_Y; ++y)
        for (int x = 0; x < TILES_X; ++x) indices[y * TILES_X + x] = uint16_t(((x + y) & 1) ? 1 : 2);
    TilemapDesc md{}; md.indices = indices; md.width = TILES_X; md.height = TILES_Y;
    md.layers = 1; md.tile_w = 8; md.tile_h = 8; md.tileset = ts_tex;
    TilemapId map = r->upload_tilemap(md);

    InputState in;
    int px = FBW / 2 - 4, py = FBH / 2 - 4;
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
    return 0;
}
