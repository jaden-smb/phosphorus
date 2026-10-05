# Phosphorus Engine — Memory Management

> `engine/memory/` — the bedrock. Every other module allocates *through* these and
> never through `malloc`/`new` on the hot path. Primary goals: **zero runtime
> fragmentation, zero hidden allocations, deterministic footprint.**

## 0. The mandate

```
                         ┌──────────────────────────────┐
   ONE OS allocation ──▶ │         ROOT ARENA           │  (or static buffer on GBA)
   at boot only          └───────────────┬──────────────┘
                                          │ carved once, at init
   ┌───────────────┬─────────────────┬────┴─────────┬──────────────────────┐
   ▼               ▼                 ▼              ▼                       ▼
┌─────────┐  ┌──────────────┐  ┌──────────┐  ┌──────────────┐   ┌────────────────────┐
│persistent│  │resource cache│  │ ecs pools│  │ scene stack  │   │ frame stack A | B  │
│  arena   │  │ (LRU arena)  │  │ (typed)  │  │ (LIFO push/  │   │ (double-buffered,  │
│lifetime= │  │              │  │          │  │  pop scenes) │   │  reset every frame)│
│  app     │  │              │  │          │  │              │   │                    │
└─────────┘  └──────────────┘  └──────────┘  └──────────────┘   └────────────────────┘
```

After boot, **engine systems never touch the OS heap again** (host-only tools may).
Two honest caveats to the diagram above, as built:

- The **platform seam is outside the arena**: backends `malloc` init-time state (the
  software framebuffer at `init()`, desktop file/bundle images at mount) — load/init
  time only, never per-frame. `phx_platform_desc.root_arena` is the planned hook to
  bring this inside the arena; today it is passed as null and no backend reads it.
- On GBA the root arena is the same single boot `malloc` (newlib heap in EWRAM) as
  everywhere else — the static-EWRAM-buffer variant mentioned in `memory_root.cpp`'s
  header is not yet built.

## 1. Allocator catalog

| Allocator        | Layout            | Alloc | Free       | Fragmentation | Typical owner                |
|------------------|-------------------|-------|------------|---------------|------------------------------|
| `ArenaAllocator` | bump pointer      | O(1)  | whole only | none          | boot-time / per-scene        |
| `StackAllocator` | bump + marker     | O(1)  | LIFO/marker| none          | per-frame scratch, scoped    |
| `PoolAllocator`  | free-list, N×size | O(1)  | O(1) any   | none (fixed)  | ECS nodes, fixed records     |
| `ObjectPool<T>`  | typed pool + ctor | O(1)  | O(1) any   | none          | bullets, particles, entities |

All four share one invariant: **no per-allocation header walking, no coalescing, no
search.** That is what buys determinism.

## 2. ArenaAllocator — linear bump

```cpp
class ArenaAllocator {
    uint8_t* base_; size_t cap_; size_t off_ = 0;
public:
    void  init(void* mem, size_t cap)        { base_=(uint8_t*)mem; cap_=cap; off_=0; }
    void* alloc(size_t n, size_t align = 16) {
        size_t p = align_up(off_, align);
        if (p + n > cap_) return nullptr;          // caller checks; boot-time fatal
        off_ = p + n;
        return base_ + p;
    }
    template <class T, class... A> T* make(A&&... a) {
        void* m = alloc(sizeof(T), alignof(T));
        return m ? new (m) T(static_cast<A&&>(a)...) : nullptr;  // placement new only
    }
    size_t mark() const { return off_; }
    void   reset_to(size_t m) { off_ = m; }        // bulk free back to a mark
    void   reset() { off_ = 0; }
};
```

```
 alloc(A) alloc(B) alloc(C)            reset_to(markAfterA)
 ┌──┬───┬─────┬──────────┐    ───▶     ┌──┬───────────────────┐
 │A │ B │  C  │  free    │             │A │      free          │
 └──┴───┴─────┴──────────┘             └──┴───────────────────┘
   off ─────────────▲                    off ─▲
```

**Use:** anything whose lifetime is a clean nesting — the whole engine boot, a scene's
static data (freed on scene exit via `reset_to`).

## 3. StackAllocator — scoped LIFO scratch

A specialization of arena with an RAII scope guard. This is the **per-frame transient
allocator**.

```cpp
class StackAllocator : public ArenaAllocator {
public:
    struct Scope {                          // RAII: frees everything alloced within
        StackAllocator* s; size_t m;
        explicit Scope(StackAllocator* s) : s(s), m(s->mark()) {}
        ~Scope() { s->reset_to(m); }
    };
};
#define PHX_SCRATCH(allocPtr) phx::StackAllocator::Scope PHX_CONCAT(_sc,__LINE__)(allocPtr)
```

```cpp
void build_render_list(EngineCtx* ctx, World& w) {
    PHX_SCRATCH(&ctx->mem->frame_stack());      // auto-reset at scope end
    auto* visible = ctx->mem->frame_stack().alloc_array<EntityId>(w.count());
    // ... fill, sort, submit ...
}   // <-- all of `visible` reclaimed in O(1)
```

The engine's **frame stack is double-buffered** (A/B). Frame N writes to A while the
GPU/DMA may still read frame N-1's submission from B; they swap at frame end. This
makes the render submission lifetime safe without copies — vital on PSP where the GE
reads display lists asynchronously.

## 4. PoolAllocator — fixed-size free list

