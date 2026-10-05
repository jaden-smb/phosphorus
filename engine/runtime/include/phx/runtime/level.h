// phx/runtime/level.h — a level as authored in Phosphorus Studio, running: the tilemap (drawn, with
// its parallax), its collision (the physics grid from the LAST tile layer, with the per-tile
// flags) and its spawns turned into entities, each one built from the PREFAB TABLE row whose
// `type` column names it. What a game used to write by hand, per game (examples/platformer's
// LevelScene), is one call:
//
//     level.load(app, *res, &physics, LevelOptions{ "level"_hash });   // on_start / scene enter
//     physics.step(app.world(), dt, hits);  anim.tick(app.world(), dt); // on_fixed_update
//     level.draw(app.render());  draw_sprites(app.world(), app.render()); // on_render
//
// The prefab table ("prefabs" by default; a phxbin table, read by column name — TableView) says
// what each spawn type is made of. Every column is optional:
//
//   | column   | type | makes                                                                 |
//   |----------|------|-----------------------------------------------------------------------|
//   | type     | str  | the spawn type this row builds (what the map editor places)           |
//   | sprite   | str  | a SpriteRenderer + Animator from that sprite asset (or a texture)      |
//   | clip     | str  | the clip it starts in (default "idle", else the first)                 |
//   |          |      | (the Animator also gets the sprite's transitions: anim_trigger())     |
//   | w, h     | int  | collider size in px (default: the sprite's frame, else the spawn's)    |
//   | collide  | int  | 0 = no collider (default 1)                                            |
//   | layer    | int  | collision layer bits (default 1); `mask` the layers it reports (0xFFFF)|
//   | body     | int  | 1 = a dynamic Body: gravity + tile collision (default 0: static)       |
//   | z        | int  | draw layer (default 10)                                                |
//
// Every spawn becomes an entity with a Transform (the spawn's centre) and a PrefabRef (its type
// and prefab row), prefab or not, so game code finds "the player" or "every coin" by type, or one
// spawn by the name it was given in the map editor (find_named). LevelOptions::on_spawn lets a
// game add its own components as each entity is made.
//
// A placed spawn can carry its own PROPERTIES (set in the map editor's spawn inspector; Tiled
// custom properties). A property with a prefab column's name OVERRIDES that column for this one
// entity (a bigger coin: `w` = 12), and any other is the instance's own data (a door's `target`,
// an enemy's `range`). Game code reads a setting with the same inheritance the loader uses:
//
//     const int32_t range = level.get_int(ref, "range"_hash, 24);   // the spawn's, else the
//                                                                   // prefab's, else 24
//
// A prefab's `components` column ("Enemy Coin") attaches REFLECTED components (phx/ecs/reflect.h:
// a game's own components, declared with PHX_COMPONENT). Each field is filled from the column
// `Enemy_range` (or the spawn's `Enemy_range` property), else keeps its C++ default; a scalar field
// takes a decimal, a hash field (PHX_FIELD_HASH) the text it is the hash of.
#ifndef PHX_RUNTIME_LEVEL_H
#define PHX_RUNTIME_LEVEL_H

#include "phx/anim/anim.h"
#include "phx/ecs/reflect.h"
#include "phx/ecs/world.h"
#include "phx/physics/physics.h"
#include "phx/render/renderer.h"
#include "phx/resource/cache.h"
#include "phx/resource/table.h"

namespace phx {

class App;

// Draws an entity: a region of a texture at its Transform (centred). With an Animator on the
// same entity, the region follows the animation.
struct SpriteRenderer {
    TextureId tex   = kNoTexture;
    int16_t   sx = 0, sy = 0, sw = 0, sh = 0;
    uint8_t   layer = 10;
    uint16_t  flags = 0;      // SpriteFlags (kFlipX to face left)
    // The sprite's clip names, in Animator clip order (play_clip, phx/runtime/behaviours.h).
    const NameHash* clip_names = nullptr;
    uint16_t        clip_count = 0;
};

// Which spawn made an entity, and from which prefab row (-1: the type has no prefab).
struct PrefabRef {
    NameHash type  = 0;
    int32_t  row   = -1;
    uint16_t spawn = 0;       // index in the level's spawn table
};

struct LevelOptions {
    NameHash map     = "level"_hash;     // the tilemap asset (its spawns come with it)
    NameHash prefabs = "prefabs"_hash;   // the prefab table (optional)
    NameHash tileset = 0;                // 0 = the tileset the map names
    // Called for every entity the level makes, after its prefab components are added.
    void (*on_spawn)(void* user, ecs::World&, ecs::Entity, const SpawnDef&, const TableView& prefabs,
                     int32_t row) = nullptr;
    void* user = nullptr;
};

class Level {
public:
    static constexpr uint32_t kMaxSprites = 16;   // distinct sprite assets per level
    static constexpr uint32_t kMaxClips   = 12;   // clips kept per sprite

