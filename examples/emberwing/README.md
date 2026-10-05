# Emberwing — Cinder Hollow (vertical slice)

A complete, original 2D pixel-art platformer level built on the Phosphorus engine, running the
**same gameplay code** on GBA, PSP, Windows and Linux. You play **Ember**, a phoenix hatchling
crossing **Cinder Hollow** — a dusk-lit volcanic valley — to reach the **Sungate** and rekindle
it. The level is designed on the pacing/readability philosophy of the first level of the
original NES Super Mario Bros. (safe start → teach by placement → escalate → risk/reward split
→ final skill test), with entirely original art, layout, enemies, mechanics and audio.

Build & play (from the repo root):

```bash
make emberwing            # headless verified playthrough (part of `make check`)
make emberwing-ppu        # the same playthrough over the GBA PPU hardware model (in `check`)
make emberwing-sdl        # desktop window, software renderer + real audio device
make emberwing-gl         # desktop window, OpenGL backend
make gba-emberwing-ppu    # devkitARM -> the SHIPPING GBA ROM (native PPU: Mode-0 BGs + OBJ)
make gba-emberwing        # the software-render GBA ROM (slow; kept as the on-device reference)
make psp-emberwing        # pspsdk   -> build/psp/emberwing/EBOOT.PBP
```

Controls: D-pad/arrows move · A (X in mGBA, Cross on PSP, Z on desktop) jump (hold = higher)
· Start pause · Select frame-profiler overlay.

---

## 1. Engine analysis → design constraints

The game was designed *around* what the engine genuinely offers. The load-bearing findings:

| Engine fact | Design consequence |
|---|---|
| One 2D-intent renderer: sprites + retained tilemaps + **per-layer parallax** (Q16, free BG scroll on GBA) + camera shake/zoom | The level is a single 4-layer tilemap: sky (0.25×), crag silhouettes (0.5×), non-solid deco (1×, lava/ruins), gameplay/solid (1×) — exactly the PPU's four text backgrounds. Shake is used for stomps/eruptions/hurt; zoom stays 1 (the GBA PPU ignores it, so it is not gameplay-load-bearing). |
| GBA **PPU** backend: four 32×32-cell text BGs (arbitrary map sizes streamed through screenblock windows), 16 palette **banks** of 16 colours, 1D-mapped OBJs re-packed per frame, no tint/scale on OBJs | The **shipping GBA build is the PPU tier** (`make gba-emberwing-ppu`, native 240×160): the silicon scans the frame out and the CPU runs only gameplay + audio. The design honours the hardware: every sprite frame is a real OBJ shape (16×16, 16×32, 32×32, 8×8), each sprite texture stays ≤ 15 opaque colours (one palette bank), and HUD states (lost hearts, uncollected shards) are art frames, not tints. This backend was extended FOR this game — the game still shaped itself to the machine. |
| Physics = axis-separated AABB-vs-tile + overlap Hits; **out-of-bounds tiles are solid** (closed box); `solid_from = 1` on the *last* tile layer | Pits cannot kill by falling out of the world: every pit floor is **lava** — deco-layer tiles for the visual plus a merged rect **hazard spawn** for the kill. Non-solid visuals (lava, ruins) live on the deco layer so they never collide. |
| No hot-path heap; entities/components are pool-backed; GBA caps: 256 entities, 128 sprites, 160 KB engine arena | Entity budget ≈ 120 live (≈85 spawns + bounded 16-slot particle bursts). Every array in game code is fixed-size; systems allocate nothing. |
| Determinism gate: both scalar tiers (float / Q16.16) must render byte-identical frames | All gameplay timers are **integer frame counts**, velocities are integer px/s via `s_from_int`, and camera smoothing is integer pixel math — the same idiom as the engine's zoom/parallax front end. No float literals anywhere in game code. |
| Audio: pure software mixer + SPSC command queue; per-platform device pumps (`phx_sdl/gba/psp_audio_start`); tier-0 bake resamples sounds to the GBA Direct Sound rate (18157 Hz) | Game code only ever *pushes intents* into an `AudioCommandQueue`. Entry points decide who drains it: SDL/PSP audio threads, the GBA per-frame pump (18157 Hz mixer), or the game's own render hook when headless — so gameplay code is identical everywhere. |
| Assets: offline-baked `.phxp`, zero-copy views (ROM pointer on GBA); Tiled importer, sprite clips, sounds all share one bake path | All art/audio/level data is **authored on the host** (pixel-art painters, a chiptune synthesizer, an ASCII section composer that emits real Tiled JSON) and baked through the same importers the CLI tools use. Consoles mount a ROM/EBOOT-embedded bundle; nothing is parsed at runtime. |
| Scene stack with persistent Blackboard + platform save seam | Title → Level ⇄ Pause, Level → Clear. Best-run stats persist via the save seam (file on PC/PSP, battery SRAM on GBA). |

