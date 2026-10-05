# engine/platform/CMakeLists.txt — the C-ABI seam; EXACTLY ONE backend links per build
# (docs/02 §1). There is no runtime platform branching — the linker picks the one translation
# unit that exports the seam.
#   gba/psp -> the native console backend.
#   desktop -> the headless `null` backend (framebuffer + scripted input + file I/O) by default
#              so a build always succeeds and the test suite runs without a display; pass
#              -DPHX_USE_SDL=ON for the real SDL2 window/input/audio backend.
add_library(phx_platform STATIC)
target_include_directories(phx_platform PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(phx_platform PUBLIC phx_core)

option(PHX_USE_SDL "Desktop: link the SDL2 platform backend (window/input/audio device)" OFF)

if(PHX_TARGET STREQUAL "gba")
  target_sources(phx_platform PRIVATE src/gba/gba_platform.cpp)
  set(_platform_backend "gba")
elseif(PHX_TARGET STREQUAL "psp")
  target_sources(phx_platform PRIVATE src/psp/psp_platform.cpp)
  set(_platform_backend "psp")
elseif(PHX_USE_SDL)
  find_package(SDL2 REQUIRED)
  target_sources(phx_platform PRIVATE src/sdl/sdl_platform.cpp)
  target_compile_definitions(phx_platform PRIVATE PHX_HAVE_SDL=1)
  target_link_libraries(phx_platform PUBLIC SDL2::SDL2)
  set(_platform_backend "sdl")
else()
  target_sources(phx_platform PRIVATE src/null/null_platform.cpp)
  set(_platform_backend "null (headless)")
endif()

message(STATUS "Phosphorus platform backend: ${_platform_backend}")
