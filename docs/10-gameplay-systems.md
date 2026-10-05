# Phosphorus Engine — Gameplay Systems

> Input · Audio · Scene · Physics · Animation · UI. These sit above ECS/render/resource
> and below the game. Each is small, decoupled, and meets the others only through ECS
> data or explicit handles — never through direct includes of one another.

---

## 1. Input (`engine/input/`)

Turns the platform's `phx_input_raw` snapshot into semantic, frame-stable state.

```cpp
namespace phx {
enum class Button : uint8_t {            // canonical order — same meaning everywhere
    Up, Down, Left, Right, A, B, X, Y, L, R, Start, Select, Count
};
struct InputMap {                         // remappable controls (POD, save-file friendly)
    uint8_t physical[12];                 // logical Button <- physical bit (identity default)
    uint8_t stick_to_dpad;                // left stick also drives Up/Down/Left/Right
    int16_t stick_deadzone;               // RAW axis units -> integer-deterministic
    void remap(Button logical, Button physical);
    void reset();
};
struct InputState {
    uint32_t held, pressed, released;     // bitmasks over LOGICAL Button (after `map`)
    uint32_t raw_held, raw_pressed;       // PHYSICAL bits (before `map`) — rebind capture
    vec2     lstick, rstick;              // [-1,1], zero on GBA
    vec2     pointer; bool pointer_down;  // mouse/touch; off on GBA
    InputMap map;                         // the active remap (App::input_map() mutates it)

    bool down(Button b)     const { return held    & (1u<<int(b)); }
    bool just(Button b)     const { return pressed & (1u<<int(b)); }   // edge
    bool up(Button b)       const { return released& (1u<<int(b)); }
    void update(const phx_input_raw&);    // remap -> stick synthesis -> edges vs last frame
};
} // namespace phx
```

Each platform backend fills `phx_input_raw` in the fixed canonical order:

| `Button` | GBA key      | PSP        | PC keyboard | PC gamepad (SDL) |
|----------|--------------|------------|-------------|------------------|
| A        | A            | Cross      | Z / Space   | A (south)        |
| B        | B            | Circle     | X           | B (east)         |
| Start    | Start        | Start      | Enter       | Start            |
| Up..Right| D-pad        | D-pad      | Arrows/WASD | D-pad + stick    |

The SDL backend supports **hot-plugged game controllers** (keyboard and pad are both always
live, OR'd together; sticks land in `axis[0..3]`). Above the seam, **remapping is engine-side
and platform-agnostic**: `InputState::update()` routes each physical bit through `InputMap`
before edge detection, and synthesizes dpad bits from the left stick using an integer
deadzone on the raw axis (tier-exact — no float in the path). A game's options scene rebinds
via `App::input_map()` and can persist the POD `InputMap` through the platform save seam.
For a "press any button to bind" capture, read `raw_pressed` — the pre-remap physical edge
mask that exists exactly for this (logical edges are already remapped, so they can't tell
you which physical button the player just hit). The platformer example ships a working
options scene (`examples/platformer/src/systems.cpp`, `OptionsScene`): pause → OPTIONS →
rebind JUMP/PAUSE, cycle the stick deadzone, reset — persisted in its SaveData (v2) and
proven end-to-end by scripted runs 4/5 of the platformer suite, including one gotcha worth
copying around: an options overlay must **not** set `render_below` over a menu with focus
buttons, because immediate-mode confirms happen inside `render()` and the menu underneath
would eat the same press.

Edge detection (`pressed`/`released`) is computed against last frame, so gameplay reads
"jump pressed this frame" identically on every device. Input is sampled once per frame
*before* the fixed-step sim loop, so all sub-steps see a consistent snapshot (determinism).

---

## 2. Audio (`engine/audio/`)

A small software mixer feeding the platform audio device. One path for all targets;
voice count scales with `phx_caps::audio_channels`.

```cpp
namespace phx {
class AudioMixer {
public:
    static Result<AudioMixer*> create(ArenaAllocator&, const Caps&, uint32_t out_rate = 44100);
    VoiceId play_sfx(const SoundView&, float vol=1, float pan=0, bool loop=false);
    void    stop(VoiceId);
    void    play_music(const SoundView&, float vol=1, bool loop=true);
    void    set_music_volume(float);
    void    mix(int16_t* out, uint32_t frames);      // called by the device callback
};
} // namespace phx
```

**What a game uses: `App::audio()`** (`phx/runtime/audio.h`, `GameAudio`). The engine owns the
mixer, the lock-free `AudioCommandQueue` and the platform's device, so game code only states
intents and never calls a platform function:

```cpp
jump = to_sound(res->sound("jump"_hash).unwrap());   // on_start: a baked sound, zero-copy
app.audio().play(jump);                              // sfx (vol, pan, loop)
app.audio().play_music(theme, 0.6f);                 // the music bus
app.audio().stop_all();
```

Nothing is allocated or started until the first play. At that point the mixer and the queue
come out of the persistent arena, and the platform's device (`phx_platform::audio()`, a
`phx_audio { rate, start, stop }`) is started at the device's rate:

