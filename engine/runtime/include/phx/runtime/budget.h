// phx/runtime/budget.h — what a run of the game used, against the target's limits, as JSON: the
// arena (everything persistent: ECS stores, the renderer, the bundle's decompressed assets, the
// game's own allocations), the frame scratch, entities, sprites per frame (and any the target had
// to drop), tiles, sounds, and the warnings / errors the engine logged. `make project-budget`
// runs a project headlessly under each target's profile (PC, GBA, PSP) with scripted play and
// writes build/budget-<target>.json; Phosphorus Studio's Budget view shows them.
//
// Host builds only (stdio); linked by the desktop game build and the headless project runs.
#ifndef PHX_RUNTIME_BUDGET_H
#define PHX_RUNTIME_BUDGET_H

#include "phx/runtime/app.h"
#include "phx/runtime/main.h"

namespace phx {

// Write `app`'s budget report (after App::run: it reads App::peaks(), which outlive the teardown)
// for `target` (its name and ceilings) to `path`.
// `host_only` bytes of the arena exist only off the target (the GBA PPU model's compose buffer,
// which is VRAM on the console): they are left out of both the arena's use and its capacity.
bool write_budget_report(App& app, const TargetProfile& target, const char* path, uint32_t host_only = 0);

} // namespace phx
#endif // PHX_RUNTIME_BUDGET_H
