# Phosphorus Engine — Entity Component System

> `engine/ecs/` — data-oriented game state. Tuned so the *same* ECS scales from 512
> entities in 256 KB (GBA) to 65,536 entities on PC, with no per-entity heap churn.

## 1. Model choice: sparse-set ECS (not archetypes)

Two mainstream designs:

| Design          | Iteration speed | Add/remove cost | Memory overhead | GBA fit |
|-----------------|-----------------|-----------------|-----------------|---------|
| **Archetype** (EnTITT/Unity DOTS) | excellent (contiguous) | expensive (moves rows between tables) | high (many tables) | poor |
| **Sparse set** (EnTT-classic) | very good (dense arrays) | cheap (swap-remove) | low, predictable | **good** |

Phosphorus uses **sparse sets**. Rationale for our targets:

- Predictable, low, *bounded* memory — each component type is one fixed pool sized
  from `phx_caps::max_entities`. No dynamic table proliferation. This is what makes it
  fit a static GBA budget.
- Add/remove is O(1) swap-remove — gameplay (spawning bullets/enemies) is add/remove
  heavy.
- Iteration is over dense contiguous arrays → cache-friendly on PSP, IWRAM-resident
  hot components on GBA.
- Simpler to read and audit (pillar #3) than an archetype mover.

## 2. Core types

```cpp
namespace phx::ecs {

using Entity = uint32_t;                         // index:24 | generation:8
constexpr Entity kInvalid = 0xFFFFFFFF;

inline uint32_t index_of(Entity e)  { return e & 0x00FFFFFF; }
inline uint8_t  gen_of(Entity e)    { return e >> 24; }
// generation guards against stale handles: despawn bumps the slot's generation,
// so a dangling Entity fails is_alive() instead of aliasing a recycled slot.

class World {
public:
    static Result<World*> create(ArenaAllocator&, uint32_t max_entities);

    Entity spawn();
    void   despawn(Entity);
    bool   is_alive(Entity) const;

    template <class C> C&   add(Entity, const C& = {});   // O(1)
    template <class C> void remove(Entity);               // O(1) swap-remove
    template <class C> C*   get(Entity);                  // nullptr if absent
    template <class C> bool has(Entity) const;

    template <class... Cs, class Fn> void each(Fn&&);     // iterate matching set
    uint32_t count() const;
};
} // namespace phx::ecs
```

## 3. Component storage: `SparseSet<C>`

```cpp
template <class C>
class SparseSet {
    uint32_t* sparse_;   // entity index -> dense index   (size = max_entities)
    Entity*   dense_;     // dense index   -> entity        (packed)
    C*        comps_;     // dense index   -> component     (packed, parallel to dense_)
    uint32_t  size_ = 0;
public:
    void init(ArenaAllocator& a, uint32_t max) {           // one-shot, boot-time
        sparse_ = a.alloc_array<uint32_t>(max);
        dense_  = a.alloc_array<Entity>(max);
        comps_  = a.alloc_array<C>(max);
        for (uint32_t i=0;i<max;i++) sparse_[i] = kTombstone;
    }
    C& add(Entity e, const C& v) {
        uint32_t i = index_of(e);
        sparse_[i] = size_; dense_[size_] = e; comps_[size_] = v;
        return comps_[size_++];
    }
    void remove(Entity e) {                                // swap-remove, O(1)
        uint32_t d = sparse_[index_of(e)];
        uint32_t last = --size_;
        dense_[d] = dense_[last]; comps_[d] = comps_[last];
        sparse_[index_of(dense_[d])] = d;
        sparse_[index_of(e)] = kTombstone;
    }
    bool has(Entity e) const { return sparse_[index_of(e)] != kTombstone; }
    C&   get(Entity e)       { return comps_[sparse_[index_of(e)]]; }
    // dense iteration:
    C*      data()  { return comps_; }
    Entity* ents()  { return dense_; }
    uint32_t size() const { return size_; }
};
```

```
 sparse (by entity)         dense (packed)        comps (packed, parallel)
 ┌───┬───┬───┬───┐          ┌────┬────┬────┐       ┌────┬────┬────┐
 │ 2 │ . │ 0 │ 1 │          │ e2 │ e3 │ e1 │       │ C  │ C  │ C  │
 └───┴───┴───┴───┘          └────┴────┴────┘       └────┴────┴────┘
   iterate comps[0..size) linearly → cache-friendly; entity known via dense[]
```

All three arrays are carved **once** from an arena at `World::create`. Zero runtime
allocation; footprint = `Σ_types max_entities × (4 + 4 + sizeof(C))`.

## 4. Iteration & systems

```cpp
// World::each picks the smallest set among Cs, iterates it, probes the rest.
world.each<Position, Velocity>([&](Entity e, Position& p, Velocity& v){
    p.value += v.value * dt;          // contiguous Position/Velocity, hot loop
});
```

```cpp
struct System {                       // systems are plain functions/objects
    virtual void tick(World&, scalar dt) = 0;   // PC/PSP
};
// On GBA, systems are free functions registered in a fixed array (no vtable),
// selected by PHX_NO_VIRTUAL; same call sites via a macro shim.
```

Systems are ordered explicitly in `App` (sim systems list). Determinism comes from
fixed order + fixed timestep. Example gameplay system order:

```
InputSystem → AISystem → PhysicsIntegrate → CollisionResolve → AnimationSystem →
CameraSystem → (render reads world)
```

## 5. Worked example (the example platformer uses exactly this)

```cpp
// components — pure data, no logic
struct Position { vec2 value; };
struct Velocity { vec2 value; };
struct SpriteRef{ TextureId tex; int16_t sx,sy,sw,sh; uint8_t z; };
struct AABBColl  { vec2 half; uint16_t mask; };
struct PlayerTag {};

// system — logic, no data ownership
struct PhysicsIntegrate : System {
    void tick(World& w, scalar dt) override {
        w.each<Position, Velocity>([&](Entity, Position& p, Velocity& v){
            v.value.y += kGravity * dt;          // scalar == fixed16 on GBA
            p.value   += v.value * dt;
        });
    }
};

// spawn a player
Entity make_player(World& w, vec2 at, TextureId tex) {
    Entity e = w.spawn();
    w.add<Position>(e, {at});
    w.add<Velocity>(e, {});
    w.add<SpriteRef>(e, {tex, 0,0,16,16, /*z*/10});
    w.add<AABBColl>(e, {{8,8}, kMaskPlayer});
    w.add<PlayerTag>(e);
    return e;
}
```

The render system simply iterates `SpriteRef`+`Position` and emits `DrawSprite`s — the
ECS never knows about the renderer, the renderer never knows about the ECS. They meet
in one small system.

## 6. Advantages & disadvantages (honest accounting)

**Advantages**
- Cache-friendly linear iteration → the performance win that matters on PSP/GBA.
- O(1) add/remove/has/get; bounded, statically sized memory.
- Decoupling: data (components) vs. behavior (systems) → easy to test, reorder, port.
- Trivial serialization: dense arrays dump/load as blobs (save system, networking).
- No deep inheritance trees; composition over hierarchy.

**Disadvantages**
- Less intuitive than OOP entities for newcomers (data here, logic there).
- Cross-component queries with many component types can need set-intersection care.
- Sparse arrays cost `max_entities × 4` per type even when few entities have it —
  acceptable because our ceilings are known and small.
- No structural change *during* iteration without deferral (we provide a command
  buffer: `world.defer().despawn(e)`, auto-flushed by `App::run` once per fixed step
  right after `Game::on_fixed_update` returns — see `engine/runtime/src/app.cpp`. A
  headless caller that drives a `World` without `App` must call `flush_deferred()`
  itself, as `tests/unit/test_ecs.cpp` does).
- Relationships/hierarchy (parent-child) are not native — modeled as a `Parent`
  component, resolved by a system.

## 7. Determinism & save support

Because storage is dense POD arrays and systems run in fixed order at a fixed step,
the entire `World` serializes by writing each `SparseSet`'s `{size, dense[], comps[]}`.
The example game's save system (`docs/09` MVP) does exactly this into a `.phxsave`
blob — see `examples/platformer`.

## Testing (`tests/ecs/`)

Generation/handle-recycling correctness (stale handle rejected), swap-remove
invariants, multi-component `each` intersection, deferred structural changes, and a
fuzz test that spawns/despawns 1M times asserting no leak and stable footprint.
