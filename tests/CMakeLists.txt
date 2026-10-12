# tests/CMakeLists.txt — the host test suites, registered with CTest (run with `ctest`).
# Mirrors `make check`: one unit runner (self-registering PHX_TESTs) + the headless
# integration binaries, each linking the engine module libraries. This is the default
# (headless) configuration: null platform + software render backend — no display needed.
#
# Note: the GBA-PPU (`make ppu`) and PSP-GU (`make gu`) host-model checks deliberately swap
# the render backend, which is incompatible with CMake's one-backend-per-build configure; they
# are compiled here as standalone executables (front end + the one backend, no phx_render lib)
# so `ctest` covers the same ground as `make check`.

set(_phxpack_inc ${CMAKE_SOURCE_DIR}/tools/phxpack)
set(_render_src  ${CMAKE_SOURCE_DIR}/engine/render/src)
# builders.h -> analyze.h (tools/phxviz) -> viz.h (examples/miracle-player): the bake path is
# shared, so anything including builders.h (or the viz format directly) needs these on its path.
set(_viz_inc ${CMAKE_SOURCE_DIR}/tools/phxviz ${CMAKE_SOURCE_DIR}/examples/miracle-player/src)
# tinyllm's fixed-point core, blob format and host-side blob builder (unit + integration tests).
set(_tinyllm_inc ${CMAKE_SOURCE_DIR}/examples/tinyllm/src)

# Several integration tests bake a bundle to "build/<name>.phxp" (a path relative to the cwd)
# and mount it back. Run every test from the CMake build tree and give it that "build/" subdir,
# so artifacts stay inside the build directory instead of the source tree.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/build)
set(_test_wd ${CMAKE_BINARY_DIR})

# ---- unit suite (foundation) ----------------------------------------------
add_executable(phx_tests
  unit/main.cpp
  unit/test_fixed.cpp unit/test_math.cpp unit/test_memory.cpp unit/test_ecs.cpp unit/test_time.cpp
  unit/test_input.cpp unit/test_physics.cpp unit/test_anim.cpp unit/test_scene.cpp
  unit/test_ui.cpp unit/test_audio.cpp unit/test_stream.cpp unit/test_lz.cpp
  unit/test_png.cpp unit/test_json.cpp unit/test_wav.cpp unit/test_cmdqueue.cpp
  unit/test_viz.cpp
  unit/test_particles.cpp
  unit/test_tinyllm_math.cpp
  unit/test_tinyllm_format.cpp
  unit/test_tinyllm_tokenizer.cpp
  unit/test_tinyllm_font.cpp
  unit/test_version.cpp
  ${CMAKE_SOURCE_DIR}/examples/tinyllm/src/llm.cpp
)
target_include_directories(phx_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${_phxpack_inc} ${_viz_inc} ${_tinyllm_inc})
target_link_libraries(phx_tests PRIVATE
  phx_core phx_memory phx_ecs phx_physics phx_anim phx_scene phx_audio phx_input
  phx_ui phx_resource)
add_test(NAME unit COMMAND phx_tests WORKING_DIRECTORY ${_test_wd})

# ---- headless integration binaries ----------------------------------------
# The full gameplay/render/resource stack on the null platform + software backend. Linking the
# superset of module libs is harmless — the static linker pulls only the members each test uses.
set(_game_libs
  phx_runtime phx_scene phx_physics phx_anim phx_ui
  phx_render phx_ecs phx_input phx_resource phx_audio
  phx_platform phx_memory phx_core)

# name -> source file
set(_itests
  smoke:smoke_app
  render:render_test
  playable:playable_test
  physics:physics_test
  anim:anim_test
  scene:scene_test
  ui:ui_test
  platformer:platformer_test
  emberwing:emberwing_test
  miracle:miracle_test
  tinyllm:tinyllm_test
  audio:audio_test
  texcache:texcache_test
  png:png_test
  sprite:sprite_test
  tiled:tiled_test
  resource:resource_test
  pipeline:pipeline_test
  editors:editors_test
  level:level_test
  behaviours:behaviours_test
  flow:flow_test
  dialogue:dialogue_test)