| Platform | Device | Rate |
|---|---|---|
| SDL | audio thread | 44.1 kHz |
| PSP | sceAudio thread | 44.1 kHz |
| GBA | DirectSound, pumped by the VBlank IRQ | 18157 Hz (what the tier-0 bake resamples to) |

The device's callback drains the queue into the mixer and mixes, so the mixer is only ever
touched there. With no device (the null platform) the App mixes one frame's worth per rendered
frame itself: voices advance deterministically, and `peak()`, `plays()` and `frames_mixed()` let
tests check what would have been heard. A game that never plays anything pays nothing. A game
that runs its own mixer and device (Emberwing, Phosphorus Studio) is unaffected, because
`GameAudio` never starts. `make game-audio-verify` checks the device path on a real SDL device.

| Feature           | GBA                          | PSP                  | PC                |
|-------------------|------------------------------|----------------------|-------------------|
| SFX               | 2 DirectSound ch + 4 PSG     | 8 voices, ADPCM      | 32 voices, PCM    |
| Music             | resident tracker module      | streamed ADPCM       | streamed OGG/PCM  |
| Streaming         | no (resident)                | yes (ring buffer)    | yes               |
| Mixer rate        | ~13–18 kHz (timer-driven DMA)| 44.1 kHz             | 44.1/48 kHz       |

The mixer is the same SoA voice loop everywhere; the GBA build compiles a fixed-point,
2-voice specialization (PSG handled separately for music). Compressed formats are
decoded in `mix()` per chunk (ADPCM) or pre-decoded for short SFX. Music streaming uses
the `AudioStream` ring from `docs/06` §5.

---

## 3. Scene Management (`engine/scene/`)

A LIFO **scene stack** (so you can push a pause/menu over gameplay), with explicit
transitions and a persistent-object channel.

```cpp
namespace phx {
struct Scene {
    virtual void on_enter(EngineCtx*) {}
    virtual void on_exit(EngineCtx*)  {}
    virtual void on_pause(EngineCtx*) {}     // another scene pushed on top
    virtual void on_resume(EngineCtx*) {}
    virtual void update(EngineCtx*, scalar dt) {}
    virtual void render(EngineCtx*, scalar alpha) {}
};
class SceneStack {
public:
    void push(Scene*, Transition = Transition::None);
    void pop(Transition = Transition::None);
    void replace(Scene*, Transition = Transition::None);
    void update(EngineCtx*, scalar dt);      // updates top (or all, if "transparent")
    void render(EngineCtx*, scalar alpha);   // renders back-to-front for overlays
    Transition  last_transition() const;     // the Transition passed to the most recent op
    Blackboard& persistent();                // survives scene changes (save data, etc.)
};
enum class Transition { None, Fade, SlideLeft, SlideRight };
} // namespace phx
```

- Each scene owns a **scene-scoped arena** (`StackAllocator` mark on enter, reset on
  exit) → leaving a scene frees all its allocations in O(1), no leaks.
- The ECS `World` can be **per-scene** or shared; the example uses one world cleared on
  scene change, with persistent data (score, save slot) in the `Blackboard`.
- **`Transition` is a recorded hint only, not yet a rendered effect.** `push`/`pop`/
  `replace` store whatever `Transition` you pass as `last_transition()`
  (`engine/scene/src/scene.cpp`), but nothing in the engine reads it back — there is no
  timing/progress state machine and no fade-quad render step, on any backend. Both
  example games always pass the `None` default, so `Fade`/`SlideLeft`/`SlideRight` are
  presently just labeled enum values. `last_transition()` is a real seam for a game (or
  a future engine change) to drive its own fade — e.g. render a full-screen tinted quad
  keyed off `last_transition()` for a few frames after a push/pop — but that wiring does
  not exist yet.

