// examples/platformer/src/psp_main.cpp — the PSP entry point for the EXAMPLE PLATFORMER.
// Same game, same engine, same systems.cpp as the host/GBA builds; only the platform backend
// and the boot boilerplate differ from examples/platformer/src/main.cpp. Like the GBA entry
// (gba_main.cpp), it avoids the filesystem: the .phxp bundle is baked offline on the host and
// linked into the EBOOT (see `make psp-platformer`), handed to the PSP platform seam via
// phx_psp_set_bundle() before boot so the game's res->mount() finds it in memory.
// Built by `make psp-platformer` (pspsdk); not part of the host `make check`.
#include "components.h"      // PlatformerGame + the engine (portable; no host/STL deps)

#include <pspkernel.h>
#include <stdlib.h>

PSP_MODULE_INFO("PhosphorusPlatformer", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
// Claim a real heap: the App's root arena (cfg.total_ram) is malloc'd from it. The PSP user
// partition is ~24 MB; 16 MB is ample for the game's 4 MB arena + the framebuffer + libc.
PSP_HEAP_SIZE_KB(16384);

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
} // namespace

using namespace phx;
using namespace game;

// The .phxp bundle linked into the EBOOT by bin2s (read-only). Same symbols the GBA ROM uses.
extern "C" const unsigned char platformer_phxp[];
extern "C" const unsigned int  platformer_phxp_size;

// Game-side hook exported by the PSP platform backend (not part of the C seam).
extern "C" void phx_psp_set_bundle(const void* data, unsigned long size);

int main() {
    setup_callbacks();

    // Register the EBOOT-embedded asset bundle; the platform's file seam serves it on mount().
    phx_psp_set_bundle(platformer_phxp, platformer_phxp_size);

    Config cfg = Config::from_defaults();          // budgets seeded from phx::caps() (PC tier)
    cfg.sim_hz = 60;
    cfg.title  = "Phosphorus Platformer";
    cfg.width  = 240; cfg.height = 136;            // half-PSP; the backend 2x-scales to 480x272
    cfg.total_ram     = 4u << 20;                  // engine arena (framebuffer is allocated apart)
    cfg.frame_scratch = 64u << 10;
    cfg.max_entities  = 1024;

    App app(cfg);
    PlatformerGame game;
    game.scene_scratch_bytes = 64u << 10;
    game.bundle = "platformer.phxp";               // path ignored on PSP (single embedded bundle)
    return app.run(&game);                          // runs until HOME → Exit
}
