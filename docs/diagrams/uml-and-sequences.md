# Phosphorus Engine — UML & Sequence Diagrams

ASCII UML (class/module/sequence) referenced by the module docs. Kept in one place so
the relationships are auditable at a glance.

---

## 1. Core ownership (class diagram)

```
            ┌──────────────────────────────────────────────┐
            │                     App                       │
            │  - cfg: Config                                │
            │  - mem: MemoryRoot                            │
            │  - ctx: EngineCtx                             │
            │  + run(Game*) : int                           │
            └───────────────┬──────────────────────────────┘
                            │ owns (composition)
   ┌──────────┬───────────┬─┴────────┬───────────┬──────────┬──────────┐
   ▼          ▼           ▼          ▼           ▼          ▼          ▼
┌────────┐┌──────────┐┌────────┐┌──────────┐┌────────┐┌────────┐┌──────────┐
│Memory  ││Renderer  ││Audio   ││Resource  ││ ecs::  ││Scene   ││Input/UI/ │
│Root    ││          ││Mixer   ││Cache     ││ World  ││Stack   ││Physics   │
└───┬────┘└────┬─────┘└───┬────┘└────┬─────┘└───┬────┘└────────┘└──────────┘
    │ carves   │ uses     │ uses     │ uses     │
    ▼          ▼          ▼          ▼          ▼
┌──────────────────────────────────────────────────────────────┐
│   const phx_platform*   (the C seam — single indirect table)  │
└──────────────────────────────────────────────────────────────┘

EngineCtx is the POD bag of non-owning pointers threaded into gameplay.
```

---

## 2. Allocator hierarchy (class diagram)

```
        ┌────────────────────┐
        │   ArenaAllocator   │   bump pointer; reset_to(mark)
        │  + alloc/make/mark │
        └─────────┬──────────┘
                  │ «extends»
        ┌─────────▼──────────┐        ┌────────────────────┐
        │   StackAllocator   │        │   PoolAllocator    │  fixed-size free list
        │  + Scope (RAII)    │        │  + alloc/free O(1) │
        └────────────────────┘        └─────────┬──────────┘
                                                 │ «wraps»
                                       ┌─────────▼──────────┐
                                       │  ObjectPool<T,N>   │  typed spawn/despawn
                                       └────────────────────┘

   MemoryRoot owns: root Arena -> { persistent Arena, frame Stack[2], sub-arenas }
```

---

## 3. ECS storage (class diagram)

```
   ┌──────────────────────┐        1     *  ┌─────────────────────────┐
   │        World         │────────────────►│   SparseSet<C>          │
   │ - generations[]      │   registry of   │ - sparse[max] : u32     │
   │ - free_slots[]       │   one set per   │ - dense[]     : Entity  │
   │ + spawn/despawn      │   component     │ - comps[]     : C       │
   │ + add/remove/get/has │   type          │ + add/remove(swap) O(1) │
   │ + each<Cs...>(fn)    │                 │ + data()/ents()/size()  │
   │ + defer()/flush      │                 └─────────────────────────┘
   └──────────┬───────────┘
              │ iterated by
              ▼
   ┌──────────────────────┐     System list runs in FIXED order each sim step:
   │   System (interface) │     Input → AI → Physics → Interaction → Anim → Camera
   │ + tick(World&, dt)   │
   └──────────────────────┘
```

---

## 4. Boot sequence

```
 phx_main()                App                MemoryRoot   platform     subsystems
   │  Config::from_defaults  │                     │           │            │
   ├────────────────────────►│                     │           │            │
   │                         │  boot(total_ram)    │           │            │
   │                         ├────────────────────►│  (1 OS alloc / static) │
   │                         │  platform_get()/init│           │            │
   │                         ├────────────────────────────────►│  window+devices
   │                         │  create(sub_alloc, caps)         │            │
   │                         ├─────────────────────────────────────────────►│ Renderer/Audio/
   │                         │                                  │            │ Cache/World/UI
   │                         │  Game::on_start(ctx)             │            │
   │                         ├──────────────────────────────────────────────► mount assets,
   │                         │                                  │            │ register comps,
   │                         │                                  │            │ push first Scene
   │  run(game)              │                                  │            │
   └────────────────────────►│  ── main loop (see §5) ──        │            │
```

---

## 5. Frame sequence (fixed-step sim + interpolated render)

```
 App        Input     SystemList    SceneStack   Renderer    platform     AudioMixer
  │ clock_ns │           │              │           │           │            │
  │──────────┼───────────┼──────────────┼───────────┼──────────►│ (monotonic)│
  │ poll_input                          │           │           │            │
  │──────────►│ (raw -> Button edges)   │           │           │            │
  │  while(acc >= step):                │           │           │            │
  │            │  tick(world, dt) × each system in fixed order   │            │
  │            │──────────►│ Input/AI/Physics/Interaction/Anim/Camera        │
  │            │           │  update(scene, dt)     │           │            │
  │  render(alpha):        │              │         │           │            │
  │            │           │  render(scene, alpha)──►│ begin_frame│           │
  │            │           │              │ draw_tilemap/sprite/UI ──────────►│ batch+sort
  │            │           │              │           │ end_frame -> submit   │
  │            │           │              │           │──────────►│ present(vblank/swap)
  │  frame_stack.swap()  (O(1) transient reclaim)    │           │            │
  │  ── audio callback pulls samples ───────────────────────────────────────►│ mix()
```

---

## 6. Asset pipeline (data-flow, build-time)

```
   assets/*.png/.wav/.tmj/.json
        │ phxsprite / phxsnd / phxtile / phxbin
        │ (per-target encodes run here, in the shared BundleWriter: tier-0 sound resample
        │  + 4bpp paletted textures, tier-1 swizzled textures — docs/06 §4, docs/08 §2/§5)
        ▼
   *.phxspr / *.phxsnd / *.phxtmap / *.phxbin
        │ phxpack  (FNV-1a names -> sorted TOC, optional LZ77; writes <out>.lock for
        │           incremental/reproducible rebakes — docs/08 §2/§9)
        ▼
   assets.phxp  ──(GBA: bin2o into ROM | PSP/PC: shipped alongside)──►  runtime
        │ ResourceCache::mount  =  heap load-once (PC) / linked-in EBOOT data (PSP) / ROM ptr (GBA)
        ▼
   zero-copy typed views (TextureView/TilemapView/SoundView/BlobView)
```

---

## 7. Platform seam selection (deployment view)

```
                      phx_platform_get()   (ONE symbol, linker-resolved)
                                  │
   build = linux/win  ───────────┼──────────  build = gba          build = psp
        ▼                         ▼                  ▼                    ▼
  src/sdl/*.cpp           src/{windows,linux}/   src/gba/*.cpp        src/psp/*.cpp
  (SDL2 window/GL/         (timer/paths)         (MMIO/DMA/IRQ/       (sceDisplay/
   input/audio/file)                              ROM-mapped files)    sceCtrl/sceIo)

  Exactly one translation unit defines phx_platform_get(); no runtime branching.
```
