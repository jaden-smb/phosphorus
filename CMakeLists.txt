# Phosphorus Engine — root build. One tree -> four runtime binaries + host tools.
# See docs/07-build-system.md for the full strategy.
cmake_minimum_required(VERSION 3.20)

# Version is single-sourced from the version header (see RELEASING.md) — parsed, never copied.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/engine/core/include/phx/core/version.h _phx_version_h)
foreach(_part MAJOR MINOR PATCH)
  string(REGEX MATCH "#define PHX_VERSION_${_part} +([0-9]+)" _ "${_phx_version_h}")
  set(_v "${CMAKE_MATCH_1}")   # if(MATCHES) below re-clobbers CMAKE_MATCH_1 — copy out first
  if(NOT _v MATCHES "^[0-9]+$")
    message(FATAL_ERROR "could not parse PHX_VERSION_${_part} from phx/core/version.h")
  endif()
  set(PHX_VERSION_${_part} ${_v})
endforeach()

project(PhosphorusEngine
  VERSION ${PHX_VERSION_MAJOR}.${PHX_VERSION_MINOR}.${PHX_VERSION_PATCH}
  LANGUAGES C CXX)
message(STATUS "Phosphorus version: ${PROJECT_VERSION}")

# ---- target selection ------------------------------------------------------
set(PHX_TARGET "linux" CACHE STRING "Target platform: linux | windows | gba | psp")
set_property(CACHE PHX_TARGET PROPERTY STRINGS linux windows gba psp)
message(STATUS "Phosphorus target: ${PHX_TARGET}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/phx_module.cmake)
include(cmake/caps_select.cmake)   # sets PHX_TARGET_* defines + PHX_RENDER_TIER

# ---- per-target compile policy --------------------------------------------
if(PHX_TARGET STREQUAL "gba" OR PHX_TARGET STREQUAL "psp")
  # Consoles: no exceptions, no RTTI — matches the engine's no-throw contract.
  add_compile_options(-fno-exceptions -fno-rtti)
  add_compile_definitions(PHX_NO_EXCEPTIONS=1)
  # Every GBA/PSP build always ships on real hardware or an emulator standing in for it, never
  # a dev host — so PHX_BUILD_RELEASE (phx/core/assert.h, phx/core/log.h) is unconditional here,
  # same as the Makefile's GBA_FLAGS/PSP_FLAGS: a debug PHX_ASSERT trap hangs a console instead
  # of dropping into a debugger, and Trace/Debug log format strings just burn ROM the GBA size
  # gate (docs/09 MVP gate) is already tight on. Host/Windows builds instead pick this up from
  # the standard CMake Release/RelWithDebInfo/MinSizeRel configs, which already define NDEBUG —
  # see the `defined(NDEBUG)` fallback in assert.h/log.h — so pass -DCMAKE_BUILD_TYPE=Release
  # for a release desktop build; the default (no CMAKE_BUILD_TYPE) stays debug, matching the
  # Makefile's default.
  add_compile_definitions(PHX_BUILD_RELEASE=1)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- modules (each is a STATIC lib; only the matching backend compiles) ----
add_subdirectory(engine/core)
add_subdirectory(engine/memory)
add_subdirectory(engine/platform)
add_subdirectory(engine/render)
add_subdirectory(engine/ecs)
add_subdirectory(engine/input)
add_subdirectory(engine/audio)
add_subdirectory(engine/resource)
add_subdirectory(engine/scene)
add_subdirectory(engine/physics)
add_subdirectory(engine/anim)
add_subdirectory(engine/ui)
add_subdirectory(engine/runtime)

# ---- the example games -----------------------------------------------------
add_subdirectory(examples/platformer)
add_subdirectory(examples/miracle-player)
add_subdirectory(examples/emberwing)
add_subdirectory(examples/tinyllm)

# ---- host-only tools (always built for the host, even when cross-compiling) -
# The asset pipeline (docs/08): per-format CONVERTERS bake author sources into intermediate
# .phx* files; the phxpack ASSEMBLER merges them into assets.phxp. They share one bake path
# (tools/phxpack/builders.h). Phosphorus Studio (tools/phxstudio): phxnew always; phxstudio and the
# standalone phxtmap / phxentity editors when configured with -DPHX_USE_SDL=ON (they open a
# window through the engine's SDL platform backend).
if(NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(tools/phxpack)     # assembler
  add_subdirectory(tools/phxsprite)   # PNG/sprdef/json -> .phxspr
  add_subdirectory(tools/phxtile)     # Tiled .tmj      -> .phxtmap
  add_subdirectory(tools/phxsnd)      # WAV             -> .phxsnd
  add_subdirectory(tools/phxbin)      # JSON            -> .phxbin (+ .gen.h)
  add_subdirectory(tools/phxviz)      # WAV             -> .phxviz (visualization track)
  add_subdirectory(tools/phxstudio)   # phxnew (+ phxstudio / phxtmap / phxentity with PHX_USE_SDL)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- install / package (host SDK only) -------------------------------------
# `cmake --install` lays down a host SDK: the 13 static module libs + public headers + the five
# asset-pipeline CLIs + a find_package(phosphorus) config; `cpack` wraps the same tree in a
# versioned tar.gz. Console targets deliberately have no install flow — their shippable
# artifacts are the ROM/EBOOT bundles from `make dist-gba` / `make dist-psp` (see RELEASING.md).
if(NOT CMAKE_CROSSCOMPILING)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  # GNUInstallDirs derives DOCDIR from PROJECT_NAME ("PhosphorusEngine"); ship as plain "phosphorus".
  set(CMAKE_INSTALL_DOCDIR ${CMAKE_INSTALL_DATAROOTDIR}/doc/phosphorus)

  set(_phx_modules core memory platform render ecs input audio resource scene physics anim ui runtime)
  foreach(_m ${_phx_modules})
    install(TARGETS phx_${_m} EXPORT phosphorus-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY engine/${_m}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  endforeach()

  install(TARGETS phxpack phxsprite phxtile phxsnd phxbin phxviz
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

  # find_package(phosphorus CONFIG) support: import the libs as phosphorus::phx_<module>.
  install(EXPORT phosphorus-targets
          FILE phosphorus-targets.cmake
          NAMESPACE phosphorus::
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/phosphorus)
  configure_package_config_file(cmake/phosphorus-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/phosphorus-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/phosphorus)
  # Pre-1.0: any MINOR bump may break API/asset formats, so compatibility is same-minor.
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/phosphorus-config-version.cmake
    COMPATIBILITY SameMinorVersion)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/phosphorus-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/phosphorus-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/phosphorus)
  install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

  set(CPACK_PACKAGE_NAME phosphorus)
  set(CPACK_PACKAGE_VENDOR "Phosphorus Engine")
  set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
  set(CPACK_GENERATOR TGZ)
  string(TOLOWER ${CMAKE_SYSTEM_NAME} _phx_os)
  set(CPACK_PACKAGE_FILE_NAME phosphorus-${PROJECT_VERSION}-sdk-${_phx_os}-${CMAKE_SYSTEM_PROCESSOR})
  include(CPack)
endif()

# ---- architectural law: acyclic module dependency graph (CI gate) ----------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_custom_target(depcheck ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/common/depcheck.py ${CMAKE_SOURCE_DIR}/engine
    COMMENT "Verifying acyclic module dependency graph (docs/00 §3)")
endif()
