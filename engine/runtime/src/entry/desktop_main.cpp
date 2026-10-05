// engine/runtime/src/entry/desktop_main.cpp — main() for a game project on PC (Linux/Windows,
// the SDL platform) and for its headless host runs (the null platform). The bundle is a file
// the game mounts itself. Linked by `make game` when the project's sources have no main().
//
// Desktop extras, none of which a console build has:
//   * the developer tools (phx/runtime/devtools.h): F1 overlay + inspector, F5 pause, F6 step;
//   * PHX_PLAY_FROM="x,y": start the player there instead of at its spawn (the map editor's
//     "Play from here"; phx/runtime/behaviours.h: set_start_override);
//   * PHX_DUMP_COMPONENTS=file: write the game's reflected components as JSON (for Phosphorus
//     Studio; `make game` does it after linking) and exit without booting a window;
//   * an exported game (`make game-export`: the executable next to build/<name>.phxp) runs from its
//     own folder whatever the working directory (phx_desktop_use_exe_dir).
#include "phx/runtime/main.h"
#include "phx/runtime/behaviours.h"
#include "phx/runtime/devtools.h"
#include "phx/platform/desktop.h"

#include <cstdio>
#include <cstdlib>

int main() {
    if (const char* path = std::getenv("PHX_DUMP_COMPONENTS"); path && *path) {
        if (phx::write_component_schema(path)) return 0;
        std::fprintf(stderr, "cannot write the component schema to %s\n", path);
        return 1;
    }
    if (const char* at = std::getenv("PHX_PLAY_FROM"); at && *at) {
        int x = 0, y = 0;
        if (std::sscanf(at, "%d,%d", &x, &y) == 2) phx::set_start_override(x, y);
    }
    phx_desktop_use_exe_dir("build");                         // a shipped game: find its bundle
    phx::Game& game = phx::game_instance();
    phx::App app(phx::game_config(game, phx::kTargetDesktop));
    phx::install_devtools(app);
    return app.run(&game);
}
