// examples/platformer/src/gba_ppu_main.cpp — GBA entry for the platformer rendered by the
// NATIVE PPU backend (Mode-0 tiles + OBJ on real silicon), as opposed to gba_main.cpp which
// drives the software rasterizer into a Mode-3 bitmap. Same game, same systems.cpp, same baked
// bundle; the only differences:
//   1. It links the PPU backend (gba_ppu.cpp) instead of the soft rasterizer (link-time choice;
//      see `make gba-platformer-ppu`), so the engine programs VRAM/OAM/PALRAM directly.
//   2. It renders at the GBA's NATIVE 240x160 (the PPU has no 2x upscale step), so the camera
//      viewport — `config().width/height` — is 240x160, not the soft path's 120x80.
//   3. It calls phx_gba_set_direct(1) BEFORE boot so the platform skips its Mode-3 blit AND
//      allocates only a 1x1 stub software framebuffer (the PPU path never uses it) — keeping the
//      EWRAM budget intact at native resolution.
// Built by `make gba-platformer-ppu` (devkitARM); not part of the host `make check`.
#include "components.h"

using namespace phx;
using namespace game;

extern "C" const unsigned char platformer_phxp[];
extern "C" const unsigned int  platformer_phxp_size;
extern "C" void phx_gba_set_bundle(const void* data, unsigned long size);
extern "C" void phx_gba_set_direct(int on);

int main() {
    phx_gba_set_bundle(platformer_phxp, platformer_phxp_size);
    phx_gba_set_direct(1);                          // PPU owns the display; stub the soft fb

    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60;
    cfg.title  = "Phosphorus Platformer (PPU)";
    cfg.width  = 240; cfg.height = 160;            // native PPU resolution (no 2x upscale)
    cfg.total_ram     = 160u << 10;
    cfg.frame_scratch = 4u  << 10;
    cfg.max_entities  = 256;

    App app(cfg);
    PlatformerGame game;
    game.scene_scratch_bytes = 16u << 10;
    game.bundle = "platformer.phxp";
    return app.run(&game);
}
