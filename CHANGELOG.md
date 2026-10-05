# Changelog

All notable changes to Phosphorus are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[semantic versioning](https://semver.org) (pre-1.0: MINOR may break, PATCH never does — see
[RELEASING.md](RELEASING.md)).

## [Unreleased]

### Changed

- Renamed the engine to **Phosphorus Engine** and the editor to **Phosphorus Studio**.
  CMake SDK packages and release archives now use `phosphorus`; `phx` APIs, commands,
  filenames, and asset formats remain unchanged.

- **Phosphorus Studio's text is now a real typeface, not pixel art.** Every label, menu, editor line
  and log line is JetBrains Mono (SIL OFL 1.1), anti-aliased and drawn at window resolution, so it
  stays sharp at any UI scale.
  - The layout is unchanged: the face is sized so each character still advances exactly 6 canvas
    pixels on a 10-pixel line, so no widget, column or caret moved.
  - Text goes to a new **native-resolution overlay** in the desktop seam
    (`phx_desktop_overlay_begin`, `phx/platform/desktop.h`): an RGBA layer with one pixel per
    window pixel, alpha-blended over the upscaled canvas at present. The SDL backend implements it;
    the null backend keeps it in memory so tests can read it back. Games never call it.
  - `twk::Gui::set_text_raster()` opts a tool in (`tools/common/text_raster.h`; the rasterizer is
    `tools/common/ttf_text.h`, over the vendored public-domain `stb_truetype.h`). Text is hidden
    where something on a higher plane or sub-layer covers it, and dimmed under a modal's backdrop.
    Tools that do not opt in (`phxtmap`, `phxentity`) and any platform without an overlay keep the
    5x7 bitmap font.
  - `phxstudio --shot` now writes every window pixel (1280x720 at the default scale) instead of the
    640x360 canvas.

### Added
- **Debugger, profiler and settings.**
  - **In-game developer tools** (`phx/runtime/devtools.h`) gain:
    - **F2**: every collider, plus the level's collision tiles.
    - **F3**: slow motion (1/2, 1/4).
    - **F9**: halt on warnings (pause when the engine logs a warning or an error).
    - **Live editing**: PgUp/PgDn and -/= change the selected entity's Transform, velocity and
      reflected fields while the game runs.
    - A **frame-time graph** of each frame's work against the step budget.
    - `PHX_TRACE=file`: a per-frame timing CSV. `DevHooks` gains `stop`, called at teardown.
  - **Phosphorus Studio**:
    - A **Profiler**, in the Budget view: a Profile launch records `build/trace.csv`. The view
      shows a per-frame update/render/present graph, frames whose work overran the step, per-phase
      avg/p50/p95/max and the heaviest frames.
    - A **Debug** launch (`make game-debug`): plays under gdb and prints every thread's backtrace
      on a crash.
    - **File > Settings**:
      - Studio tab: UI scale (Ctrl+= / Ctrl+- are remembered too), session restore, and the SDK
        and emulator paths, which become the launches' environment.
      - Project tab: `phxproject.json`'s name, folders, bundles and launches, edited in place.
  - **CMake builds the Studio:** `phxnew` always, and `phxstudio` / `phxtmap` / `phxentity` with
    `-DPHX_USE_SDL=ON`. A new `cmake-studio` CI job builds them on Linux.
- **Exporting games.** `make game-export PROJECT=... EXPORT=pc|gba|psp` builds a project and
  packages it for players in `<project>/dist/<name>-<target>/` plus a `.zip`
  (`tools/common/export_project.py`).
  - **PC:** a release build, its bundle, and, on Windows, every DLL it loads from the toolchain
    (SDL2 and the C++ runtime, found by walking the import tables), with a README.
  - **GBA:** the size-gated ROM.
  - **PSP:** `PSP/GAME/<NAME>/EBOOT.PBP`.
  - A new desktop seam call, `phx_desktop_use_exe_dir()`, lets an exported game find
    `build/<name>.phxp` from its own folder, so it runs when double-clicked or started from any
    directory. This was verified with only the Windows system PATH.
  - Phosphorus Studio adds **Export for PC / GBA / PSP** launches.
- **Measured budgets.**
  - `App::peaks()` tracks each run's high-water marks: entities, sprites per frame, dropped
    sprites, tiles, frame scratch and the arena.
  - `phx::write_budget_report()` (`phx/runtime/budget.h`) writes them as JSON against the target's
    limits, together with the engine warnings and errors logged (`log_count()`).
    `TargetProfile` gains the target's sprite and audio-channel ceilings.
  - `make project-budget PROJECT=...` bakes every tier and runs the game headlessly as PC, GBA
    (fixed-point, PPU model, GBA budget) and PSP for 30 s of scripted play, writing
    `build/budget-<target>.json`.
  - Phosphorus Studio's new **Budget** view shows each target's card: memory, entities, sprites per
    frame, frame scratch, warnings, ROM or bundle size, textures the GBA can't store as tiles, and
    the biggest assets. Each figure is marked ok, close (over 90%) or over, and the view flags
    reports older than the project's assets or code.
  - Projects made before a standard launch existed get it in the Studio's Run view.