---

## 4. Physics (`engine/physics/`)

Deliberately **minimal and tile-friendly** for the MVP, with a clear expansion seam.
No solver, no rotation — AABB + tile collision is what 2D platformers need and what the
GBA can afford.

```cpp
namespace phx {
struct AABBColl { vec2 half; uint16_t layer, mask; };   // ECS component
struct Body     { vec2 vel; bool on_ground; uint8_t flags; };

struct Hit { Entity other; vec2 normal; scalar t; bool tile; };

class PhysicsWorld {
public:
    void set_tilemap(const TilemapView&);    // collision layer from phxtile
    // swept AABB vs tilemap (per axis), then AABB-vs-AABB overlap pass:
    void step(ecs::World&, scalar dt, Span<Hit> out_hits);
    bool overlap(const aabb&, uint16_t mask, Entity ignore = ecs::kInvalid) const;
};
} // namespace phx
```

Algorithm (per fixed step):
1. Integrate velocity → tentative position.
2. **Swept tile collision per axis** (X then Y): sample the collision layer over the
   swept rect, resolve to the nearest blocking edge, set `on_ground` on a downward stop.
   Axis-separated resolution gives correct wall-slide/ground behavior cheaply.
3. **Broadphase AABB overlap** for entity-vs-entity (uniform grid binning, ceiling from
   caps) → emit `Hit`s for gameplay (enemy stomp, pickups). Overlap-only by default
   (triggers); resolution is opt-in.

**What counts as solid — two modes on `TileGrid`.** Without metadata, solid iff
`index >= solid_from` (index 0 = air; the "every non-empty tile on the gameplay layer"
MVP rule). With a per-tile **collision flags table** (authored as Tiled tileset per-tile
properties, baked into the Tilemap asset, served by `TilemapView.tile_flags`), each tile
index maps to a `TileFlags` byte:

- `kTileSolid` — blocks all movement;
- `kTileOneWay` — a one-way platform: blocks only a body moving down whose bottom edge was
  at/above the tile top before the step (jump up through it, land on it);
- `kTileHazard` — non-blocking; physics only reports it (`PhysicsWorld::tile_flags_in(aabb)`
  ORs the flags under a box — "is the player in spikes?").

Decorative non-solid tiles can therefore share the gameplay layer with walls, platforms,
and hazards. Out-of-range tiles read as solid in both modes (the map is a closed box), and
both games feed `tv.tile_flags` straight into their `TileGrid`, so authored metadata takes
effect with no game code.

```
 swept X then Y (separable):
   ┌────┐ vel                ┌────┐
   │ P  │───►   wall ▓    →   │ P ▓   stop at wall, keep Y motion
   └────┘                     └────┘
```

**Expansion seam:** `PhysicsWorld` is an interface; the MVP impl is `AABBTilePhysics`.
A future `ImpulsePhysics2D` (mass, restitution, simple manifolds) or an integration of
a slim external 2D lib can replace it without touching gameplay — they all consume the
same `AABBColl`/`Body` components and emit `Hit`s.

---

## 5. Animation (`engine/anim/`)

Sprite-sheet, frame-based, driven by a tiny state machine. Consumes the animation table
baked by `phxsprite`.

```cpp
namespace phx {
struct AnimClip { uint16_t first, count; uint8_t fps; bool loop; };
struct AnimEdge { uint8_t from, to; NameHash trigger; };   // from == kAnyClip: any clip
struct Animator {                              // ECS component
    Span<const AnimClip> clips; Span<const AnimEdge> edges; SpriteSheet sheet;
    uint16_t clip, frame; scalar timer; bool finished; /* + output rect */
    void play(uint16_t clip);
    bool trigger(NameHash trig);               // take a matching edge, if any
};
class AnimationSystem {
public:
    void tick(ecs::World&, scalar dt) const;   // advance timers, pick frame, fire kAnimDone
};
} // namespace phx
```

- `AnimationSystem` advances each `Animator`, computes the current frame, and writes the
  source rect into the entity's `SpriteRef` — so the render system stays dumb.
