// examples/platformer/src/gba_main.cpp — the GBA entry point for the EXAMPLE PLATFORMER.
// Same game, same engine, same systems.cpp as the host build; only two things differ from
// examples/platformer/src/main.cpp:
//   1. There is no filesystem on a GBA, so the .phxp bundle is baked offline (host) and
//      linked into the ROM (see `make gba-platformer`); we hand it to the platform seam via
//      phx_gba_set_bundle() before boot, and the game's res->mount() then finds it.
//   2. Budgets are sized for the GBA's 256 KB EWRAM (the host build uses 32 MB): a 120x80
//      framebuffer (the backend 2x-scales to 240x160), a ~160 KB engine arena, 256 entities,
//      and a small scene-scratch — instead of the host's PC-sized numbers.
// Built by `make gba-platformer` (devkitARM); not part of the host `make check`.
#include "components.h"      // PlatformerGame + the engine (portable; no host/STL deps)

using namespace phx;
using namespace game;

// The .phxp bundle linked into the ROM by bin2s (read-only, lives in cartridge ROM).
extern "C" const unsigned char platformer_phxp[];
extern "C" const unsigned int  platformer_phxp_size;

// Game-side hook exported by the GBA platform backend (not part of the C seam).
extern "C" void phx_gba_set_bundle(const void* data, unsigned long size);

int main() {
    // Register the ROM-embedded asset bundle; the platform's file seam serves it on mount().
    phx_gba_set_bundle(platformer_phxp, platformer_phxp_size);

    Config cfg = Config::from_defaults();          // budgets seeded from phx::caps() (GBA tier)
    cfg.sim_hz = 60;
    cfg.title  = "Phosphorus Platformer";
    cfg.width  = 120; cfg.height = 80;             // 2x-scaled to 240x160 by the GBA backend
    cfg.total_ram     = 160u << 10;                // EWRAM engine arena (fb is allocated apart)
    cfg.frame_scratch = 4u  << 10;
    cfg.max_entities  = 256;

    App app(cfg);
    PlatformerGame game;
    game.scene_scratch_bytes = 16u << 10;          // scenes are tiny; the host's 256 KB won't fit
    game.bundle = "platformer.phxp";               // path is ignored on GBA (single ROM bundle)
    return app.run(&game);                          // runs until power-off
}