## 2. Game design

**Fantasy:** a hatchling phoenix, small against a huge dusk sky, hops through cooling
volcanic ruins to relight the Sungate.

**Player verbs:** run, jump (variable height — release to cut), stomp. Feel polish that the
fixed-step loop makes cheap and deterministic: 4 frames of coyote time, a 6-frame jump
buffer, terminal fall speed, i-frames with sprite blink after a hit.

**Health:** 3 HP. Enemy brush / geyser / thorns = −1 HP with knock-up + i-frames. Lava = the
whole bar (instant death). At 0 HP you respawn at the last lit **waystone** with full HP;
score is kept (arcade-lite, keeps a vertical slice replayable rather than punishing).

### Enemies (three distinct behaviours)

| Enemy | Behaviour | Counter-play |
|---|---|---|
| **Cinderling** — a stubby walking coal | Gravity-bound patroller between bounds; walks off nothing (turns at patrol edge) | Stomp it (bounce + score) or jump over |
| **Ashwisp** — a floating ash puff with an ember core | No gravity; bobs vertically (and drifts on some spawns) over pits — a *timing* enemy | Stomp it mid-air or wait out its cycle |
| **Thornback** — an obsidian-spined creeper | Patroller whose spikes make **stomping hurt you** | Pure avoidance: jump over, or bait it away — the "looks similar, plays opposite" lesson |

### Hazards, collectibles, structure

- **Geyser vents**: ground fixtures cycling *idle → warn (puff) → erupt (flame column)* on
  integer frame timers, phase-offset by position so corridors form readable waves.
- **Lava pools**: pit floors; instant death, respawn at checkpoint.
- **Embers** (+1) — the coin: lines teach paths, arcs teach jumps, trails over pits price risk.
- **Sunstone shards** (+25, 3 per level) — the "did you master it?" pickups, each on the risky
  branch of a choice (high road, lava-lake apex, hidden-height jump).
- **Heart blooms** — restore 1 HP, placed as mercy after gauntlets.
- **Waystones** (2) — checkpoints; light up (animation + chime) when touched.
- **The Sungate** — the goal; touching it rolls the tally (embers + shards + HP bonus), saves
  best score, and returns to title.

## 3. Level layout — Cinder Hollow