foreach(spec ${_itests})
  string(REPLACE ":" ";" parts ${spec})
  list(GET parts 0 tname)
  list(GET parts 1 tsrc)
  add_executable(it_${tname} suites/${tsrc}.cpp)
  target_include_directories(it_${tname} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${_phxpack_inc}
    ${_viz_inc}                                     # builders.h -> analyze.h -> viz.h
    ${_tinyllm_inc}                                 # tinyllm_test: llm.h / llm_build.h / bake.h
    ${CMAKE_SOURCE_DIR}/tools/common                # bake.h -> debug_font.h
    ${CMAKE_SOURCE_DIR}/tools/phxtmap               # pipeline_test: the phxtmap editor model
    ${CMAKE_SOURCE_DIR}/examples/platformer/src)   # platformer_test: components.h / bake.h
  target_link_libraries(it_${tname} PRIVATE ${_game_libs})
  add_test(NAME ${tname} COMMAND it_${tname} WORKING_DIRECTORY ${_test_wd})
endforeach()

# The platformer integration test drives the real example game object, so it also needs the
# example's gameplay translation unit compiled in (the rest of the example is header-only).
target_sources(it_platformer PRIVATE ${CMAKE_SOURCE_DIR}/examples/platformer/src/systems.cpp)
target_include_directories(it_platformer PRIVATE ${CMAKE_SOURCE_DIR}/engine/resource/include)

# The editors suite also covers the Studio's TrueType text (twk.h's native-resolution path over the
# null backend's overlay), so it compiles in the rasterizer (stb_truetype + the embedded face).
target_sources(it_editors PRIVATE ${CMAKE_SOURCE_DIR}/tools/common/ttf_text.cpp)

# Same deal for Emberwing: the test drives the real EmberwingGame, so its systems/scenes TUs
# (the rest of the example is header-only) must be compiled in too. Mirrors `EMBERWING_SRC` in
# the Makefile, minus the render backend (this target uses the default soft/null build, same
# as every other headless integration test above).
target_sources(it_emberwing PRIVATE
  ${CMAKE_SOURCE_DIR}/examples/emberwing/src/systems.cpp
  ${CMAKE_SOURCE_DIR}/examples/emberwing/src/scenes.cpp)
target_include_directories(it_emberwing PRIVATE ${CMAKE_SOURCE_DIR}/engine/resource/include)

# The miracle visualizer test drives the real MiracleGame, so compile in its logic TU (the rest
# of the example is header-only) and put the example + viz/tool headers on its include path.
target_sources(it_miracle PRIVATE ${CMAKE_SOURCE_DIR}/examples/miracle-player/src/miracle.cpp)
target_include_directories(it_miracle PRIVATE
  ${CMAKE_SOURCE_DIR}/examples/miracle-player/src ${CMAKE_SOURCE_DIR}/engine/resource/include)

# The tinyllm test drives the real TinyLlmGame and the inference core, so compile in both TUs
# (the rest of the example is header-only). Its golden token file lives in the SOURCE tree while
# CTest runs from the build tree, so the path is passed in rather than assumed.
target_sources(it_tinyllm PRIVATE
  ${CMAKE_SOURCE_DIR}/examples/tinyllm/src/llm.cpp
  ${CMAKE_SOURCE_DIR}/examples/tinyllm/src/tinyllm.cpp)
target_include_directories(it_tinyllm PRIVATE ${CMAKE_SOURCE_DIR}/engine/resource/include)
target_compile_definitions(it_tinyllm PRIVATE
  TINYLLM_GOLDEN_PATH="${CMAKE_SOURCE_DIR}/tests/fixtures/tinyllm_golden.txt")

