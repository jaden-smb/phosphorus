# Phosphorus Engine — Core Module

> `engine/core/` — the mandatory foundation every other module sits on.
> A **closed** module: zero outgoing dependencies. Compiles on all four tiers.
>
> **As built:** the App/main-loop described in §1 was lifted into the top-level
> **`runtime`** module (`phx/runtime/app.h`) — the loop needs memory+platform, which
> depend on core's types, so keeping it here would have formed a module cycle
> (caught by `depcheck`). Likewise `MemoryRoot` lives in the `memory` module. The
> *behavior* documented below is unchanged; only the home of those two headers moved.

## Responsibilities

| Subsystem   | Header                       | Purpose                                              |
|-------------|------------------------------|------------------------------------------------------|
| App / loop  | `phx/runtime/app.h` (moved)  | Owns the fixed-timestep main loop & subsystem wiring |
| Memory root | `phx/memory/memory_root.h` (moved) | Boots the root arena, hands out sub-allocators  |
| Logging     | `phx/core/log.h`             | Leveled, zero-cost-when-disabled logging macros       |
| Config      | `phx/core/config.h`          | Immutable boot config (budgets, sim rate, flags)      |
| Time        | `phx/core/time.h`            | Monotonic clock, frame timing, fixed-step accumulator |
| Profiling   | `phx/core/profile.h`         | Frame phase timings (see §6)                          |
| Types       | `phx/core/types.h`           | Fixed-width ints, `Result`, `TypeId`, hashing         |
| Pixel       | `phx/core/pixel.h`           | Shared `Rgba`/`PixelFormat` (render ↔ resource)       |
| Math        | `phx/core/math.h`, `fixed.h` | `scalar`, `vec2`, `mat3`, `aabb`, Q16 helpers         |

## 1. Initialization sequence

```
phx_main()
  └─ phx::Config cfg = Config::from_defaults() [+ overlay platform caps]
  └─ phx::App app(cfg)
       ├─ MemoryRoot::boot(cfg.total_ram)        // single OS allocation (or static buf on GBA)
       ├─ phx_platform_get()->init(&desc)        // C seam: window/gfx/input/audio/file
       │                                          //   (backend state is malloc'd backend-side
       │                                          //    today; desc.root_arena is null/unused)
       ├─ Log::init(plat.log_sink)
       ├─ Renderer::create(plat.gfx, sub_alloc)  // tier-selected backend
       ├─ Audio::create(plat.audio, sub_alloc)
       ├─ ResourceCache::create(sub_alloc, cfg.cache_bytes)
       ├─ World::create(sub_alloc, caps.max_entities)   // ECS
       └─ Game::on_start(ctx)                     // user hook
  └─ app.run()                                    // loop until quit
  └─ Game::on_stop(ctx) → teardown in reverse
```

**Key rule:** every `create()` takes an allocator, never calls `malloc` itself.
Allocation failure is reported via `Result`, surfaced at boot — never mid-frame.

```cpp
struct EngineCtx {
    Config        cfg;
    Platform*     plat;       // the C seam
    MemoryRoot*   mem;
    Renderer*     render;
    AudioMixer*   audio;
    InputState*   input;
    ResourceCache* res;
    World*        world;      // ECS
    Profiler*     prof;
    SceneStack*   scenes;
};
```

`EngineCtx` is the one object threaded through gameplay. It is passed by pointer; it
is never copied; it owns nothing (the `App` owns the storage).

## 2. Config (`phx/core/config.h`)

```cpp
struct Config {
    // budgets (defaults overridden per platform tier in caps.h)
    uint32_t total_ram      = 0;       // 0 => use phx_caps::total_main_ram
    uint32_t cache_bytes    = 0;       // resource LRU budget
    uint32_t frame_scratch  = 0;       // per-frame stack size (x2 for double-buffer)

    uint16_t sim_hz         = 60;      // fixed simulation rate
    uint16_t max_fps        = 0;       // 0 => vsync-locked
    uint16_t max_entities   = 0;       // 0 => caps default

    bool     vsync          = true;
    bool     enable_profiler = (PHX_BUILD == PHX_DEBUG);
    LogLevel log_level      = LogLevel::Info;

    static Config from_defaults();     // fills zeros from phx_caps
    Result validate() const;           // fails build-time-known bad budgets early
};
```

Config is **immutable after boot**. Anything tunable at runtime lives in gameplay
state, not here. This keeps the engine's resource footprint statically analyzable —
critical for the GBA where we must *prove* we fit before shipping.

## 3. Time (`phx/core/time.h`)

```cpp
struct Clock {
    uint64_t (*now_ns)(void* user);   // wired to platform monotonic source
    void*    user;
    uint64_t base;                     // captured at init for relative timing
};

// Fixed-step accumulator (see App::run in 00-architecture §7)
struct StepAccumulator {
    uint64_t step_ns;        // 1e9 / sim_hz
    uint64_t acc = 0;
    uint64_t max_ns;         // spiral-of-death clamp (e.g. 5 * step_ns)
    int      advance(uint64_t elapsed_ns);  // returns # of sim steps to run
    scalar   alpha() const;                 // render interpolation factor [0,1)
};
```

On GBA the "clock" is the VCount/timer registers; `sim_hz == 60` equals the display,
so `advance()` returns exactly 1 each frame and `alpha()` is always 0 — the
interpolation path is compiled out by the optimizer.

## 4. Math & Fixed-Point (`phx/core/math.h`, `fixed.h`)

### `fixed16` — Q16.16 signed fixed-point

