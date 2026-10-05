# Phosphorus Engine — Folder Structure (annotated)

Every engine module follows the identical shape: a public `include/phx/<mod>/` (the
only thing siblings may include) and a private `src/` with optional per-backend
subfolders. The build links exactly one backend per module.

```
phosphorus/
├── README.md                     Engine identity + navigation
├── STRUCTURE.md                  (this file)
├── LICENSE                       MIT
├── Makefile                      Host build + every suite/gate/cross target (day-to-day driver)
├── CMakeLists.txt                Root build: target select, module registration (canonical multi-target)
├── Doxyfile                      API reference config (`make docs` -> build/docs/html; public headers only)
│
├── cmake/                        Build machinery (a new platform = a file here + a backend folder)
│   ├── phx_module.cmake          phx_add_module(): one-backend-per-module helper
│   ├── caps_select.cmake         PHX_TARGET -> compile defines + render tier
│   ├── gba.toolchain.cmake       devkitARM cross toolchain (+ phx_gba_rom; sets PHX_GBA_HW)
│   ├── psp.toolchain.cmake       pspsdk cross toolchain  (+ EBOOT.PBP packaging)
│   └── mingw.toolchain.cmake     MinGW-w64 cross toolchain (Windows PE32+ from Linux/CI)
│
├── docs/                         The technical design documentation (start at 00)
│   ├── 00-architecture.md        Master architecture, seams, risk register
│   ├── 01-core.md … 10-gameplay-systems.md
│   └── diagrams/                 Standalone UML / module / sequence diagrams
│
├── engine/                       The engine. Acyclic dependency graph (enforced).
│   ├── core/                     Closed foundation (zero outgoing module edges)
│   │   ├── include/phx/core/     types.h assert.h fixed.h math.h caps.h pixel.h log.h config.h time.h profile.h
│   │   └── src/                  assert.cpp fixed.cpp log.cpp
│   ├── memory/                   Arena / Stack / Pool / Object allocators + MemoryRoot
│   │   └── include/phx/memory/   allocators.h memory_root.h
│   ├── platform/                 The C-ABI seam (the ONLY place OS/SDK headers appear)
│   │   ├── include/phx/platform/ platform.h (C seam: gfx/input+pointer/audio/file/save/clock/log)
│   │   └── src/{null,sdl,gba,psp}/ one linked per build — null=headless (scripted clock/input/pointer),
│   │                                sdl=window+audio+mouse, gba=ROM+DirectSound+SRAM save,
│   │                                psp=EBOOT+sceAudio+sceIo save (ms0:-anchored keys)
│   ├── runtime/                  Composition root: the App fixed-step main loop (top layer)
│   │   ├── include/phx/runtime/  app.h (App, Game hooks, FrameProfile accessor) · main.h (PHX_GAME,
│   │   │                         target profiles, run_game: the engine-owned entry for game projects)
│   │   │                         · audio.h (GameAudio = App::audio(): mixer + queue + the seam's
│   │   │                         device, started on first play; headless mix where there's none)
│   │   │                         · dialogue.h (DialogueRunner: .dlg conversations — lines, choices,
│   │   │                         if/do on variables — in a typewriter box; Talk NPCs, talk screens)
│   │   │                         · flow.h (GameFlow: the flow table's screens — title, levels,
│   │   │                         lives, pause, HUD, end — run from data)
│   │   │                         · font.h (load_font: a baked Font asset -> a ui BitmapFont,
│   │   │                         proportional glyph table read in place)
│   │   │                         · devtools.h (desktop: F1 overlay, F5 pause, F6 step, live
│   │   │                         entity inspector over the reflected components)
│   │   │                         · behaviours.h (stock components: PlatformerController, Patrol,
│   │   │                         Pickup, Hazard, Checkpoint, Exit, CameraFollow + the Behaviours system)
│   │   │                         · level.h (Level: tilemap + collision grid + spawns -> entities
│   │   │                         from the prefab table; SpriteRenderer, PrefabRef, draw_sprites)
│   │   └── src/                  app.cpp (boot -> loop w/ per-phase profiling -> teardown) ·
│   │                             game_main.cpp (profile -> Config) · component_schema.cpp (host:
│   │                             reflected components -> JSON for the Studio) · budget.cpp (host: a run's
│   │                             App::peaks() vs the target's limits -> JSON, `make project-budget`) · level.cpp (linked by projects
│   │                             + the level suite) · entry/{desktop,gba,psp}_main.cpp
│   │                             (each target's main(), linked only by `make game|game-gba|game-psp`)
│   ├── render/                   One 2D-intent API; backends per render tier; LRU texture cache
│   │   ├── include/phx/render/   renderer.h texture_cache.h
│   │   └── src/{soft,gl,gu,gba}/  software (the golden reference) · GL (tier 2) ·
│   │                              PSP GU (tier 1: gu_backend.cpp + gu_model.h compositor) ·
│   │                              GBA PPU (tier 0: gba_ppu.cpp + ppu_model.h; PHX_GBA_HW = real MMIO)
│   │                              front end (renderer.cpp): sort/batch + camera zoom/shake +
│   │                              per-layer parallax — Q16 tier-exact, inherited by every backend
│   ├── ecs/                      Sparse-set World, components, systems; reflect.h (PHX_COMPONENT /
│   │                             PHX_FIELD: a game's components by name, for the level loader + tools)
│   ├── input/                    phx_input_raw -> semantic Button/edge/axis/pointer state,
│   │                             remappable via InputMap (+ integer stick->dpad synthesis)
│   ├── audio/                    Software mixer (mixer.h) + SPSC ring streaming (stream.h) + lock-free command queue (command_queue.h)
│   ├── resource/                 .phxp bundle mount, zero-copy in-place views (incl. per-layer parallax), LZSS codec;
│   │                             table.h (TableView: a phxbin table's columns by name, via its schema trailer)
│   ├── scene/                    Scene stack, transitions, persistent blackboard
│   ├── physics/                  AABB + swept tile collision (per-tile flags: solid/one-way/
│   │                             hazard, or the solid_from fallback); overlap queries
│   ├── anim/                     Sprite-sheet frames + animation state machine
│   └── ui/                       Immediate-mode menus/text/HUD + typewriter dialogue + profiler overlay
│
├── tools/                        Host-only (C++17, STL allowed; never ships to console).
│   │                             Every tool folder has an instructions.md (usage/formats/controls).
│   ├── phxpack/                  Bundle ASSEMBLER: bakes sources directly OR merges .phx* intermediates -> assets.phxp.
│   │                             builders.h = the ONE shared bake path (DEFLATE/PNG/JSON/Tiled/WAV importers);
│   │                             synth.h = .sfx (sound-effect generator) / .song (tracker) -> PCM;
│   │                             font.h = .font grid sheets / BMFont .fnt -> a Font glyph table;
│   │                             dialogue.h = .dlg conversations: validate, compile, simulate;
│   │                             bundle_writer.h does the per-target encode; bundle_reader.h merges.
│   ├── phxsprite/                CONVERTER: PNG + .sprdef/json sidecar -> .phxspr (atlas + animation clips);
│   │                             also fonts: .font / .fnt -> the sheet + a Font asset
│   ├── phxtile/                  CONVERTER: Tiled .tmj -> .phxtmap (layers + parallax + spawns +
│   │                             per-tile collision flags from tileset properties; flag-less
│   │                             maps: non-empty tiles on the gameplay layer = solid)
│   ├── phxsnd/                   CONVERTER: WAV / .sfx / .song -> .phxsnd (mono16; tier 0 resamples to the GBA device
│   │                             rate at bake time — ADPCM is future)
│   ├── phxbin/                   CONVERTER: JSON -> .phxbin flat table (+ generated .gen.h accessor)
│   ├── phxtmap/                  GUI tilemap editor: the Studio's map panel (ed_map.cpp) in a one-document
│   │                             window (solo.h). editor.h = the headlessly-tested .tmj document model
│   ├── phxentity/                GUI data-table editor: the Studio's table panel (ed_table.cpp) standalone.
│   │                             editor.h = the headlessly-tested phxbin JSON document model
│   ├── phxstudio/                Phosphorus Studio — THE editor: Explorer + tabs of documents in the CODE,
│   │                             SPRITE/PIXEL, TILEMAP, DATA-TABLE, SOUND-EFFECT, SONG, FONT and DIALOGUE editors;
│   │                             budget.h (the Budget + Profiler views' model) · settings.h (File >
│   │                             Settings: scale, session, SDK/emulator paths -> launch env) ·
│   │                             CMakeLists.txt (phxnew; phxstudio/phxtmap/phxentity with PHX_USE_SDL)
│   │                             (ed_*.cpp; workspace.cpp),
│   │                             plus the module graph + caps tiers, the bundle browser with per-render-
│   │                             tier previews, and one-click games/editors/gates/suites/console builds.
│   │                             Headless models (editors + pipeline suites): textdoc.h · syntax.h ·
│   │                             pixeldoc.h · project.h · model.h · projectdoc.h (game projects:
│   │                             phxproject.json, the new-project template, and the ACCESS POLICY —
│   │                             a project writes only its own folder, reads the engine's public API).
│   │                             host.h = the panel<->host seam ·
│   │                             solo.h = one-document host · jobs.h = process runner (winjob.cpp =
│   │                             its Windows half: sh.exe in a Job Object) · main.cpp = shell ·
│   │                             new_main.cpp = `phxnew DIR [NAME]` (File > New project, headless) ·
│   │                             components.h = the game's reflected components (build/components.json)
│   │                             + the table editor's prefab inspector logic · spawnart.h = spawn type ->
│   │                             its prefab sprite's first frame (the map editor draws spawns with it)
│   └── common/                   depcheck.py (layering gate) · size_gate.py (GBA budget gate) ·
│                                 bin2s.py (portable asset embedding) · debug_font.h (shared tool font) ·
│                                 ascii_font.h (full-ASCII 5x7 tool font) · twk.h (+ twk_geom.h,
│                                 twk_icons.h: the tool widget kit) · ttf_text.h/.cpp + text_raster.h +
│                                 jetbrains_mono_data.h (the Studio's TrueType text; fonts/ = the OFL
│                                 face, third_party/ = stb_truetype) · png_write.h (PNG encoder) ·
│                                 bake_project.py (a game project's assets -> one .phxp, `make game-assets`) ·
│                                 export_project.py (a built project -> dist/<name>-<target>/ + .zip,
│                                 `make game-export`: the release exe + bundle + DLLs, the ROM, the EBOOT) ·
│                                 doxyfilter.py (promotes '//' header comments for `make docs`)
│
├── examples/
│   ├── platformer/               The MVP gate: full game slice using ONLY engine systems ✅
│   │   └── src/                  components.h (EngineCtx) · systems.cpp (logic+scenes) · main.cpp (PC) ·
│   │                             gba_main.cpp/gba_ppu_main.cpp/psp_main.cpp (console entries) ·
│   │                             bake.h + bake_main.cpp (host asset baker, per-target tier)
│   ├── emberwing/                Emberwing: Cinder Hollow — the polished vertical slice ✅ (README.md
│   │   └── src/                  = engine analysis + level design). game.h (EngineCtx/components) ·
│   │                             systems.cpp + scenes.cpp (game) · art.h (ASCII-grid pixel art) ·
│   │                             audio_gen.h (SFX + chiptune synth) · level.h (ASCII sections -> .tmj) ·
│   │                             bake.h/bake_main.cpp · main/desktop_main/gba_main/gba_ppu_main/
│   │                             psp_main entries (`make emberwing[-ppu|-sdl|-gl]`,
│   │                             `gba-emberwing[-ppu]` — the PPU ROM is the shipping GBA build —
│   │                             and `psp-emberwing`)
│   ├── tinyllm/                  A quantized transformer LANGUAGE MODEL on a GBA ✅ (README.md =
│   │   ├── src/                  the memory budget + measured tok/s). llm_format.h (the .phxllm
│   │   │                         blob: engine-free POD) · fixedmath.h (divide-free Q16.16:
│   │   │                         Newton reciprocal, integer isqrt, ROM exp/SiLU LUTs) ·
│   │   │                         llm.h/llm.cpp (float-free, heap-free inference core; weights
│   │   │                         streamed IN PLACE from cartridge ROM) · llm_build.h (host
│   │   │                         .phxllm writer + the generated test fixture) · text_font.h ·
│   │   │                         tinyllm.h/.cpp (BG-tilemap text console) · bake.h/bake_main.cpp ·
│   │   │                         main/desktop_main/gba_ppu_main entries (`make tinyllm[-sdl]`,
│   │   │                         `gba-tinyllm-ppu` = the shipping ROM)
│   │   └── tools/                export_model.py: checkpoint -> int8 .phxllm, + a BIT-EXACT
│   │                             Python mirror of the device arithmetic (writes the goldens)
│   ├── gba_smoke/ gba_ppu/       Console bring-up + verification smokes (one concern each):
│   ├── gba_audio/ gba_save/      render, PPU hardware, Direct Sound, battery-SRAM save
│   └── psp_smoke/ psp_gu/        PSP equivalents: render, sceGu display list,
│       psp_audio/ psp_save/      sceAudio device, sceIo save (verdicts via PPSSPP log / GDB stub)
│
└── tests/                        Host test suites (the architecture's safety net). See tests/README.md.
    ├── README.md                 the test layout, the harness API, how to add a test
    ├── phx_test.h                tiny in-house unit harness (PHX_TEST / CHECK*) — no GoogleTest
    ├── unit/                     main.cpp (runner) + test_*.cpp — ALL linked into one binary
    │                             (build/phx_tests): fixed, memory, ecs, time, input, physics,
    │                             anim, scene, ui, audio, stream, lz, png, json, wav, cmdqueue,
    │                             viz, particles, tinyllm_{math,format,tokenizer,font}
    ├── suites/                   *_test.cpp — headless integration binaries, one per `make`
    │                             suite target, each with its own main() (smoke render ppu gu
    │                             playable physics anim scene ui platformer emberwing miracle
    │                             tinyllm audio texcache png sprite tiled resource pipeline)
    ├── verify/                   real-device verification (SDL window / GL readback / live audio
    │                             device) — the sdl-verify / gl-verify / audio-verify targets
    └── fixtures/                 shared test data (png_fixtures.h: in-source PNG byte blobs)
```

Release gates beyond `make check`: `make determinism` (both scalar tiers byte-identical),
`make sanitize` (ASan+UBSan), `make size-gate` (GBA budgets), the Windows suite under Wine
(`make win-verify`) — all CI jobs.

## The two rules that keep this clean

1. **Include only `include/phx/<mod>/`** of another module — never its `src/`.
   Dependencies point *downward* only (core never includes ecs). A back-edge is a
   build break, caught by `tools/common/depcheck.py`.
2. **One backend per module per build.** Platform/render backends live in `src/<name>/`
   subfolders; `phx_add_module(... BACKENDS ...)` links exactly the matching one. No
   `#ifdef PLATFORM` in gameplay or systems code — ever. (In per-tier backend code,
   `PHX_TARGET_GBA` = the fixed-point scalar tier — also set by host `TIER=gba_sim` —
   while `PHX_GBA_HW` = real silicon; guard MMIO with the latter.)