- **Dialogue.** A `.dlg` holds conversations: lines (speaker, text, `next`) and choices.
  - `if` conditions skip a line or hide a choice (`coins >= 5`, `key`, `!key`). `do` effects set,
    add to or subtract from variables (`coins -= 5, key = 1`).
  - It bakes to a new **Dialogue** asset (`AssetType::Dialogue`): index tables and strings read
    in place, with every index bounds-checked at load (`ResourceCache::dialogue()`). The
    compiler, validation and a simulator live in `tools/phxpack/dialogue.h`.
  - **`phx::DialogueRunner`** (`phx/runtime/dialogue.h`) plays a conversation in a box along the
    bottom: speaker tab, portrait, typewriter reveal, and a choice panel (Up/Down + A). Its
    variables come through `DialogueVars`.
  - `GameFlow` gains a **`talk`** screen kind (a cutscene: its `dialogue` column) and plays the
    new stock **`Talk`** component's conversation when the player presses Up at it (with an
    "UP: TALK" hint). Conversation variables are the flow's totals, so `do coins -= 2` changes
    the HUD.
  - Phosphorus Studio gains a **dialogue editor**: conversations and speakers; line cards with
    speaker, text, next, `if` / `do` and choices; a play panel running the compiled
    conversation by the runtime's rules, with editable variables; and the bake's
    problems/warnings. There is also a **New dialogue** command, and the asset view lists a
    Dialogue's conversations.
  - New projects get `assets/dialogue.dlg` and a talking sign in the level.
  - A new `dialogue` suite (in `check` and `determinism`) checks the runner against the Studio's
    simulator step by step, and plays a talk screen and a Talk NPC trading coins from data.
- **Fonts.**
  - **Author formats:** a `.font` over a grid sheet PNG, or an imported BMFont text `.fnt`. The
    Font asset (formerly reserved) now bakes a glyph table: each character's rect, offset and
    advance (`FontBlobHeader` / `FontGlyphDef`, `ResourceCache::font()`), over the sheet texture.
    - `.font` widths are proportional (measured from each glyph's pixels) or fixed.
    - `.fnt` comes from BMFont, Hiero, Littera and similar tools.
  - **Bake:** `phxsprite` bakes both (`tools/phxpack/font.h`), and `bake_project.py` routes them.
  - **Runtime:**
    - `phx::load_font()` (`phx/runtime/font.h`) turns the asset into a `BitmapFont` whose glyph
      table is read in place; an older project's plain `font.png` still loads as the classic grid.
    - `BitmapFont` gains an optional `FontGlyph` table.
    - `UI::text_width()` / `glyph_advance()` measure text, and `text()`, `button()` and the
      dialogue wrap all lay out by pixel width. A fixed-width font lays out exactly as before.
    - The game flow uses `load_font` and centres and right-aligns by measured width.
  - **Studio:** a **font editor** shows the sheet with each glyph's measured box (click a glyph
    to paint it), the font settings, and a live sample line with its width. **New font**, **New
    sound effect** and **New song** join the Explorer and welcome-page create menus. The asset
    view shows a Font's metrics and glyph table.
  - **Template:** new projects' `assets/font.font` makes the flow's text proportional.
- **Sound effects and music, authored in the Studio.**
  - `tools/phxpack/synth.h` renders two new author formats to PCM at bake time.
    - **`.sfx`**: an sfxr-style parameter set (wave, pitch, slide, vibrato, arpeggio, duty,
      envelope, filters). It comes with seeded presets plus randomize and mutate.
    - **`.song`**: a pattern tracker (up to 8 channels, synth instruments with an envelope,
      slide and vibrato, patterns of up to 128 rows, an order list).
  - `phxsnd`, `phxpack` and `bake_project.py` bake both to ordinary Sound assets named after the
    file, including the tier-0 resample. The runtime is unchanged: `GameAudio::play()` /
    `play_music()`.
  - Phosphorus Studio gains a **sound effect editor** (presets, randomize/mutate, parameter sliders,
    waveform, auto-play) and a **song editor**. The song editor has a tracker grid with piano-key
    note entry, patterns, an order list and instruments, and plays the song or a pattern with a
    playhead.
  - Both editors play the exact PCM the bake produces, through a new `Host::play_pcm()`. Numbers
    are saved at the shortest precision that reads back exactly, so the bake renders what the
    editor played.
  - The flow table's `music` screens take a `music_vol` column (percent, default 60) to leave
    headroom for sound effects.
  - New projects get `jump.sfx`, `coin.sfx` and a `theme.song` started by the title screen,
    replacing the generated WAVs.