```cpp
class PoolAllocator {
    uint8_t* base_; size_t blk_; uint32_t count_;
    uint32_t free_head_;                 // index, kInvalid == full
    static constexpr uint32_t kInvalid = 0xFFFFFFFF;
public:
    void init(void* mem, size_t block_size, uint32_t count) {
        base_=(uint8_t*)mem; blk_=block_size; count_=count; free_head_=0;
        for (uint32_t i=0;i<count;i++)   // thread the free list through the blocks
            *(uint32_t*)(base_+i*blk_) = (i+1<count) ? i+1 : kInvalid;
    }
    void* alloc() {
        if (free_head_==kInvalid) return nullptr;
        uint32_t i = free_head_;
        free_head_ = *(uint32_t*)(base_+i*blk_);
        return base_ + i*blk_;
    }
    void free(void* p) {
        uint32_t i = uint32_t(((uint8_t*)p - base_) / blk_);
        *(uint32_t*)(base_+i*blk_) = free_head_;
        free_head_ = i;
    }
};
```

```
 free list threaded in-place (no side metadata):
 ┌──────┬──────┬──────┬──────┐
 │ →1   │ →2   │ →3   │ →inv │     free_head=0
 └──────┴──────┴──────┴──────┘
 after alloc()×2, free(blk0):
 ┌──────┬──────┬──────┬──────┐
 │ →2   │used  │used  │ →inv │     free_head=0 (block0 re-linked to old head=2)
 └──────┴──────┴──────┴──────┘
```

**Use:** ECS component blocks, linked-node storage, anything fixed-size and churning.

## 5. ObjectPool<T> — typed recycling

```cpp
template <class T, uint32_t N>
class ObjectPool {
    alignas(T) uint8_t storage_[sizeof(T)*N];
    PoolAllocator pool_;
public:
    ObjectPool() { pool_.init(storage_, sizeof(T), N); }
    template <class... A> T* spawn(A&&... a) {
        void* m = pool_.alloc();
        return m ? new (m) T(static_cast<A&&>(a)...) : nullptr;   // ctor runs
    }
    void despawn(T* p) { p->~T(); pool_.free(p); }                // dtor runs
};
```

**Use:** gameplay objects with constructors — bullets, pickups, particles. `spawn`
returns `nullptr` when the pool is exhausted; the game decides policy (skip, recycle
oldest). The ceiling is *visible and fixed*, which is exactly what we want on console.

## 6. MemoryRoot — the boot-time carving

```cpp
class MemoryRoot {
    ArenaAllocator root_;          // owns the single OS/static allocation
    ArenaAllocator persistent_;    // app-lifetime
    StackAllocator frame_[2];      // double-buffered transient
    uint8_t        cur_ = 0;
public:
    static Result<MemoryRoot*> boot(size_t total, size_t frame_scratch);
    ArenaAllocator& persistent()   { return persistent_; }
    StackAllocator& frame_stack()  { return frame_[cur_]; }
    void            swap_frame()   { cur_ ^= 1; frame_[cur_].reset(); }
    ArenaAllocator  sub(size_t bytes); // carve a named child arena at boot
};
```

Boot-time budget table (example PSP, 24 MB usable):

| Region            | Bytes  | Allocator     | Lifetime  |
|-------------------|--------|---------------|-----------|
| persistent        | 2 MB   | Arena         | app       |
| resource cache    | 14 MB  | Arena+LRU     | app (LRU) |
| ecs pools         | 4 MB   | Pool/typed    | app       |
| scene stack       | 2 MB   | Stack(LIFO)   | per-scene |
| frame scratch ×2  | 2 MB   | Stack         | per-frame |

GBA equivalent (224 KB EWRAM + 32 KB IWRAM): IWRAM holds the frame stack and ECS hot
component arrays (fast 32-bit bus); EWRAM holds resource/persistent. The *same* table,
two orders of magnitude smaller, same code.

## 7. Debugging aids (host builds only)

- **Guard bytes / canaries** around pool blocks in debug → detect overflow writes.
- **Allocation tags** (a `const char*` per sub-arena) → a boot-time memory map dump:
  ```
  [phx.mem] root 24.0MB | persistent 2.0/2.0 | cache 11.3/14.0 | ecs 1.8/4.0 ...
  ```
- **Poison on free** (`0xDD`) in ObjectPool debug builds → catch use-after-despawn.
- Stripped entirely from console release builds (`PHX_BUILD == PHX_RELEASE`).

## 8. Why this beats a general heap (justification)

| Property            | General `malloc`        | Phosphorus allocators           |
|---------------------|-------------------------|------------------------------|
| Fragmentation       | accumulates over hours  | structurally impossible      |
| Worst-case latency  | unbounded (search/coalesce) | O(1) bounded             |
| Footprint provable? | no (depends on history) | yes (sum of carved arenas)   |
| Fits GBA static RAM | no heap available       | static buffer, fits by design|
| Cache locality      | scattered               | contiguous, hot data in IWRAM|

The cost is discipline: you must know your ceilings. For a retro-targeted engine,
that knowledge is mandatory anyway — so we make it a first-class API instead of a
debugging surprise.

## Testing (`tests/memory/`)

Stress tests: pool exhaustion + recycle (no leak, free list integrity), arena
mark/reset invariants, double-buffer swap aliasing, alignment correctness across
types, and a 1M-iteration churn that asserts the footprint never grows (the
anti-fragmentation proof).
