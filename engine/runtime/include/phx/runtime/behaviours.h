// phx/runtime/behaviours.h — STOCK BEHAVIOURS: the gameplay most 2D games start from, as engine
// components a prefab attaches by name (its `components` column, phx/runtime/level.h) and tunes
// with columns or spawn properties (`PlatformerController_jump` = 240). No game code:
//
//   | component            | what it does                                                        |
//   |----------------------|---------------------------------------------------------------------|
//   | PlatformerController | run (Left/Right) and jump (A) on its Body; walk/idle/jump clips by  |
//   |                      | name (or events into the sprite's transitions, below); a jump      |
//   |                      | sound; hazard TILES send it back to the respawn point              |
//   | Patrol               | walk back and forth `range` px around where it spawned, turning at  |
//   |                      | walls; needs a Body                                                 |
//   | Pickup               | touched by the player: add `value` to the `counter` counter, play   |
//   |                      | `sound`, disappear                                                  |
//   | Hazard               | touched by the player: back to the respawn point ("deaths" += 1)    |
//   | Checkpoint           | touched by the player: becomes the respawn point                    |
//   | Exit                 | touched by the player: exit() reports its `target` (a level name)   |
//   | CameraFollow         | the camera centres on it, kept inside the level                     |
//   | Talk                 | touched by the player + Up (or at once, auto_start): talk() reports |
//   |                      | its `conversation` (the game flow plays it: phx/runtime/dialogue.h)  |
//
// "The player" is the first PlatformerController. Collisions come from the entities' colliders
// (the prefab's layer/mask: give the player a mask that includes the others' layers).
//
//     behaviours.start(app, level, *res, physics);          // after level.load
//     behaviours.update(app, dt);                            // on_fixed_update: control -> patrol
//                                                            //   -> physics -> contacts -> anim
//     r.begin_frame(behaviours.camera());                   // on_render
//
// ANIMATION. A sprite without transitions is driven by clip name (idle_clip/walk_clip/jump_clip).
// A sprite WITH transitions (`trans` lines in its .sprdef; the sprite editor's Transitions list)
// is a state machine, and the controller only sends it events, every step:
//
//   | trigger | when                                        | e.g. in hero.sprdef          |
//   |---------|---------------------------------------------|------------------------------|
//   | jump    | the step it jumps                           | trans * jump jump            |
//   | fall    | in the air, moving down                     | trans jump fall fall         |
//   | land    | the step it touches ground again            | trans * land land            |
//   | move    | on the ground, running                      | trans idle walk move         |
//   | stop    | on the ground, standing                     | trans walk idle stop         |
//   | hurt    | sent back to the respawn point (respawn())  | trans * hurt hurt            |
//   | done    | a non-looping clip ended (the anim system)  | trans land idle done         |
//
// A game fires its own the same way: anim_trigger(world, e, "attack"_hash).
//
// A game adds its own rules around update(): hits() are this step's contacts, counter("coins")
// the tallies, exit() the level exit reached. Everything is fixed-step and scalar-typed, so it
// runs identically on the float and fixed-point tiers.
#ifndef PHX_RUNTIME_BEHAVIOURS_H
#define PHX_RUNTIME_BEHAVIOURS_H

#include "phx/runtime/level.h"

namespace phx {

class App;

struct PlatformerController {
    scalar   speed = s_from_int(70);       // px/s
    scalar   jump  = s_from_int(200);      // take-off speed, px/s
    NameHash jump_sound = "jump"_hash;     // a sound asset (0: silent)
    NameHash idle_clip  = "idle"_hash;     // clips by name (a missing one is skipped)
    NameHash walk_clip  = "walk"_hash;
    NameHash jump_clip  = "jump"_hash;
    // state (not authored)
    bool     airborne = false;             // in the air last step (for the `land` trigger)
};

struct Patrol {
    scalar  speed = s_from_int(30);        // px/s
    int16_t range = 32;                    // px either side of where it spawned
    // state (not authored)
    scalar  home_x{};
    int8_t  dir   = 1;
    bool    homed = false;
};

struct Pickup {
    int16_t  value   = 1;
    NameHash counter = "coins"_hash;
    NameHash sound   = 0;                  // a sound asset (0: silent)
};

struct Hazard     { NameHash sound = 0; };  // played when the player is sent back
struct Checkpoint { NameHash sound = 0; };
struct Exit       { NameHash target = 0; }; // the level to go to (the game loads it)
struct CameraFollow { int16_t offset_y = 0; };
struct Talk {
    NameHash conversation = 0;             // a conversation in the game's dialogue (.dlg)
    bool     auto_start   = false;         // start on the first touch, without Up (then as usual)
};

class Behaviours {
public:
    static constexpr uint32_t kMaxHits     = 64;
    static constexpr uint32_t kMaxCounters = 8;

