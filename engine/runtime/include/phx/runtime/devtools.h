// phx/runtime/devtools.h — DEVELOPER TOOLS in a running game (desktop builds only; the engine's
// desktop entry installs them, a console build never links them):
//
//   F1          show / hide the overlay
//   F2          outline every collider (coloured by layer) and the level's collision tiles
//               (grey solid, blue one-way, red hazard)
//   F3          slow motion: full speed -> 1/2 -> 1/4
//   F5          pause / resume the simulation (rendering goes on)
//   F6          advance exactly one fixed step (pauses first)
//   F7 / F8     select the previous / next entity; or click one
//   F9          halt on warnings: pause the moment the engine logs a warning or an error
//   PgUp / PgDn move the inspector's cursor over the selected entity's values;
//   - / =       change the value under it (Shift: x10; a bool toggles) — live, while it runs
//
// The overlay is a live INSPECTOR for the selected entity: its prefab type and spawn, Transform,
// Body, collider and animation, plus every reflected component it has (phx/ecs/reflect.h: the
// stock behaviours and the game's own PHX_COMPONENTs) with its fields' current values. The
// selected entity's collider is outlined in the world. A FRAME-TIME GRAPH (bottom right) plots
// each frame's work (update + render) against the step budget. It is drawn straight into the
// software framebuffer after the game's frame, so it needs no font asset and never touches the
// game's sprites or budgets.
//
// PHX_TRACE=file records every frame's timings (update / render / present / frame µs, steps,
// entities, sprites) as CSV: Phosphorus Studio's Profile launch sets it, and its Budget view's
// Profiler reads build/trace.csv.
#ifndef PHX_RUNTIME_DEVTOOLS_H
#define PHX_RUNTIME_DEVTOOLS_H

#include "phx/runtime/app.h"

namespace phx {

// Install the tools on `app` (before app.run()). Host-only: engine/runtime/src/devtools.cpp.
void install_devtools(App& app);

} // namespace phx
#endif // PHX_RUNTIME_DEVTOOLS_H
