# engine/render/CMakeLists.txt — one 2D-intent API; EXACTLY ONE backend links per build.
# The backend-agnostic front end (record/sort/dispatch) + the LRU texture cache always compile;
# the backend is chosen by target:
#   gba  -> src/gba (PPU, tier 0)      psp -> src/gu (sceGu, tier 1)
#   desktop -> src/soft (the software GOLDEN reference, headless/CI) by default,
#              or src/gl (OpenGL, tier 2) when PHX_USE_GL=ON (needs a GL context from SDL).
# The soft and gl backends both target tier 2, so they are mutually exclusive (linking both
# would double-define phx_make_render_backend) — hence an explicit switch, not tier matching.
add_library(phx_render STATIC
  src/renderer.cpp
  src/texture_cache.cpp
)
target_include_directories(phx_render
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(phx_render PUBLIC phx_core phx_memory phx_platform)

option(PHX_USE_GL "Desktop: link the OpenGL render backend instead of the software reference" OFF)

if(PHX_TARGET STREQUAL "gba")
  target_sources(phx_render PRIVATE src/gba/gba_ppu.cpp)
  set(_render_backend "gba-ppu (tier 0)")
elseif(PHX_TARGET STREQUAL "psp")
  target_sources(phx_render PRIVATE src/gu/gu_backend.cpp)
  set(_render_backend "psp-gu (tier 1)")
elseif(PHX_USE_GL)
  find_package(OpenGL REQUIRED)
  target_sources(phx_render PRIVATE src/gl/gl_backend.cpp)
  target_compile_definitions(phx_render PRIVATE PHX_HAVE_GL=1)
  target_link_libraries(phx_render PUBLIC OpenGL::GL)
  set(_render_backend "gl (tier 2)")
else()
  target_sources(phx_render PRIVATE src/soft/soft_renderer.cpp)
  set(_render_backend "soft (tier 2, golden reference)")
endif()

message(STATUS "Phosphorus render backend: ${_render_backend}")