- **Animation state machines as data.** A sprite's clips can carry transitions:
  `trans <from|*> <to> <trigger>` lines in a `.sprdef`, or a `"transitions"` list in sprite
  `.json`. The bake resolves clip names to indices, so an unknown clip fails the bake, and writes
  a backward-compatible trailer after the clips (`SpriteTransDef`, `SpriteView::trans`).
  `phx::Animator` now carries its edges and `trigger(name)`. An edge from the current clip beats
  a `*` edge, and a `*` edge into the playing clip does nothing. The anim system fires `done`
  when a non-looping clip ends; a one-frame non-looping clip now finishes after its frame time.
  `Level` gives every spawned animator its sprite's transitions, read in place from the bundle
  (the baked layout is `AnimEdge`'s). A sprite keeps up to 12 clips, up from 8. When a sprite has transitions, `PlatformerController` sends it
  `jump`/`fall`/`land`/`move`/`stop`/`hurt` instead of playing clips by name, and games call
  `phx::anim_trigger(world, e, "attack"_hash)`. Phosphorus Studio's sprite editor gains a
  Transitions list in the Clips tab. Renaming or deleting a clip updates its transitions, and the
  asset view counts them. The project template's hero gains jump and land frames and a six-edge
  state machine. `AnimStateMachine::Edge` is now `AnimEdge`, whose trigger is a `NameHash`.
- **The game flow as data.** `phx::GameFlow` (`phx/runtime/flow.h`) runs a flow table
  (`assets/flow.json`, one row per screen) with title, level and end screens:
  - chaining by `next` or an Exit's target;
  - lives, and a "gameover" screen;
  - START to pause;
  - a HUD of a counter and the lives left;
  - counters totalled across levels;
  - per-screen music.

  Text is drawn with `phx::UI` from a font sheet, so it works on every target.
  - **`Level` caches uploaded maps**, so revisiting or restarting a level reuses its tilemap slot
    and tileset. Renderer slots are never freed, so repeated restarts no longer run out.
  - **`ResourceCache::has(name, type)`** probes quietly: a sprite name that is a plain texture
    (the door) no longer logs a warning.
  - **The Studio's placeable spawn types** come only from tables with a `type` column, so a flow
    table's screen names aren't offered.
  - **The template is a complete small game:** a title screen with the project's name, the level
    with 3 lives and a coin HUD, a door (stock `Exit`) to a "you win" ending, and a game over
    that retries. It adds `font.png` and `door.png`, and `src/main.cpp` only runs the flow.
  - The new `flow` suite covers it on both scalar tiers.
- **A scene view that looks like the game, Play from here, and in-game developer tools.**
  - The map editor draws each spawn as its prefab's sprite (the first frame of its "idle" clip),
    centred where the level puts it, and selects it by that sprite. `spawnart.h` resolves type →
    prefab `sprite` → the project's `.sprdef`/`.png`.
  - **Play from here** (▶, Shift+F5 at the pointer) saves the map and runs the project's Play
    with `PHX_PLAY_FROM=x,y`. The desktop entry passes it to `set_start_override`, and
    `Behaviours::start` puts the player there.
  - **Developer tools** (`phx/runtime/devtools.h`; desktop builds only, installed by the engine's
    desktop entry): F1 overlay, F5 pause, F6 single step, F7/F8 or click to select, and a live
    inspector of the selected entity. The inspector shows its Transform, Body, collider, animation
    and every reflected component's fields. The overlay draws into the software framebuffer after
    the frame.
  - Supporting changes: `App::set_dev_hooks`, `Renderer::camera()`, `ComponentInfo::get`, and
    `Level::active()`.
- **Stock behaviours: common gameplay without code.** `phx/runtime/behaviours.h` provides these
  as engine components a prefab lists in its `components` column and tunes with columns or
  spawn properties:
  - `PlatformerController`: run, jump, clips by name, jump sound; hazard tiles respawn it;
  - `Patrol`: back and forth within a range, turning at walls;
  - `Pickup`: counters and a sound;
  - `Hazard`: sends the player back to the respawn point;
  - `Checkpoint`: becomes the respawn point;
  - `Exit`: reports a target level;
  - `CameraFollow`: the camera follows it, clamped to the level.

  One `Behaviours` system runs them in order: control, patrol, physics, contacts, animation,
  camera. It exposes `counter()`, `hits()`, `exit()`, `respawn_point()` and `camera()`.
  - `play_clip()` plays a clip by name. `SpriteRenderer` now carries its sprite's clip names.
  - phxbin gains **`str64`**. A `components` list such as "PlatformerController CameraFollow"
    (33 characters) no longer fits a `str32`, which silently cut it off.
  - The **template** is now data only: player, coins and a new patrolling **slime** enemy are all
    stock components, and `src/main.cpp` just loads the level and updates the behaviours.
  - The new `behaviours` suite plays a data-only level with scripted input on both scalar tiers.
- **Component reflection and a prefab inspector.** A game declares its components once with
  `PHX_COMPONENT(Enemy, PHX_FIELD(Enemy, range), …)` (`phx/ecs/reflect.h`). Supported field types
  are ints, bool, `scalar` and `PHX_FIELD_HASH` names. The registry is static, with no heap.
  - **Level:** a prefab's `components` column attaches reflected components, filled from its
    `Enemy_range` columns (a spawn property of the same name overrides it); a field with no value
    keeps its C++ default. Scalars come out identical on both tiers.
  - **Schema export:** `make game` runs the built game with `PHX_DUMP_COMPONENTS` to write
    `build/components.json` (names, field types, defaults) without opening a window.
  - **Studio:** the table editor's new COMPONENTS section ticks components on a prefab record,
    creating their typed columns at the C++ defaults.
  - `TableView::get_q16` and `SpawnsView::get_q16` read decimals exactly on both tiers.
  - **Tests:** `level`, `smoke` (schema export) and `editors` (the inspector model) cover it,
    and `project-check` now also checks the export.
- **Per-spawn properties.** The map editor's spawn inspector has a PROPERTIES list (name, int,
  float, bool or string, value), saved as Tiled custom properties; `tiled_load` imports them.
  - The bake adds an optional **spawn extension** after the `SpawnDef`s: each spawn's name hash,
    its typed properties and a string table. The 12-byte `SpawnDef` is unchanged, and maps with
    bare spawns bake exactly as before.
  - `SpawnsView` gains `name()`, `find_named()`, `get_int()`, `get_str()` and `get_hash()`.
  - In `phx::Level`, a property named like a prefab column **overrides** it for that one entity.
    `level.get_int(ref, key, def)` / `get_str` / `get_hash` read any setting as "the spawn's,
    else the prefab's, else the default", and `level.find_named()` finds a spawn by its editor name.
  - The map editor now JSON-escapes spawn names and types when saving.
