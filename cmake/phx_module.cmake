# cmake/phx_module.cmake — keeps every engine module's CMakeLists identical and tiny,
# and guarantees the linker gets exactly ONE platform/render backend.
#
# Usage:
#   phx_add_module(render
#     SRC      src/renderer.cpp
#     DEPS     phx_core phx_memory phx_platform
#     BACKENDS gl:tier2 gu:tier1 gba:tier0 soft:tier2)   # backend:<select-key>
#
# A backend compiles only when its select-key matches PHX_TARGET or PHX_RENDER_TIER.

function(phx_add_module name)
  cmake_parse_arguments(M "" "" "SRC;DEPS;BACKENDS" ${ARGN})

  add_library(phx_${name} STATIC ${M_SRC})
  # BUILD_INTERFACE/INSTALL_INTERFACE split so the target is exportable: installed consumers
  # (find_package(phosphorus), see the root CMakeLists' install section) resolve headers from
  # <prefix>/include instead of this source tree.
  target_include_directories(phx_${name} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_compile_features(phx_${name} PUBLIC cxx_std_17)
  if(M_DEPS)
    target_link_libraries(phx_${name} PUBLIC ${M_DEPS})
  endif()

  foreach(spec ${M_BACKENDS})
    string(REPLACE ":" ";" parts ${spec})
    list(GET parts 0 bname)
    list(LENGTH parts plen)
    set(key "")
    if(plen GREATER 1)
      list(GET parts 1 key)
    endif()
    # select backend by exact target name (e.g. "gba") or by render tier (e.g. "tier2")
    if(bname STREQUAL PHX_TARGET OR key STREQUAL "tier${PHX_RENDER_TIER}" OR bname STREQUAL "${PHX_TARGET}")
      file(GLOB _bsrc CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${bname}/*.cpp
                                        ${CMAKE_CURRENT_SOURCE_DIR}/src/${bname}/*.c)
      if(_bsrc)
        target_sources(phx_${name} PRIVATE ${_bsrc})
        message(STATUS "  phx_${name}: backend '${bname}' selected")
      endif()
    endif()
  endforeach()
endfunction()
