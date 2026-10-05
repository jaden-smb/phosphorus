# cmake/caps_select.cmake — maps PHX_TARGET to compile defines + render tier.
# These feed phx/core/caps.h (the compile-time capability tier).

if(PHX_TARGET STREQUAL "gba")
  add_compile_definitions(PHX_TARGET_GBA=1)
  set(PHX_RENDER_TIER 0 CACHE INTERNAL "tile/sprite PPU")
elseif(PHX_TARGET STREQUAL "psp")
  add_compile_definitions(PHX_TARGET_PSP=1)
  set(PHX_RENDER_TIER 1 CACHE INTERNAL "fixed-function GU")
elseif(PHX_TARGET STREQUAL "windows")
  add_compile_definitions(PHX_TARGET_PC=1 PHX_TARGET_WINDOWS=1)
  set(PHX_RENDER_TIER 2 CACHE INTERNAL "programmable GL/VK")
else() # linux (default)
  add_compile_definitions(PHX_TARGET_PC=1 PHX_TARGET_LINUX=1)
  set(PHX_RENDER_TIER 2 CACHE INTERNAL "programmable GL/VK")
endif()

message(STATUS "Phosphorus render tier: ${PHX_RENDER_TIER}")