```cpp
struct fixed16 {
    int32_t raw;                                  // 16.16
    static constexpr int kShift = 16;

    static constexpr fixed16 from_int(int32_t i)  { return { i << kShift }; }
    static constexpr fixed16 from_raw(int32_t r)  { return { r }; }
    constexpr int32_t to_int() const              { return raw >> kShift; }

    friend fixed16 operator+(fixed16 a, fixed16 b){ return { a.raw + b.raw }; }
    friend fixed16 operator-(fixed16 a, fixed16 b){ return { a.raw - b.raw }; }
    friend fixed16 operator*(fixed16 a, fixed16 b){
        return { int32_t((int64_t(a.raw) * b.raw) >> kShift) };   // 64-bit intermediate
    }
    friend fixed16 operator/(fixed16 a, fixed16 b){
        return { int32_t((int64_t(a.raw) << kShift) / b.raw) };
    }
};
fixed16 fx_sqrt(fixed16);        // Newton/LUT hybrid
fixed16 fx_sin(fixed16 turns);   // 256-entry LUT, turns in [0,1)
fixed16 fx_rcp(fixed16);         // reciprocal via LUT + 1 Newton step (no HW div on GBA)
```

> On ARM7TDMI there is **no hardware divide**. `operator/` and `fx_rcp` are the only
> sanctioned division paths; both are budgeted and LUT-accelerated. A lint rule flags
> raw `/` on `fixed16` outside `fixed.cpp`.

### Vector/matrix types are scalar-generic

```cpp
template <class T> struct Vec2 { T x, y; /* + - * dot len2 ... */ };
template <class T> struct AABB { Vec2<T> min, max; bool overlaps(const AABB&) const; };

using vec2   = Vec2<scalar>;     // scalar == float (PC/PSP) or fixed16 (GBA)
using aabb   = AABB<scalar>;
using vec2i  = Vec2<int32_t>;    // integer (tile coords, pixel coords)

// Mat3 is NOT templated like Vec2/AABB: its identity/translate/scale factories bake in
// the literal "1", which the fixed16 tier can only get correctly via s_from_int(1) (a raw
// `T(1)` would build a near-zero fixed16, since fixed16's single-int ctor is RAW bits, not
// a value) — so it operates on `scalar` directly.
struct Mat3 { scalar m[9]; /* 2D affine; translate/scale/apply — phx/core/math.h */ };
using mat3 = Mat3;
```

Gameplay writes `vec2 v{3, 4}; v = v * dt;` and it compiles to SSE on PC, FPU on PSP,
and shift/MUL on GBA — no `#ifdef` in user code.

## 5. Logging (`phx/core/log.h`)

```cpp
enum class LogLevel : uint8_t { Trace, Debug, Info, Warn, Error, Off };

#define PHX_LOG_INFO(ctx, fmt, ...)  PHX_LOG_(ctx, Info,  fmt, ##__VA_ARGS__)
#define PHX_LOG_WARN(ctx, fmt, ...)  PHX_LOG_(ctx, Warn,  fmt, ##__VA_ARGS__)
// expands to nothing when (level < ctx->cfg.log_level) is a compile-time-foldable
// build floor; below PHX_LOG_FLOOR the macro is empty → zero code, zero strings in ROM
```

Sink is a single function pointer supplied by the platform: `stdout` on PC,
`mgba_printf` on GBA (no-op on hardware), `pspDebugScreenPrintf` on PSP. Format
strings above the build floor are stripped from the binary entirely — important
because string literals are precious ROM bytes on GBA.

## 6. Profiling (`phx/core/profile.h`)

```cpp
struct ScopedZone {
    Profiler* p; uint32_t id; uint64_t t0;
    ScopedZone(Profiler* p, uint32_t id) : p(p), id(id), t0(p->clock()) {}
    ~ScopedZone() { p->add(id, p->clock() - t0); }
};
#define PHX_ZONE(ctx, name) phx::ScopedZone PHX_CONCAT(_z,__LINE__)((ctx)->prof, PHX_ZONE_ID(name))
```

The profiler keeps a fixed ring buffer of the last N frames of per-zone nanoseconds.
On PC it can dump Chrome-tracing JSON; on PSP it draws an on-screen bar overlay; on
GBA it is compiled out unless `enable_profiler` (it costs too much there to ship).

> **As built:** a leaner shape shipped first. `phx/core/profile.h` holds a pure-data
> `FrameProfile` (update/render/present/frame µs + budget — integer POD, so core stays
> closed and tier-agnostic); the **runtime loop** stamps it each frame from the platform
> clock (`App::profile()`), and **`UI::profile_overlay`** draws phase bars against the
> frame budget on every tier (four clock reads per frame — cheap enough to keep on,
> even on GBA). The scoped-zone/ring-buffer design above remains the plan for deeper
> tooling if it's ever needed.

## 7. Result & assertions (`phx/core/types.h`)

```cpp
enum class Status : int16_t { Ok=0, OutOfMemory, NotFound, BadArg, IoError, Unsupported };
template <class T> struct Result { Status st; T val;
    bool ok() const { return st == Status::Ok; }
    T&   unwrap() { PHX_ASSERT(ok()); return val; } };

#define PHX_ASSERT(c)  /* debug: trap+log; release: (void)0 */
#define PHX_VERIFY(c)  /* always-checked, returns Status::BadArg on failure */
```

- **Recoverable** failures (file missing, cache full) → `Result`/`Status`, handled.
- **Programmer** errors (null arg, pool overflow) → `PHX_ASSERT`, removed in release.
- **No exceptions** ever cross a module boundary.

## Testing

The unit suite (`tests/test_fixed.cpp`, `test_memory.cpp`, `test_time.cpp`, ...; `make test`)
covers: fixed-point ops vs. reference float (bounded error), the step
accumulator (clamping, alpha), arena/pool semantics, and FNV hashing stability. The
fixed-point determinism is held by the `make determinism` gate (both scalar tiers,
byte-compared) and by running the unit suite under `TIER=gba_sim`.
