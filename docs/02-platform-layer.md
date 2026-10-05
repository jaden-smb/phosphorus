# Phosphorus Engine — Platform Layer

> `engine/platform/` — the **C-ABI seam** that isolates every OS/hardware dependency.
> Above this line: portable C++17. Below it: exactly one backend, chosen by the linker.
> No `phx` module other than the platform backends may include a platform SDK header.

## 1. The seam: `phx_platform`

A flat struct of function pointers and opaque handles. C linkage, no C++ in the
interface, so it works identically for the GBA's register pokes and the PC's SDL.

```c
// phx/platform/platform.h  (C header, included by C++ above)
#ifdef __cplusplus
extern "C" {
#endif

typedef struct phx_window  phx_window;   // opaque
typedef struct phx_gfx     phx_gfx;      // opaque graphics device handle
typedef struct phx_file    phx_file;     // opaque file handle (stable in-memory view)

// The audio output device. The platform owns the device, never the mixer: start() runs `fill`
// where the device wants samples (SDL/PSP thread, GBA VBlank IRQ). Null where there is none.
typedef void (*phx_audio_fill)(void* user, int16_t* out, int frames);   // stereo S16 frames
typedef struct phx_audio {
    int   rate;                                                  // SDL/PSP 44100, GBA 18157
    int   (*start)(int rate, phx_audio_fill fill, void* user);  // 0 = playing
    void  (*stop)(void);
} phx_audio;

typedef struct phx_platform {
    /* lifecycle */
    int   (*init)(const phx_platform_desc* desc);   // create window+devices
    void  (*shutdown)(void);

    /* clock */
    uint64_t (*clock_ns)(void);                     // monotonic
    void  (*sleep_ns)(uint64_t ns);                 // may be no-op on consoles

    /* window / present */
    int   (*pump_events)(void);                     // returns 0 to request quit
    void  (*present)(void);                         // swap / vblank wait

    /* device handles handed to higher layers */
    phx_gfx*   (*gfx)(void);
    phx_audio* (*audio)(void);

    /* input snapshot (filled by platform, read by input module) */
    void  (*poll_input)(phx_input_raw* out);

    /* file / asset access (load-once, stable-pointer views) */
    phx_file* (*open)(const char* path, size_t* out_size);
    const void* (*map)(phx_file*);                  // zero-copy view; ROM ptr on GBA
    void  (*close)(phx_file*);

    /* persistent save storage — a tiny key->blob store. A file on PC; battery-backed
     * SRAM at 0x0E000000 on GBA (single slot, byte-wise 8-bit bus, key ignored);
     * sceIo on PSP (bare keys anchor at ms0:/PSP/SAVEDATA/PHX/ — a relative path has
     * no cwd when an EBOOT is launched directly, and a UMD game can't write next to
     * itself). The CALLER validates its own magic/version: uninitialised SRAM reads
     * as garbage, so "no save present" is a failed magic check, not a seam state. */
    int   (*save)(const char* key, const void* data, uint32_t size);
    int   (*load)(const char* key, void* out, uint32_t cap, uint32_t* out_size);

    /* logging sink */
    void  (*log)(int level, const char* msg);
} phx_platform;

/* Each backend exports exactly this one symbol; CMake links one backend. */
const phx_platform* phx_platform_get(void);

#ifdef __cplusplus
}
#endif
```

The C++ `Platform` wrapper (`phx/platform/platform.hpp`) is a thin, inlined adapter
over the struct — it adds type safety and `Result` but no virtual dispatch.

## 2. Why a struct-of-pointers, not an abstract base class

| Concern                | C++ ABC (virtual)        | `phx_platform` (C struct)        |
|------------------------|--------------------------|----------------------------------|
| GBA (no exceptions/RTTI)| awkward, vtable overhead | natural, just data               |
| Link-time backend pick | needs factory + RTTI     | one symbol, linker resolves      |
| Hot-path cost          | indirect call + vtable   | one indirect call (or inlined)   |
| C interop / tooling    | hard                     | trivial                          |
| Mixing backends        | possible footgun         | impossible (one TU links)        |

The few calls that are genuinely hot (clock, input poll) are small enough to be
`static inline` shims that the backend can also provide as macros on GBA.

## 3. What each backend implements

```
engine/platform/src/
├── null/     ← headless: software framebuffer, virtual clock, scripted input/pointer
│               (what the test suites run on — deterministic by construction)
├── sdl/      ← shared by windows + linux (SDL2: window, GL ctx, input+mouse, audio)
├── gba/      ← devkitARM: MMIO registers, DMA, IRQ, ROM-mapped "files", SRAM save
└── psp/      ← pspsdk: sceDisplay/sceCtrl/sceAudio/sceIo (+ save), callbacks
```

