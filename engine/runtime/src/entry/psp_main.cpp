// engine/runtime/src/entry/psp_main.cpp — main() for a game project on PSP (software renderer;
// see examples/platformer/src/psp_main.cpp). Linked by `make game-psp`, which bakes the project's
// tier-1 bundle and links it into the EBOOT as `phx_game_phxp` (tools/common/bin2s.py --name).
// The game's ResourceCache::mount() gets this bundle whatever path it names.
#include "phx/runtime/main.h"

#include <pspkernel.h>

PSP_MODULE_INFO("PhosphorusGame", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
// The App's root arena (kTargetPsp: 4 MB) is malloc'd from this heap, next to the framebuffer
// and libc; the PSP user partition is ~24 MB.
PSP_HEAP_SIZE_KB(16384);

extern "C" const unsigned char phx_game_phxp[];
extern "C" const unsigned int  phx_game_phxp_size;

// Hook exported by the PSP platform backend (not part of the C seam).
extern "C" void phx_psp_set_bundle(const void* data, unsigned long size);

namespace {
// HOME > Exit, so the EBOOT behaves on hardware and in PPSSPP.
int exit_cb(int, int, void*) { sceKernelExitGame(); return 0; }
int cb_thread(SceSize, void*) {
    const int cb = sceKernelCreateCallback("exit", exit_cb, nullptr);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}
} // namespace

int main() {
    const int th = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, nullptr);
    if (th >= 0) sceKernelStartThread(th, 0, nullptr);
    phx_psp_set_bundle(phx_game_phxp, phx_game_phxp_size);
    return phx::run_game(phx::game_instance(), phx::kTargetPsp);
}