- **Levels load themselves: spawns placed in the map editor become entities.** `phx::Level`
  (`phx/runtime/level.h`) takes a map from the bundle and, in one `load()` call:
  - uploads it with its parallax, and builds the physics grid from the last layer with its
    per-tile collision;
  - turns every spawn into an entity built from the **prefab table** (`assets/prefabs.json`) row
    whose `type` matches. The columns `sprite`, `clip`, `w`/`h`, `layer`/`mask`, `body`, `z` and
    `collide` are read by name.

  Each entity gets a `Transform` and a `PrefabRef` (its type and row). `SpriteRenderer` and
  `draw_sprites()` draw them, following an `Animator`. `LevelOptions::on_spawn` lets a game add
  its own components, and `level.prefabs()` reads its own columns. The new `level` suite covers
  it on both scalar tiers.
  - **Self-describing data tables.** phxbin appends a schema trailer (column name hash, type,
    offset). `phx::TableView` (`phx/resource/table.h`) reads any column by name. Existing
    readers are unaffected.
  - **The project template is a small platformer driven by that data:** hero and coin prefabs,
    run and jump, coins with a pickup sound, and spike tiles. `project-check` runs it on all
    three profiles.
- **Engine-owned sound: `App::audio()`.** A game plays sounds with
  `app.audio().play(to_sound(res->sound("jump"_hash).unwrap()))`, plus `play_music` and
  `stop_all`, and never calls a platform function. The engine owns the mixer, the lock-free
  command queue and each target's device: SDL on PC, sceAudio on PSP, DirectSound on GBA.
  - The platform seam's `audio()` now returns a `phx_audio { rate, start, stop }` device on
    SDL, GBA and PSP.
  - Nothing starts until the first sound. Games with their own mixer (Emberwing, the Studio)
    are unaffected, and games that play nothing link no mixer.
  - Without a device (the null platform), the App mixes headlessly; `plays()`, `peak()` and
    `frames_mixed()` report what would have been heard.
  - The project template plays a jump sound on A, and `project-check` asserts it on all three
    profiles. `make game-audio-verify` checks a real SDL device.
- **Game projects build for GBA and PSP.** `make game-gba | play-gba | game-psp | play-psp
  PROJECT=path` turn a Studio project into a size-gated `.gba` ROM (native PPU) or a PSP
  `EBOOT.PBP`, opened in mGBA / PPSSPP when installed. New projects get **GBA ROM** and **PSP
  EBOOT** in the Run view.
  - A game names its Game with `PHX_GAME(MyGame);` (`phx/runtime/main.h`) and has no `main()`:
    the engine supplies one per target (`engine/runtime/src/entry/`) and boots the game with
    that target's profile. The GBA gets a 160 KB arena and a 240×160 screen, the PSP a 4 MB
    arena. The new `Game::on_configure(Config&)` hook is where the game sets its title and
    resolution. Projects with their own `main()` still build for PC as before.
  - `phxnew DIR [NAME]` creates a project from the command line. `make project-check` (now part
    of `make check`) bakes the template for all three tiers and runs it headlessly under each
    profile, the GBA run in fixed point on the PPU model. CI builds the template as a real ROM
    and EBOOT.
  - `bin2s.py --name SYM`; launch `needs` may name `$DEVKITARM`; `PSPDEV` defaults to the
    install that `psp-g++` on `PATH` comes from.