# pipeline_test is in-process; it also drops author-source fixtures into build/. The CLI smoke
# below then runs the REAL converter + assembler binaries over those fixtures (proving the
# executables link/parse/run), so it must run after pipeline.
add_test(NAME tools_cli
  COMMAND ${CMAKE_COMMAND}
    -DPHXSPRITE=$<TARGET_FILE:phxsprite> -DPHXTILE=$<TARGET_FILE:phxtile>
    -DPHXSND=$<TARGET_FILE:phxsnd>       -DPHXBIN=$<TARGET_FILE:phxbin>
    -DPHXPACK=$<TARGET_FILE:phxpack>     -DWD=${CMAKE_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_tools.cmake
  WORKING_DIRECTORY ${_test_wd})
set_tests_properties(tools_cli PROPERTIES DEPENDS pipeline)

# ---- console-native render backends, host-model verification --------------
# Compiled directly (front end + the chosen backend, no phx_render) so both backends can coexist
# in one build tree without colliding on phx_make_render_backend.
foreach(spec ppu:gba/gba_ppu gu:gu/gu_backend)
  string(REPLACE ":" ";" parts ${spec})
  list(GET parts 0 tname)
  list(GET parts 1 bsrc)
  add_executable(it_${tname}
    ${CMAKE_SOURCE_DIR}/engine/core/src/assert.cpp
    ${CMAKE_SOURCE_DIR}/engine/core/src/fixed.cpp
    ${CMAKE_SOURCE_DIR}/engine/core/src/log.cpp
    ${CMAKE_SOURCE_DIR}/engine/memory/src/memory_root.cpp
    ${CMAKE_SOURCE_DIR}/engine/platform/src/null/null_platform.cpp
    ${_render_src}/renderer.cpp
    ${_render_src}/${bsrc}.cpp
    ${_render_src}/texture_cache.cpp
    suites/${tname}_test.cpp)
  target_include_directories(it_${tname} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine/core/include
    ${CMAKE_SOURCE_DIR}/engine/memory/include
    ${CMAKE_SOURCE_DIR}/engine/platform/include
    ${CMAKE_SOURCE_DIR}/engine/render/include
    ${_phxpack_inc}                                        # tex_encode.h (bake==upload proofs)
    ${_render_src} ${_render_src}/gba ${_render_src}/gu)   # ppu_model.h / gu_model.h
  add_test(NAME ${tname} COMMAND it_${tname} WORKING_DIRECTORY ${_test_wd})
endforeach()

# ---- Emberwing on the native PPU backend (the shipping GBA render path) ---
# `make emberwing-ppu` re-links EMBERWING_SRC with gba_ppu.cpp swapped in for the soft
# rasterizer — the SAME headless game/test TU, just proving it renders identically through
# the PPU model. That backend swap is incompatible with linking phx_render (soft by default),
# so — like ppu/gu above — this compiles every module directly from source instead of linking
# the phx_* libs, guaranteeing the PPU backend (not whatever phx_render/phx_platform would
# otherwise select) is the one under test.
add_executable(it_emberwing_ppu
  ${CMAKE_SOURCE_DIR}/engine/core/src/assert.cpp
  ${CMAKE_SOURCE_DIR}/engine/core/src/fixed.cpp
  ${CMAKE_SOURCE_DIR}/engine/core/src/log.cpp
  ${CMAKE_SOURCE_DIR}/engine/memory/src/memory_root.cpp
  ${CMAKE_SOURCE_DIR}/engine/ecs/src/world.cpp
  ${_render_src}/renderer.cpp
  ${_render_src}/gba/gba_ppu.cpp
  ${CMAKE_SOURCE_DIR}/engine/physics/src/physics.cpp
  ${CMAKE_SOURCE_DIR}/engine/anim/src/anim.cpp
  ${CMAKE_SOURCE_DIR}/engine/scene/src/scene.cpp
  ${CMAKE_SOURCE_DIR}/engine/ui/src/ui.cpp
  ${CMAKE_SOURCE_DIR}/engine/platform/src/null/null_platform.cpp
  ${CMAKE_SOURCE_DIR}/engine/runtime/src/app.cpp
  ${CMAKE_SOURCE_DIR}/engine/resource/src/cache.cpp
  ${CMAKE_SOURCE_DIR}/engine/audio/src/mixer.cpp
  ${CMAKE_SOURCE_DIR}/examples/emberwing/src/systems.cpp
  ${CMAKE_SOURCE_DIR}/examples/emberwing/src/scenes.cpp
  suites/emberwing_test.cpp)
target_include_directories(it_emberwing_ppu PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${_phxpack_inc}
  ${CMAKE_SOURCE_DIR}/tools/common
  ${CMAKE_SOURCE_DIR}/engine/core/include
  ${CMAKE_SOURCE_DIR}/engine/memory/include
  ${CMAKE_SOURCE_DIR}/engine/ecs/include
  ${CMAKE_SOURCE_DIR}/engine/input/include
  ${CMAKE_SOURCE_DIR}/engine/audio/include
  ${CMAKE_SOURCE_DIR}/engine/platform/include
  ${CMAKE_SOURCE_DIR}/engine/render/include
  ${_render_src} ${_render_src}/gba
  ${CMAKE_SOURCE_DIR}/engine/resource/include
  ${CMAKE_SOURCE_DIR}/engine/physics/include
  ${CMAKE_SOURCE_DIR}/engine/anim/include
  ${CMAKE_SOURCE_DIR}/engine/scene/include
  ${CMAKE_SOURCE_DIR}/engine/ui/include
  ${CMAKE_SOURCE_DIR}/engine/runtime/include)
add_test(NAME emberwing-ppu COMMAND it_emberwing_ppu WORKING_DIRECTORY ${_test_wd})