    // Upload the map, set `physics`' collision grid (if given), spawn every entity into
    // app.world(). Ok, or why not (NotFound: no such map in the mounted bundles).
    Status load(App& app, ResourceCache& res, PhysicsWorld* physics, const LevelOptions& opt = {});
    // Despawn this level's entities and free its sprite textures. The map stays uploaded (its
    // tilemap slot and tileset), so loading the same map again with this Level reuses it.
    void unload(App& app);

    void draw(Renderer& r) const;            // every tile layer, backdrops first

    bool      loaded()    const { return map_ != kNoTilemap; }
    TilemapId map()       const { return map_; }
    const TilemapView& tilemap() const { return view_; }
    int32_t   width_px()  const { return int32_t(view_.width) * view_.tile_w; }
    int32_t   height_px() const { return int32_t(view_.height) * view_.tile_h; }
    const TileGrid&   grid()    const { return grid_; }
    const TableView&  prefabs() const { return prefabs_; }
    const SpawnsView& spawns()  const { return spawns_; }
    uint32_t  spawned()   const { return spawned_; }
    // The first entity spawned as `type` ("player"_hash), else ecs::kInvalid.
    ecs::Entity find(ecs::World& w, NameHash type) const;
    // The entity whose spawn was named `name` in the map editor ("door_a"_hash), else kInvalid.
    ecs::Entity find_named(ecs::World& w, NameHash name) const;
    // The level loaded last (nullptr after its unload): what the developer overlay names
    // entities from (phx/runtime/devtools.h). Header-inline: reading it links no level code.
    static const Level* active();

    // A setting of the entity `ref` made: its spawn's property `key`, else its prefab row's
    // column `key`, else `def`. The same lookup the loader uses for the engine's columns.
    int32_t     get_int(const PrefabRef& ref, NameHash key, int32_t def = 0) const;
    bool        get_q16(const PrefabRef& ref, NameHash key, int32_t& out) const;   // decimal, Q16.16
    const char* get_str(const PrefabRef& ref, NameHash key) const;       // nullptr when absent
    NameHash    get_hash(const PrefabRef& ref, NameHash key) const;      // 0 when absent / empty
    NameHash    name(const PrefabRef& ref) const { return spawns_.name(ref.spawn); }

private:
    struct SpriteSlot {
        NameHash    name = 0;
        TextureId   tex  = kNoTexture;
        SpriteSheet sheet{};
        AnimClip    clips[kMaxClips]{};
        NameHash    clip_names[kMaxClips]{};
        uint16_t    clip_count = 0;
        Span<const AnimEdge> edges;        // the sprite's transitions, in the bundle (zero-copy)
        int16_t     w = 0, h = 0;          // one frame (or the whole texture)
    };
    const SpriteSlot* sprite(ResourceCache& res, Renderer& r, NameHash name);
    void attach_components(ecs::World& w, ecs::Entity e, const PrefabRef& ref, const char* list);

    // Maps this Level has uploaded: loading one again reuses its tilemap slot and tileset texture
    // (renderer tilemap slots are never freed), so a game flow can restart and revisit levels.
    static constexpr uint32_t kMapCache = 8;
    struct MapCache { NameHash map = 0; TilemapId id = kNoTilemap; TextureId tileset = kNoTexture; };
    MapCache    cache_[kMapCache]{};
    uint32_t    cache_n_ = 0;

    TilemapView view_{};
    TilemapId   map_ = kNoTilemap;
    TextureId   tileset_ = kNoTexture;
    bool        tileset_cached_ = false;
    TileGrid    grid_{};
    TableView   prefabs_{};
    SpawnsView  spawns_{};
    SpriteSlot  sprites_[kMaxSprites]{};
    uint32_t    sprite_count_ = 0;
    uint32_t    spawned_ = 0;
};

// Draw every (SpriteRenderer, Transform) entity, following its Animator when it has one.
void draw_sprites(ecs::World& w, Renderer& r);

namespace level_detail { inline const Level* g_active = nullptr; }   // set by load, cleared by unload
inline const Level* Level::active() { return level_detail::g_active; }

} // namespace phx
#endif // PHX_RUNTIME_LEVEL_H