- **Editing assets after they're made, in Phosphorus Studio.**
  - The Assets view has **edit source**: it opens the file an asset was baked from (a texture's
    PNG, a sprite's def, a map's `.tmj`, a table's `.json`). Double-clicking an asset does the
    same. In a project, **bake** rebakes the assets and reloads the bundle.
  - Plain PNGs such as tilesets open in **tile mode**. The tile grid is labelled with GIDs, and
    its size is detected from the map or sprite that uses the image. The strip steps and zooms
    through tiles, and the Image tab can add a row of tiles.
  - In the map editor, double-click a palette tile (or right-click > Edit tile) to paint it,
    zoomed on that tile. Open maps redraw when the tileset PNG is saved. A map drawn with swatch
    colours gets **create tileset…**, which writes a real tileset PNG to paint.
- **Game projects and a project boundary in Phosphorus Studio.**
  - A project is a folder with a `phxproject.json` (name, code and asset folders, bundles, and the
    Run view's launches). The Studio opens one project at a time (`--project DIR`, a project
    picker, or File > New / Open / Close project).
  - The Studio can **write only inside the project folder**. It can **read the engine's public API
    headers** (`engine/*/include`, under ENGINE API in the Explorer and quick open, in a locked
    editor) and its docs. Everything else is never opened: the engine's sources, tools, tests,
    Makefile and other projects. This covers the Explorer, quick open, `--open`, drops,
    compiler-error links and sprite/tileset references.
  - Paths are canonicalised, so `../` and symlinks can't escape.
  - The module graph and the engine's gates and suites are hidden in a project. `--engine-dev`
    restores the whole-checkout Studio for engine work.
- **File > New project** creates a small, complete game from a template (`src/main.cpp`, a hero
  sprite, a tileset, a level). New generic Make rules build any project with no Makefile edit:
  `make game`, `make game-assets` (via `tools/common/bake_project.py`, `TIER=0|1|2`) and
  `make play PROJECT=path`. Games compile against the engine's **public headers only**.
- `phxproject.json` for the examples (`emberwing`, `platformer`, `miracle-player`, `tinyllm`), so
  they open as projects.
- **Phosphorus Studio is now the editor for the engine.** A new **Editor** view (the default)
  has an Explorer over the repository, tabs of documents, quick open (Ctrl+P), New-asset
  templates, save / save-all with confirm-on-close, reload-on-external-change and session
  restore. Each document opens in its own editor:
  - **Code**: syntax highlighting for C/C++, JSON, Makefile, shell, Python, CMake, Markdown and
    `.sprdef`; auto-indent; word-granular undo; find/replace; go-to-line; comment, duplicate
    and move lines; UTF-8-safe editing. Compiler errors from the Run view become clickable links
    and gutter markers.
  - **Sprite / pixel**: the pencil, eraser, fill, line, rectangle, ellipse, picker and marquee
    tools; floating selections, mirror painting and palettes. GBA BGR555 snapping and the
    8×8-tile 15-colour check. For sprite defs: frame grid, onion skin, named clips and a live
    preview. It saves real PNGs and `.sprdef` / sprite `.json`.
  - **Tilemap**: see the rebuilt `phxtmap` below.
  - **Data table**: see the rebuilt `phxentity` below.

  The Studio window is now resizable, with an integer UI scale (Ctrl+= / Ctrl+-). It adds menus
  (File/Edit/View/Build/Help), toasts, modal dialogs, tooltips and a shortcut sheet (F1).
  `--open`, `--fresh`, and a much richer `--script` (key, type, drag, wheel, dclick, open)
  cover the new features.
- **`phxtmap` and `phxentity` rebuilt** as the Studio's map and table panels in one-document
  windows, so there is one implementation of each.
  - The **map editor** draws the real tileset art. It adds brush, eraser, fill, rectangle,
    picker and select tools with multi-tile stamps; collision flags with an overlay; spawns you
    can place, drag and inspect; layer add/delete/reorder/rename/parallax with a parallax
    preview; map resize with an anchor; and undo for all of it. The tileset `image` round-trips,
    so Tiled opens the map with its art. New flags: `--tile`, `--tileset`, `--scale`, `--shot`.
  - The **table editor** is a spreadsheet with in-cell typed editing (strings are finally
    editable) and schema editing (rename, retype, reorder fields). It adds a record inspector,
    duplicate-name and over-long-text checks, and undo.
- **`phx/platform/desktop.h`**: a desktop-only extension of the platform seam for tools. It
  covers key events with modifiers, typed text, right/middle buttons and the wheel, the
  clipboard, a resizable window with an integer UI scale, cursors, titles, dropped files, and a
  vetoable window close. It is implemented by the SDL backend and as a scripted queue in the
  null backend (`phx_null_desktop_push`). No console backend or game uses it, so ROMs and game
  behaviour are unchanged.
- **The tool widget kit `tools/common/twk.h`** (+ `twk_geom.h`, `twk_icons.h`): an immediate-mode
  GUI over `phx::UI` with clipping, planes, text fields, number fields, dropdowns, menus, modals
  and pixel-art icons. **`tools/common/png_write.h`**: a dependency-free PNG encoder (indexed or
  RGBA, deflate).
- **`make editors`** (on `check`): a new headless suite that covers the desktop seam queue, the
  widget kit, and every editor document model. Each saved form is re-read by the bake's own
  loaders (`load_sprdef`, `load_sprjson`, `tiled_load`, `build_bin`).
- **Phosphorus Studio (`tools/phxstudio`, `make studio`) — a graphical hub over the whole engine**,
  built on the engine itself (App loop, SDL window, software golden renderer, `phx::ui`).
  *Overview*: the module dependency graph as built (layers from `depcheck.py`, edges from real
  `#include`s) with per-module details, and the GBA/PSP/PC capability tiers parsed from
  `caps.h`. *Assets*: every `.phxp` validated like `ResourceCache::mount()`, with live previews
  — textures re-encoded per render tier by the bake's own encoders (GBA palettes, 8×8
  colour-budget grid), animated sprite clips, tilemaps through the real parallax path with
  collision/spawn overlays, sounds on the real mixer, spawn tables, blob hex. *Run*: one-click
  games, editors, gates, every `make check` suite (pass/fail chips) and console builds, with
  live colour-classified output and a stop that kills the whole process tree. `--shot`,
  `--script` and `--run` make it scriptable. Its headless model is covered by `make pipeline`.