### 3.1 PC (SDL2 backend) — `sdl/`

- `init` → `SDL_CreateWindow` + `SDL_GL_CreateContext`. No `SDL_GL_SetAttribute` context-
  version request — it gets whatever default/compatibility context the driver hands back,
  which is what the GL 1.1 immediate-mode backend (`docs/03` §6) needs. No Vulkan surface:
  a Vulkan backend is an explicit non-goal (`docs/03` §6 records the decision).
- `clock_ns` → `SDL_GetPerformanceCounter` scaled.
- `poll_input` → `SDL_PollEvent` accumulated into `phx_input_raw`.
- `open`/`map` → stdio read-once into a heap buffer; `map` returns that stable pointer.
  There is deliberately **no OS mmap** (`mmap`/`MapViewOfFile`) — the seam contract is
  "map returns a stable view for the mount's lifetime", not how it's backed, and a
  portable 20-line stdio loader beats two per-OS mapping paths on the one platform
  where RAM is free. Cost: the bundle is resident heap, counted like any other buffer.
- `audio` → SDL audio callback pulls from our mixer ring buffer.
- Rationale: SDL2 is the one *optional but pragmatic* dependency on desktop; it is
  isolated entirely here, so a future GLFW or raw-win32 backend is a sibling folder.

### 3.2 GBA backend — `gba/`

- No OS, no files, no heap. "Files" are addresses inside the ROM/`.gba` image; `map`
  returns a `const void*` straight into cartridge address space — **zero-copy by
  definition**.
- `clock_ns` derives from a hardware timer (TM0) or VCount.
- `present` waits for VBlank (`SWI VBlankIntrWait`); the renderer's OAM/VRAM updates
  happen in the VBlank window.
- `poll_input` reads `REG_KEYINPUT` (active-low) → normalized `phx_input_raw`.
- DMA channels are exposed to the render/audio backends through small helpers, not
  through the generic seam (they are GBA-private).

### 3.3 PSP backend — `psp/`

- `init` sets up `sceGuInit`, display list, double buffer in VRAM.
- Requires the **callback thread + exit callback** boilerplate (HOME button); handled
  here so gameplay never sees it.
- `open`/`map` → the `.phxp` bundle is **linked into the EBOOT** and registered before
  boot via `phx_psp_set_bundle()` (same pattern as the GBA ROM bundle); `map` returns
  that pointer — no `sceIo` read at mount, and the *contract* "map returns a stable
  view" still holds.
- `audio` → `sceAudioChReserve` + blocking output thread fed by the mixer.

## 4. `phx_input_raw` — the normalized input frame

The platform fills a single POD struct; the `input` module turns it into
edge/held/axis semantics (see `docs` for input). This keeps device specifics below
the seam.

```c
typedef struct phx_input_raw {
    uint32_t buttons;     // bitmask in canonical phx_button order (see input.h)
    int16_t  axis[4];     // lx, ly, rx, ry  (-32768..32767), 0 where absent
    int16_t  pointer_x, pointer_y;   // mouse/touch; -1 if none
    uint8_t  pointer_down;
    uint8_t  connected_pads;
} phx_input_raw;
```

GBA maps its 10 keys into `buttons`; PSP maps Cross/Circle/etc. + analog nub into
`axis[0..1]`; PC maps keyboard+mouse+gamepad. The **canonical button order is defined
once** so a game's `Button::Jump` means the same thing everywhere.

## 5. Conformance: every backend passes the same tests

Per-seam verification suites run against each backend (render smoke, audio-device
smoke, save smoke — on mGBA/PPSSPP/a real window) — the CI runs them against each
build:

- clock monotonicity & resolution sanity,
- `open`/`map`/`close` round-trips a known bundle and checks bytes,
- input button-order canonicalization (replay a synthetic raw frame),
- present/pump quit signalling.

A new platform is "done" when it links the seam **and** passes conformance. That is
the entire porting checklist — by design.

## 6. Adding a new platform (the extensibility story)

```
1. Create engine/platform/src/<newplat>/   implementing phx_platform_get().
2. Add a render backend under engine/render/src/<newplat or shared tier>/.
3. Add cmake/<newplat>.toolchain.cmake (compiler, sysroot, flags).
4. Add a phx_caps tier in phx/core/caps_<newplat>.h.
5. Pass the per-seam verification smokes (render, audio device, save) on the new target.
```

No edits to `core`, `ecs`, `render` public API, or any gameplay code. This is the
seam paying for itself. See `docs/09-roadmap.md` §Scalability for DS/3DS/Vita/Android/
Steam Deck specifics.
