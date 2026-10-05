# examples/platformer/CMakeLists.txt — the same sources build for all four targets.
# (On the host without CMake, the Makefile builds this example as `make platformer`.)
add_executable(platformer
  src/main.cpp
  src/systems.cpp
)
# main.cpp self-bakes the demo bundle via the host BundleWriter, so the example needs the
# phxpack writer headers on the include path (and the engine resource headers they reference).
target_include_directories(platformer PRIVATE
  ${CMAKE_SOURCE_DIR}/tools/phxpack
  ${CMAKE_SOURCE_DIR}/tools/common
  ${CMAKE_SOURCE_DIR}/engine/resource/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(platformer PRIVATE
  phx_core phx_memory phx_platform phx_render phx_ecs phx_input
  phx_resource phx_scene phx_physics phx_anim phx_ui phx_audio phx_runtime
)

# --- per-target packaging ----------------------------------------------------
# A real project bakes authored assets offline with `phxpack` into assets.phxp; this demo
# synthesizes its bundle at startup (src/bake.h) so it stays self-contained. The packaging
# hooks below still apply when shipping a prebaked bundle.
if(PHX_TARGET STREQUAL "gba")
  phx_gba_rom(platformer)                          # link bundle into the ROM, emit .gba
elseif(PHX_TARGET STREQUAL "psp")
  phx_psp_eboot(platformer "Phosphorus Platformer")   # -> EBOOT.PBP (fixup + prxgen + mksfo + pack-pbp)
endif()
# NOTE: the runnable console artifacts (with the asset bundle embedded + a console entry point —
# gba_main.cpp / psp_main.cpp) are produced by the Makefile: `make gba-platformer` /
# `make psp-platformer`. This self-baking demo's CMake target builds the portable engine for the
# console but does not embed the bundle, so prefer the Makefile for a loadable ROM/EBOOT.
# PC: the exe writes/loads platformer.phxp in the working dir alongside it.