- **`tools/common/ascii_font.h`**: a full printable-ASCII 5×7 tool font (the shared
  `debug_font.h` is uppercase-only).
- **`phx_sdl_set_window_scale()`** (SDL platform backend): a desktop-only, pre-init hook to pick
  the window's integer scale (default unchanged at 3×), for tools with larger canvases (now
  also reachable as `phx_desktop_set_scale()`).

### Fixed
- `FrameProfile` (`App::profile()`) reported milliseconds in its `*_us` fields: the loop's
  ns-to-µs multiply-shift used 4295/2^32 (10^-6) instead of 4294967/2^32 (10^-3). Every figure was
  1000x too small, so a 16.7 ms frame read as 17 µs, the profile overlay's ms readout showed 0.0,
  and the budget tick was never reached.
- The `emberwing` and `emberwing-ppu` suites wrote the same bundle and save file, so under
  `make check -j` one run could clobber the other's save ("the goal run's clear was persisted").
  The PPU build now uses its own `build/emberwing_ppu.*`.
- Phosphorus Studio on Windows: opened files kept native `\` separators, so a sprite def naming its
  sheet by bare file name (every template sprite) failed to open with "the sheet is outside the
  project". Opened paths and bundle listings now always use `/`.
- **`make game-assets TIER=0|1` rebuilt every host tool.** A bake tier was also taken as the
  scalar tier, so it switched the host object directory and relinked every host binary. It is now
  only a bake tier.
- **The GBA PPU backend crashed on an arena too small for its stores.** It wrote through a null
  allocation; `Renderer::create` now fails with an error instead.
- **Phosphorus Studio's Run view on Windows.** Launches went to `cmd.exe`, which can't run their
  POSIX shell commands, so every Play, Build and suite failed at once with no output. They now
  run through MSYS2's or Git for Windows' `sh.exe`, found automatically (or set `PHX_SH`), and
  **Stop** ends the whole process tree. Launches that need a tool (`sdl2-config`, …) are no
  longer greyed out: `PATH` is now split on `;` and `.exe` names are found. The suite chips no
  longer show a stray `\r` from a CRLF checkout of the Makefile, and without `HOME` the session
  and recent projects are kept in `%APPDATA%\phxstudio`.
- **`BinDoc` (phxbin tables in the editors) truncated `f32` fields to integers on load.** A float
  column opened in `phxentity` (or the Studio) and saved lost its fractions. Floats are now kept,
  edited and saved exactly.
- **`examples/tinyllm` — a quantized transformer language model running on Game Boy Advance
  hardware.** A 260K-parameter llama2-architecture model (RMSNorm, RoPE, grouped-query attention,
  SwiGLU) generates English text on screen at **2.17 tokens/sec**, measured on mGBA. The int8
  weights live in cartridge ROM and are streamed in place — the console's 256 KB of EWRAM never
  holds a single weight. The inference core is float-free, heap-free and STL-free, so `make
  determinism` proves the PC (float) and GBA (fixed16) builds emit byte-identical tokens.
  New host exporter `examples/tinyllm/tools/export_model.py` (llama2.c `.bin` or PyTorch
  `state_dict` → a versioned `.phxllm` blob) which also carries a bit-exact Python mirror of the
  device arithmetic, used to generate the golden tokens the suite asserts against. Targets:
  `make tinyllm`, `tinyllm-sdl`, `gba-tinyllm-ppu`, `size-gate-tinyllm`, `tinyllm-test` (on
  `check` and `determinism`).
- **`phx_gba_vblank_clock_start()`** (GBA platform backend): installs the VBlank IRQ — and with
  it the true elapsed-frame counter the fixed-step accumulator uses — *without* starting audio.
  Previously only `phx_gba_audio_start()` did, so a compute-heavy silent ROM had its sim clock
  dilate (one step per loop iteration regardless of how many vblanks really passed).
- **`make -j determinism` failing on suite completion order.** The gate diffed the two tiers'
  outcome lines in print order, which varies when suites run concurrently; it now compares them
  sorted, with each suite's output kept whole via `--output-sync` (GNU make >= 4.0).

## [0.1.0] - 2026-08-02

First tagged release: the complete engine slice proven on all four targets.

### Added
- **Engine core** — fixed-step App loop; one-arena memory model (arena/stack/pool/object
  allocators, zero hot-path heap); sparse-set ECS; scene, physics (tile collision incl.
  per-tile flags), animation, and UI systems; deterministic `phx::scalar` dual-tier math
  (`float` on PC/PSP, Q16.16 `fixed16` on GBA) held byte-identical by a determinism gate.
- **Rendering** — one 2D-intent API (sprites, tilemaps, per-layer parallax, palettes, zoom,
  shake) with four backends: software golden reference, OpenGL, PSP GU, GBA PPU (Mode-0 tiles +
  OAM, streamed backgrounds).
- **Audio** — block mixer + streaming, per-target sample-rate baking, SDL/PSP/GBA output paths.
- **Asset pipeline** — `.phxp` bundles, zero-copy/LZSS loading, per-target encoding; converters
  (`phxsprite`, `phxtile`, `phxsnd`, `phxbin`) + the `phxpack` assembler; save/load on every
  target including GBA SRAM and PSP savedata.
- **Editors** — `phxtmap` (tilemap, incl. collision-flag painting and prefab palettes) and
  `phxentity` (record tables), both built on the engine itself.
- **Games** — `examples/platformer` (the MVP gate) and **Emberwing — Cinder Hollow**, a
  complete level shipping as GBA ROM, PSP EBOOT, Windows exe, and Linux binary from one tree.
- **Targets** — Linux, Windows (MinGW-w64, static), GBA (devkitARM, ROM size gates), PSP
  (pspsdk); CI gates: full suite, cross-tier determinism, ASan+UBSan, release config, Wine-run
  Windows suite, console cross builds.
- **Release plumbing** — single-source version header (`phx/core/version.h`), `make dist*`
  packaging, CMake install/`find_package(phosphorus)`/CPack SDK, tag-driven release workflow.
- `make gba-miracle-ppu`: the "A Small Miracle" visualizer as a native-PPU GBA ROM (the shipping
  console build). The spectrogram renders as a **BG tilemap** (the PPU can't scale/tint OBJ, so
  ui.rect bars can't work there — the tilemap is the native answer), particles are 8×8 OBJ sparks,
  text is OBJ glyphs; the resident tier-0 song (18157 Hz, ~9.5 MB in cartridge ROM) streams
  through the mixer from the VBLANK audio IRQ. Verified on mGBA (boots, audio DMA streaming, the
  BG spectrogram reacts frame-to-frame). `make size-gate-miracle` passes (ROM 9.8/16 MB, IWRAM
  18.7/28 KB). CMake gains the `miracle` example + gba hook. Mounts the bundle with
  `verify_checksum=false` — CRC32-ing the ~10 MB ROM-resident song took ~27 s of black screen at
  boot; it's self-baked and immutable, and mount still does every structural check.
- `Renderer::refresh_tilemap()` (backend `invalidate_map`): mark a retained tilemap's cells as
  changed in place so the GBA PPU re-streams its cached BG window — enabling a per-frame-rebuilt
  map like the spectrogram. Non-pure default no-op; the software backend reads cells live so it
  needs nothing, and other backends are unaffected.
- `tools/common/size_gate.py --rom-budget-mb`: override the ROM cartridge budget for a ROM that
  legitimately links a large baked asset (the RAM budgets, which guard scarce on-chip memory,
  are not overridable).
- `examples/miracle-player/`: the "A Small Miracle" music visualizer app (engine-only). Streams
  the resident song PCM through the mixer (`AudioStream` + `play_music_stream`, pumped off the
  audio path each frame) and drives a 16-band bars spectrogram (two styles), a beat-spawned
  integer particle pool, bass/loudness-reactive background + beat-synced screen shake/zoom, a
  transport UI, and the intro/outro dedication cards entirely from the precomputed viz track — the
  record index is derived from the audio sample cursor, so audio and video stay locked. Controls:
  START play/pause, LEFT/RIGHT seek ±5 s (re-seeks the stream cursor and viz index together via a
  lock-free handshake), A style, B chrome, SELECT profiler; looping is configurable. `make
  miracle` opens the SDL window (software renderer + real audio); the headless
  `tests/suites/miracle_test.cpp` (in `check` + `determinism`) proves A/V lock across a seek, loop
  continuity, and non-silent streaming on both scalar tiers, and unit tests pin the particle-pool
  bound and the streaming ring's no-underrun invariant.
- `tools/phxviz`: a host-only visualization-track converter (WAV → per-video-frame `.phxviz`
  stream: log-spaced FFT bands, RMS, spectral-flux onset/beat, quantised to `uint8`). The GBA
  reads it zero-copy as a generic `Blob` asset so no FFT runs on the ARM7; A/V lock is a pure
  integer `samples_consumed / hop_samples` map (tier-identical). Format is the engine-free POD
  in `examples/miracle-player/src/viz.h`; `phxpack` merges `.phxviz` intermediates. First slice
  of the "A Small Miracle" music-visualizer ROM (`examples/miracle-player/`).
- `make docs`: Doxygen API reference generated from the public headers (`engine/*/include`
  only — the same boundary depcheck enforces) plus the `docs/` manual, output in
  `build/docs/html`. `tools/common/doxyfilter.py` promotes the house `//` header comments
  to doc comments so the existing prose is the reference; the version is injected from
  `phx/core/version.h` (first slice of the roadmap's v1.0 "written manual" item).

### Fixed
- GBA: eliminated the periodic scroll stutter on PPU builds. The four BG screenblock windows
  were each fully re-streamed (~700 cells + a 1024-entry VRAM rewrite) whenever their layer
  crossed an 8-px tile boundary — and with stacked parallax factors (0.25/0.5/1.0) the
  crossings coincide every 32 camera px, spiking one frame past the vblank budget into a
  visible dropped frame. `draw_tilemap` now streams only the entering rows/columns and the
  hardware push writes only those slots (~32–64 entries per crossed tile instead of 1024).
- GBA: a dropped frame no longer plays as unrecoverable slow motion. The virtual clock now
  advances one sim step per **elapsed hardware vblank** (counted by the VBlank ISR) instead of
  one per game frame, so an over-budget frame is repaid with a catch-up step — matching the
  PSP's real-time-clock behaviour. Without the ISR (no audio started) the old one-step-per-frame
  behaviour is unchanged.
- GBA PPU: sprite flicker and a fixed horizontal cut through static OBJ content (e.g. the
  Emberwing title text sliced in half). The per-frame hardware push (OAM hide-all + rewrite,
  OBJ char VRAM, scroll registers, screenblocks) ran from `end()`, *before* `present()`'s
  vblank wait — i.e. mid-scanout at whatever raster line the game frame happened to finish
  on, so scanlines drawn inside the rewrite window showed no sprites (a fixed band on a
  static scene, a roaming band under frame-time jitter) and scrolling BGs could tear.
  `submit_hardware()` now waits for vblank before touching PPU memory; verified on mGBA via
  the GDB stub that the push starts at VCOUNT≈187 (inside vblank, after the audio ISR) on
  both the title and in-level frames, with pacing still 60 fps.
- GBA: periodic stutter (~once a minute) caused by the virtual clock leaking the loop's five
  1 µs profiler-read ticks per frame into the fixed-step accumulator — after ~3,334 frames the
  residue crossed a whole step and that frame ran two sim steps (a visible hitch, and a likely
  missed vblank on a 16.78 MHz ARM7). `pump_events()` now subtracts the read ticks accrued
  since the last pump from its step advance, so the accumulator sees exactly one step per
  frame; reads still tick so intra-frame phase deltas stay strictly ordered. Same fix in the
  null backend (same convention), where the leak would have broken any exact-step-count test
  running ≥3,334 frames; the smoke suite now soaks 4,000 frames to pin this.
- Docs/comments corrected to match the implementation: desktop `map()` is a load-once heap
  buffer (there is no OS mmap), PSP bundles are linked into the EBOOT (not read via `sceIo` at
  mount), platform backends allocate init-time state outside the root arena
  (`phx_platform_desc.root_arena` is reserved/unused today), and the PC bundle image counts
  against RAM — it is not OS-backed as `docs/06-resources.md` previously claimed.

[Unreleased]: https://github.com/jaden-smb/phoenix/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/jaden-smb/phoenix/releases/tag/v0.1.0