320×20 tiles of 8 px (2560×160 px, ~11 desktop screens), authored as ten 32-tile **sections**
(ASCII grids in `src/level.h`, composed by the generator into a real Tiled `.tmj` and run
through the engine's actual importer). Reading left to right:

| # | x-range | Section | SMB-1-1 design beat |
|---|---|---|---|
| S0 | 0–31 | **The Hearth** — flat ground, ember line at head height, one cinderling walking at you | Safe start; the first enemy *is* the tutorial |
| S1 | 32–63 | **Columns** — obsidian columns of rising height (2/3/3 tiles), embers on top, cinderlings between | "Pipes": staircase of jump heights |
| S2 | 64–95 | **First Blood** — a 3-wide lava pit with an ember arc, block cluster with a hidden-height shard, cinderling pair | First real failure state, first hidden reward |
| S3 | 96–127 | **Waystone I** — checkpoint, then an ashwisp bobbing over a 2-wide pit and a 4-wide pit | Checkpoint before the difficulty step; flyer intro |
| S4 | 128–159 | **Geyser Alley** — three phase-offset geysers, ember bait between them, thornback at the exit, block stair up | Timing corridor; the "don't stomp this one" lesson |
| S5 | 160–191 | **The Split** — low road (lava pools + thornback + geyser, faster) vs high road (floating platforms, ember trail, shard) | Risk/reward branch |
| S6 | 192–223 | **The Lava Lake** — 20-wide lava lake crossed on column tops and a floating platform, two ashwisps, shard at the apex, heart bloom on landing | Mid-level climax gauntlet + mercy |
| S7 | 224–255 | **Waystone II + Stair Valley** — checkpoint, then ascending/descending block stairs with pits between, cinderlings marching down | The classic stair-gap rhythm |
| S8 | 256–287 | **The Sprint** — flat run with two 2-wide pits, a three-cinderling column, a wisp overhead, one last geyser | Speed section; tests everything at pace |
| S9 | 288–319 | **The Sungate** — the big staircase to a plateau, ember crown over the final jump, the Sungate, end-cap wall | Final ascent → goal |

Backdrop layers are generated (deterministically) rather than hand-drawn per cell: a banded
dusk-gradient sky with star specks, a jagged crag silhouette ridge with glowing cracks at
half camera speed, and the deco layer carrying lava pools and ruined arches.

## 4. Code map (engine/game separation)

Portable game code (**no STL, no platform headers, compiles unchanged for all four targets**):

- `src/game.h` — the game-defined `EngineCtx`, ECS components, tuning constants (all
  `s_from_int` / integer-frame), collision layers, asset hashes, `EmberwingGame` (the `Game`
  hook) and system/scene declarations.
- `src/systems.cpp` — one function per concern: `player_system` (movement, coyote/buffer,
  variable jump), `enemy_system` (three behaviours), `geyser_system`, `interaction_system`
  (Hit resolution: pickups/stomp/hurt/checkpoint/goal), `particle_system`, `camera_system`
  (facing lookahead, integer smoothing, level clamp), plus save/load through the platform seam.
- `src/scenes.cpp` — Title / Level / Pause / Clear scenes over the engine `SceneStack`, HUD
  (hearts, ember count, shard pips, profiler toggle), and the composition root that mounts the
  bundle, uploads textures, wires the mixer + command queue and pushes the title scene.

Host-only bake path (STL allowed; never ships to console):

- `src/art.h` — every texture painted per-pixel from ASCII pixel grids (tileset, hero sheet
  with 6 clips, three enemies, geyser column, collectibles, waystone, gate, particles) — real
  reviewable pixel art in source form.
- `src/audio_gen.h` — SFX synthesizer (square/noise + envelopes) and a two-section chiptune
  loop (lead/bass/noise-hat tracker at 22050 Hz) — baked as Sound assets; the tier-0 encode
  resamples them to the GBA device rate automatically.
- `src/level.h` — the ten ASCII sections, the legend, backdrop generators, and the `.tmj`
  emitter (tile character → tile index, entity character → centred Tiled object, contiguous
  lava runs merged into rect objects).
- `src/bake.h` / `bake_main.cpp` — assembles the bundle via the shared `BundleWriter`
  (`--target 0|1|2` per-tier encoding, same as `phxpack`).

Entry points: `main.cpp` (headless/production), `desktop_main.cpp` (SDL/GL window + real
audio device via the SPSC queue discipline), `gba_main.cpp` (ROM bundle, 18157 Hz mixer,
per-frame Direct Sound pump), `psp_main.cpp` (EBOOT bundle, audio thread).

Verification: `tests/emberwing_test.cpp` — a scripted deterministic playthrough of the
opening sections asserting movement, pickups, stomp kill, i-frames, checkpoint respawn after
a lava death, HUD pixels, non-silent mixed audio, and the save round-trip. Runs in `make
check` on both scalar tiers.

## 5. Performance notes (GBA first, PSP easily)

- **The GBA renders on silicon, not the CPU**: the PPU build streams the camera window of
  each layer into a 32×32 screenblock (~700 halfword writes/layer/frame), re-packs the
  visible sprites' tiles into a 1D OBJ run (~40 tiles/frame), and the PPU scans it all out.
  The CPU's frame is gameplay + the 18157 Hz audio pump — which is why the DirectSound
  stream no longer starves (the software-render ROM, kept as `make gba-emberwing`, spends
  the whole frame rasterizing and sounds like it).
- **Boot is instant**: the Q16 sine LUT is now a compile-time table in ROM — it used to be
  built at static-init through newlib's soft-double `sin()`, tens of seconds of black
  screen on the ARM7 before `main()` ran.
- **Zero allocation** on the frame path: all systems operate on pool-backed components and
  fixed arrays (`Hit hits[64]`, 16 particle slots, fixed spawn tables).
- **Tilemap cost is flat**: the 4 layers are retained uploads; level width is free (indices
  are zero-copy from ROM on GBA: 320·20·4·2 B ≈ 50 KB of cartridge, ~0 B of EWRAM).
- **Sprite budget**: worst case on screen ≈ 20 (enemies + pickups + geysers + HUD + a burst
  of particles) against the 128-OBJ ceiling; HUD text is the biggest consumer.
- **Audio on GBA**: the mixer runs at the 18157 Hz vblank-locked device rate (304
  samples/frame — see `tools/phxsnd/instructions.md`) and the bundle's sounds are baked *to
  that rate*, so the ARM7 mixes 1:1 with no resampling; the music is softened at bake time
  (3-tap lowpass) so the 22050→18157 Hz encode doesn't alias square-wave harmonics into fizz
  on the 8-bit DAC.
- **Fixed-point tier**: gameplay math is integer/`scalar` idiom throughout, so the GBA build
  (no FPU) runs the same code the determinism gate verifies on the host.