    // Bind to a loaded level (call after Level::load). The respawn point starts where the player
    // spawned. `physics` is stepped by update().
    void start(App& app, Level& level, ResourceCache& res, PhysicsWorld& physics);
    // One fixed step: controllers, patrols, the physics step, contacts, hazard tiles, animation,
    // the camera.
    void update(App& app, scalar dt);

    ecs::Entity player() const { return player_; }
    Camera2D    camera() const { return camera_; }
    vec2        respawn_point() const { return respawn_; }
    void        set_respawn_point(vec2 p) { respawn_ = p; }
    // A counter ("coins"_hash, "deaths"_hash, or any Pickup's), 0 when it never counted.
    int32_t     counter(NameHash name) const;
    void        set_counter(NameHash name, int32_t v);
    // Every counter slot (i < kMaxCounters): its name (0 = unused) and value.
    NameHash    counter_name(uint32_t i) const { return i < kMaxCounters ? counter_names_[i] : 0; }
    int32_t     counter_value(uint32_t i) const { return i < kMaxCounters ? counters_[i] : 0; }
    // The target of the Exit the player touched (0: none yet). clear_exit() after acting on it.
    NameHash    exit() const { return exit_; }
    void        clear_exit() { exit_ = 0; }
    // The conversation of the Talk the player asked to hear (0: none). clear_talk() after starting
    // it. can_talk(): the player is touching a Talk this step (a game shows "Up: talk").
    NameHash    talk() const { return talk_; }
    void        clear_talk() { talk_ = 0; }
    bool        can_talk() const { return can_talk_; }
    // This step's contacts (from the physics step), for the game's own rules.
    Span<const Hit> hits() const { return Span<const Hit>{ hits_, hit_count_ }; }
    // Send the player back to the respawn point (what a Hazard does).
    void        respawn(App& app);

private:
    void control(App& app, ecs::World& w);
    void patrol(ecs::World& w, scalar dt);
    void contacts(App& app, ecs::World& w);
    void follow(App& app, ecs::World& w);
    void play(App& app, NameHash sound);
    int32_t& slot(NameHash name);

    Level*          level_   = nullptr;
    ResourceCache*  res_     = nullptr;
    PhysicsWorld*   physics_ = nullptr;
    AnimationSystem anim_;
    ecs::Entity     player_  = ecs::kInvalid;
    vec2            respawn_{};
    Camera2D        camera_{};
    NameHash        exit_ = 0;
    NameHash        talk_ = 0;
    bool            can_talk_ = false;
    Hit             hits_[kMaxHits];
    uint32_t        hit_count_ = 0;
    NameHash        counter_names_[kMaxCounters]{};
    int32_t         counters_[kMaxCounters]{};
    int32_t         scratch_ = 0;
};

// Play a clip of an entity's sprite by name (the level keeps each sprite's clip names). False when
// the entity has no such clip; playing the clip it is already in does not restart it.
bool play_clip(ecs::World& w, ecs::Entity e, NameHash clip);

// Send an animation trigger to an entity's sprite state machine (Animator::trigger): "attack"_hash,
// "hurt"_hash. False when no transition fired (no Animator, or no edge for it from this clip).
bool anim_trigger(ecs::World& w, ecs::Entity e, NameHash trigger);

// "Play from here": the next Behaviours::start puts the player (and its respawn point) at (x, y)
// instead of its spawn, once. The desktop entry sets it from PHX_PLAY_FROM="x,y", which Phosphorus
// Studio's map editor passes when you play from a spot on the map.
void set_start_override(int32_t x, int32_t y);

} // namespace phx
#endif // PHX_RUNTIME_BEHAVIOURS_H
