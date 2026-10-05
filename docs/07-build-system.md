# Phosphorus Engine — Build System

> One CMake tree → four runtime binaries + host tools. The build system is itself a
> portability mechanism: a new platform is *a toolchain file + a backend folder*, with
> zero changes to the engine or game CMake targets.

## 1. Layout

```
fenix/
├── CMakeLists.txt              # root: options, target registration, depcheck
├── cmake/
│   ├── phx_module.cmake        # helper: phx_add_module() / phx_add_backend()
│   ├── gba.toolchain.cmake     # devkitARM
│   ├── psp.toolchain.cmake     # pspsdk / devkitPSP
│   ├── caps_select.cmake       # maps target -> phx_caps tier header
│   └── warnings.cmake          # shared strict flags
├── engine/<module>/CMakeLists.txt    # each module is a static lib target
├── tools/<tool>/CMakeLists.txt       # host-only executables
└── examples/platformer/CMakeLists.txt
```

## 2. The four configure invocations

```bash
# Linux  (host)
cmake -S . -B build/linux  -DPHX_TARGET=linux  -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux

# Windows (the PROVEN path: cross with MinGW-w64 — works from Linux and in CI,
# verified by running the full suite under Wine; host MSVC also configures)
cmake -S . -B build/windows -DPHX_TARGET=windows \
      -DCMAKE_TOOLCHAIN_FILE=cmake/mingw.toolchain.cmake
cmake --build build/windows   # -> platformer.exe (or: make win for every host binary)

# GBA    (cross, devkitARM)
cmake -S . -B build/gba    -DPHX_TARGET=gba \
      -DCMAKE_TOOLCHAIN_FILE=cmake/gba.toolchain.cmake
cmake --build build/gba     # -> platformer.gba (ROM)

# PSP    (cross, pspsdk)
cmake -S . -B build/psp    -DPHX_TARGET=psp \
      -DCMAKE_TOOLCHAIN_FILE=cmake/psp.toolchain.cmake
cmake --build build/psp     # -> EBOOT.PBP
```

`PHX_TARGET` drives three things via `cmake/caps_select.cmake`:
1. which `platform/src/<backend>` and `render/src/<backend>` sources compile,
2. which `phx_caps` tier header is active (`-DPHX_CAPS_HEADER=...`),
3. platform compile flags (no-exceptions/no-rtti on console, etc.).

### Debug vs. release (`PHX_BUILD_RELEASE`)

`PHX_BUILD_RELEASE` strips `PHX_ASSERT` to `((void)0)` and raises the log floor to `Info` (see
`engine/core/include/phx/core/assert.h` / `log.h`). It is wired differently per target because
"release" means different things on a dev host vs. a shipping console:

- **GBA / PSP**: always on, unconditionally — set in the root `CMakeLists.txt`'s console block
  and in the Makefile's `GBA_FLAGS`/`PSP_FLAGS`. These builds only ever run on real hardware or
  an emulator standing in for it; a debug `PHX_ASSERT` trap hangs a console instead of dropping
  a developer into a debugger, and every Trace/Debug log format string is dead weight against
  the GBA size gate (docs/09 MVP gate). There is no "GBA debug build".
- **Linux / Windows**: opt-in, matching the Makefile's default. `make check` / `ctest` stay in
  debug (asserts live) unless requested otherwise:
  - Makefile: `make check RELEASE=1` (or the dedicated `make release` gate, which runs the whole
    check suite under `RELEASE=1` in its own build root — this is where a variable that's only
    referenced inside `PHX_ASSERT` would surface as an `-Wunused-variable` warning once the
    macro compiles away, instead of only showing up in a devkitARM/pspsdk cross build).
  - CMake: `-DCMAKE_BUILD_TYPE=Release` (or `RelWithDebInfo`/`MinSizeRel`). These already define
    `NDEBUG`, which `assert.h`/`log.h` treat as a synonym for `PHX_BUILD_RELEASE` — no extra
    `add_compile_definitions` needed for the host/Windows path.

## 3. `phx_add_module` helper (keeps every module identical)

```cmake
# cmake/phx_module.cmake
function(phx_add_module name)
  cmake_parse_arguments(M "" "" "SRC;DEPS;BACKENDS" ${ARGN})
  add_library(phx_${name} STATIC ${M_SRC})
  target_include_directories(phx_${name} PUBLIC include)
  target_link_libraries(phx_${name} PUBLIC ${M_DEPS})
  target_compile_features(phx_${name} PUBLIC cxx_std_17)
  # link only the backend matching PHX_TARGET:
  foreach(b ${M_BACKENDS})
    if(b STREQUAL PHX_TARGET OR b STREQUAL PHX_RENDER_TIER)
      target_sources(phx_${name} PRIVATE ${b}/...)
    endif()
  endforeach()
endfunction()
```

Result: `engine/platform/CMakeLists.txt` is ~5 lines; the linker gets exactly one
backend; no `#ifdef` soup, no dead backend code in the binary.

## 4. Console toolchain notes

### `cmake/gba.toolchain.cmake`

```cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(DEVKITARM $ENV{DEVKITARM})
set(CMAKE_C_COMPILER   ${DEVKITARM}/bin/arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${DEVKITARM}/bin/arm-none-eabi-g++)
set(ARCH "-mthumb -mthumb-interwork -mcpu=arm7tdmi")
set(CMAKE_CXX_FLAGS_INIT "${ARCH} -fno-exceptions -fno-rtti -ffast-math -Os")
# link: specs=gba_cart, then objcopy ELF -> .gba, then gbafix (header/checksum)
```

Post-build custom command: `arm-none-eabi-objcopy -O binary` → `gbafix` writes the
cartridge header. Output: `platformer.gba`, runnable in mGBA or on hardware.

### `cmake/psp.toolchain.cmake`

```cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER   psp-gcc)
set(CMAKE_CXX_COMPILER psp-g++)
set(CMAKE_CXX_FLAGS_INIT "-G0 -fno-exceptions -fno-rtti -O2")
# link against pspsdk libs; PSP_MODULE_INFO via build_prx
```

Post-build: `psp-fixup-imports` → `mksfo` (param.sfo) → `pack-pbp` → `EBOOT.PBP`,
runnable in PPSSPP or on a CFW PSP.

## 5. Asset build integration (design sketch — not what's actually wired today)

Assets *could* be a CMake target — bundles built by host tools before the runtime binary
that consumes them, with cross-target builds still building tools for the **host** — but
`examples/platformer/CMakeLists.txt` doesn't do this today; see its own comment: "A real
project bakes authored assets offline with `phxpack` into assets.phxp; this demo [does
something simpler]." The actual GBA/PSP asset bake is Makefile-driven (`GBA_PLAT_SRC`'s
`bake_main.cpp` step, `bin2s`). What CMake asset integration *would* look like:

```cmake
# tools always build for host, even in a GBA/PSP configure (via ExternalProject/host build)
add_custom_command(
  OUTPUT  ${CMAKE_BINARY_DIR}/assets.phxp
  COMMAND phxpack --target ${PHX_TARGET}
          --in  ${CMAKE_SOURCE_DIR}/examples/platformer/assets
          --out ${CMAKE_BINARY_DIR}/assets.phxp
  DEPENDS phxpack ${ASSET_SOURCES})
add_custom_target(assets DEPENDS ${CMAKE_BINARY_DIR}/assets.phxp)
add_dependencies(platformer assets)
# GBA: assets.phxp is bin2o'd into the ROM; PSP/PC: shipped alongside EBOOT/exe.
```

CMake's `DEPENDS` list re-runs `phxpack` on any input change; *inside* that run,
`phxpack`'s lock file (`docs/08-tooling.md` §2/§9) makes the re-bake incremental — only
the changed sources re-encode, the rest are reused from the previous bundle, and an
invocation whose inputs are all unchanged is a no-op ("up to date").

## 6. Dependency enforcement in CI

```cmake
add_custom_target(depcheck ALL
  COMMAND ${Python3_EXECUTABLE} tools/common/depcheck.py engine/
  COMMENT "Verifying acyclic module dependency graph")
```

`depcheck.py` parses `#include "phx/<mod>/..."` edges and fails if any back-edge or
cycle exists (the law in `docs/00` §3). A violation breaks the build like a compile
error — the architecture is *mechanically* enforced, not just documented.

## 7. CI matrix

The matrix as implemented (`.github/workflows/ci.yml`):

| Job        | Toolchain      | Output            | Gates run                                    |
|------------|----------------|-------------------|----------------------------------------------|
| host       | gcc            | ELF               | `make check` (all suites) + `make determinism` (both scalar tiers byte-identical) |
| sanitize   | gcc            | ELF (instrumented)| `make sanitize` — full check suite under ASan+UBSan, no recover |
| windows    | MinGW-w64 + Wine| static PE32+ exes| `make win` + the unit suite AND the full game run under Wine |
| gba-size   | devkitARM      | .gba              | ROM build + `make size-gate` (ROM/IWRAM/EWRAM budgets) |
| psp        | pspsdk         | EBOOT.PBP         | full-game EBOOT builds                       |

The GBA job asserts the **size budget** — a regression that blows it fails CI,
keeping pillar #2 honest.

## 8. Single-command developer ergonomics

The top-level `Makefile` is the day-to-day driver (no CMake needed on the host):

```
make check            # THE gate: unit + integration + pipeline + tools + depcheck
make determinism      # both scalar tiers, byte-compared
make sanitize         # ASan+UBSan over the whole check suite
make release          # the whole check suite built+run with PHX_BUILD_RELEASE=1
make gba-platformer   # devkitARM ROM        make psp-platformer  # pspsdk EBOOT
make win              # MinGW-w64 .exes      make sdl / make gl   # windowed example
```

The Makefile compiles the engine directly (per-tier object dirs, so the float and
fixed-point tiers coexist without cleans); CMake remains the canonical multi-target
packaging path, so IDEs and toolchain-file-driven cross builds work unmodified.