- The **state machine** is data (clips + edges: `trans <from|*> <to> <trigger>` lines in a
  `.sprdef`, the Studio sprite editor's Transitions list), not code: `idle ⇄ run ⇄ jump ⇄ fall`
  for the player is authored, not hardcoded. The bake resolves clip names to indices; `Level`
  gives each spawned `Animator` its sprite's edges. An edge from the current clip beats a `*`
  edge, and a `*` edge into the playing clip is a no-op, so triggers can be sent every step.
- Triggers come from `PlatformerController` (`jump`, `fall`, `land`, `move`, `stop`, `hurt`), from
  the anim system itself (`done`, when a non-looping clip ends), and from game code
  (`phx::anim_trigger(world, e, "attack"_hash)`). A sprite without edges is played by clip name.
- Frame timing uses `scalar` so fixed/float builds animate identically.

```
   ┌──────┐  move>0   ┌──────┐  vy<0   ┌──────┐
   │ idle │──────────►│ run  │────────►│ jump │
   └──────┘◄──────────└──────┘         └──┬───┘
       ▲    move==0        ▲ land           │ vy>0
       └───────────────────┴────────────────┘ fall
```

---

## 6. UI (`engine/ui/`)

An **immediate-mode**, retained-where-it-pays UI that emits `DrawSprite`s (text = font
atlas), sized for GBA/PSP limits. No widget tree allocations per frame.

```cpp
namespace phx {
class UI {
public:
    void begin(EngineCtx*, const InputState&);
    // primitives (all batch into the renderer's sprite path):
    void text(vec2, NameHash font, const char*, Color = white);
    void text_fmt(vec2, NameHash font, const char* fmt, ...);
    bool button(Rect, const char* label);     // returns true on press (menus)
    void image(Rect, TextureId, Rect src);
    void bar(Rect, scalar t, Color fg, Color bg);   // HUD health/energy
    // dialogue:
    void dialogue(const DialogueView&, int line, scalar reveal_t);  // typewriter
    void end();
};
} // namespace phx
```

Supported surfaces:
- **Menus** — focus-based navigation by D-pad/buttons (not just mouse), because
  consoles have no pointer. `button()` participates in a focus ring.
- **Text rendering** — bitmap font atlas. A `BitmapFont` is a fixed grid, optionally with a
  glyph table (`FontGlyph`: rect, offset, advance per character) that makes it proportional.
  `phx::load_font` (`phx/runtime/font.h`) builds one from a baked Font asset: a `.font` grid
  sheet whose widths are measured at bake, or an imported BMFont `.fnt`.
  `UI::text_width` / `glyph_advance` measure text, and `text()`, `button()` and the dialogue
  wrap all lay out with them. Each glyph is one sprite on every tier, proportional or not.
- **HUD** — `bar()`, `image()`, `text_fmt()` for score/health/lives; cheap, per-frame.
- **Dialogue** — `UI::dialogue()` is the box: a typewriter reveal driven by `reveal_t`,
  word-wrapped, with a portrait. `phx::DialogueRunner` (`phx/runtime/dialogue.h`) plays
  conversations authored as data: a `.dlg` baked to a Dialogue asset.
  - A conversation is a list of lines (speaker, text, `next`) and choices.
  - `if` conditions skip a line or hide a choice. `do` effects set, add to or subtract from
    variables. The game flow's counter totals are the variables, so `coins` is the coins
    collected.
  - A shows the rest of a line, then goes on. Up/Down and A pick a choice.
  - The state is integer, so the same conversation reveals the same character on the same tick
    on every tier.
  - The flow plays conversations on "talk" screens (cutscenes) and when the player presses Up at
    a `Talk` component. Phosphorus Studio's dialogue editor plays them the same way; the dialogue
    suite checks its simulator against the runner.

GBA constraints baked in: glyphs are 8×8 tiles drawn as BG/OBJ; the UI batches into the
same ≤128 OBJ budget and warns (via `RenderStats`) if a HUD-heavy frame would overflow.

---

## 7. How they compose (the example's per-frame flow)

```
 input.poll ─► [sim step] InputSystem → AISystem → PhysicsWorld.step →
               (Hit handling: stomp/pickup) → AnimationSystem → CameraSystem
           ─► SceneStack.update ─► audio events queued
 render: SceneStack.render → world→DrawSprite (sprites/tilemap) → UI.* (HUD/menu)
         → Renderer.end_frame → platform.present
 audio:  platform callback → AudioMixer.mix
```

Every arrow is data or a handle — no system includes another system's header. That is
the decoupling the architecture exists to guarantee.
