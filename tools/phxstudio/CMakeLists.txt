# tools/phxstudio/CMakeLists.txt — Phosphorus Studio and its standalone editors (host-only).
#   phxnew     always: the new-project CLI (the Studio's File > New project, headless; no SDL)
#   phxstudio  with -DPHX_USE_SDL=ON: the editor (a window through the engine's own SDL platform
#   phxtmap    backend and software renderer: the engine's desktop link, like `make studio`,
#   phxentity  `make tmap`, `make entity`)
# The Studio's document models are covered by the editors + pipeline suites (tests/), which build
# without a display; these targets are the GUI shells over them.
set(_studio_inc
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/tools/phxpack          # json.h, png.h, synth.h, font.h, dialogue.h, tex_encode.h
  ${CMAKE_SOURCE_DIR}/tools/common           # twk.h (the widget kit), ttf_text.h, png_write.h, ascii_font.h
  ${CMAKE_SOURCE_DIR}/tools/phxtmap          # editor.h: the tilemap document model
  ${CMAKE_SOURCE_DIR}/tools/phxentity        # editor.h: the data-table document model
  ${CMAKE_SOURCE_DIR}/tools/phxviz)          # builders.h -> analyze.h (header-only)

add_executable(phxnew new_main.cpp)
target_include_directories(phxnew PRIVATE ${_studio_inc})
target_link_libraries(phxnew PRIVATE phx_core phx_resource)
target_compile_features(phxnew PRIVATE cxx_std_17)

if(PHX_USE_SDL)
  find_package(Threads REQUIRED)             # the job runner (jobs.h) streams launches on a thread
  set(_gui_libs phx_runtime phx_ui phx_render phx_input phx_audio phx_resource phx_ecs
                phx_platform phx_memory phx_core Threads::Threads)

  set(_studio_src main.cpp workspace.cpp ed_code.cpp ed_sprite.cpp ed_map.cpp ed_table.cpp
                  ed_sfx.cpp ed_song.cpp ed_font.cpp ed_dialogue.cpp
                  ${CMAKE_SOURCE_DIR}/tools/common/ttf_text.cpp)   # the Studio's TrueType text (stb_truetype)
  if(WIN32)
    list(APPEND _studio_src winjob.cpp)      # launches in a Job Object (tools/phxstudio/winjob.h)
  endif()
  add_executable(phxstudio ${_studio_src})
  target_include_directories(phxstudio PRIVATE ${_studio_inc})
  target_link_libraries(phxstudio PRIVATE ${_gui_libs})
  target_compile_features(phxstudio PRIVATE cxx_std_17)

  add_executable(phxtmap ${CMAKE_SOURCE_DIR}/tools/phxtmap/main.cpp ed_map.cpp)
  target_include_directories(phxtmap PRIVATE ${_studio_inc})
  target_link_libraries(phxtmap PRIVATE ${_gui_libs})
  target_compile_features(phxtmap PRIVATE cxx_std_17)

  add_executable(phxentity ${CMAKE_SOURCE_DIR}/tools/phxentity/main.cpp ed_table.cpp)
  target_include_directories(phxentity PRIVATE ${_studio_inc})
  target_link_libraries(phxentity PRIVATE ${_gui_libs})
  target_compile_features(phxentity PRIVATE cxx_std_17)
  message(STATUS "Phosphorus Studio: phxstudio, phxtmap, phxentity (SDL2)")
else()
  message(STATUS "Phosphorus Studio: phxnew only (configure with -DPHX_USE_SDL=ON for phxstudio / phxtmap / phxentity)")
endif()
