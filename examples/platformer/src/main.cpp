// examples/platformer/main.cpp — the production entry point. One source, four targets: this
// compiles UNCHANGED for Linux/Windows/GBA/PSP (only the linked platform backend differs).
// It bakes the demo bundle (a real project would ship a prebuilt .phxp), then runs the App.
#include "components.h"
#include "bake.h"          // host-only; a real build bakes assets offline instead

using namespace phx;
using namespace game;

int main() {
    const char* bundle = "platformer.phxp";
    if (!bake_platformer_assets(bundle)) {
        std::fprintf(stderr, "platformer: could not write %s\n", bundle);
        return 1;
    }

    Config cfg = Config::from_defaults();        // budgets filled from phx::caps()
    cfg.sim_hz = 60;
    cfg.title  = "Phosphorus Platformer";
    cfg.width  = 128; cfg.height = 96;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 1024;

    App app(cfg);
    PlatformerGame game;
    game.bundle = bundle;
    return app.run(&game);                       // blocks until quit (Start→Quit in the menu)
}
