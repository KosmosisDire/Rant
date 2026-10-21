# Rant built to wasm with Emscripten. rant_web is the C library over platform.c, which a C or
# C++ app links, with rant_web.js its JavaScript side. rant_wasm is the same library as a
# module of its own, which the TypeScript binding in src/ drives. npm compiles src/ into dist/
# in Rant's own build only: a project that fetched Rant takes the release's web package.

find_program(RANT_NPM NAMES npm.cmd npm)

if(RANT_NPM AND RANT_IS_TOP_LEVEL)
  set(_web_js "${CMAKE_CURRENT_SOURCE_DIR}/dist")
  file(GLOB _web_ts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.ts" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.mts")
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/.package-lock.json"
    COMMAND "${RANT_NPM}" ci --no-audit --no-fund
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/package-lock.json"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
  add_custom_command(
    OUTPUT "${_web_js}/rant.js"
    COMMAND "${RANT_NPM}" run build
    DEPENDS ${_web_ts} "${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/.package-lock.json"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
  add_custom_target(web_js DEPENDS "${_web_js}/rant.js")
endif()

if(EMSCRIPTEN)
  enable_language(CXX)

  set(_web_defs RANT_PLAT_CUSTOM RANT_NO_THREADS RANT_NO_SHM RANT_NO_PROC_STATS)
  add_library(rant_web STATIC "${PROJECT_SOURCE_DIR}/dist/rant.c" platform.c)
  add_library(rant::rant_web ALIAS rant_web)
  add_dependencies(rant_web rant_dist)
  target_link_libraries(rant_web PUBLIC rant)
  target_include_directories(rant_web PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  # One thread, no shared memory, no process stats, and platform.c is the OS layer. Public,
  # since the headers an app includes must agree with the library.
  target_compile_definitions(rant_web PUBLIC ${_web_defs})
  target_link_options(rant_web INTERFACE "--js-library=${CMAKE_CURRENT_SOURCE_DIR}/rant_web.js")
  set_property(TARGET rant_web APPEND PROPERTY INTERFACE_LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/rant_web.js")

  # The module the TypeScript binding loads: every RANT_API function exported, and the
  # pieces of the runtime the binding reads memory and makes callbacks with.
  add_executable(rant_wasm "${PROJECT_SOURCE_DIR}/dist/rant.c" platform.c)
  add_dependencies(rant_wasm rant_dist)
  target_link_libraries(rant_wasm PRIVATE rant)
  target_include_directories(rant_wasm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(rant_wasm PRIVATE ${_web_defs} RANT_BUILD_SHARED)
  set_target_properties(rant_wasm PROPERTIES SUFFIX ".mjs" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist"
                        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/rant_web.js")
  target_link_options(rant_wasm PRIVATE
    --no-entry "--js-library=${CMAKE_CURRENT_SOURCE_DIR}/rant_web.js"
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createRant -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1 -sALLOW_TABLE_GROWTH=1 -sWASM_BIGINT=1
    -sEXPORTED_FUNCTIONS=_malloc,_free
    -sEXPORTED_RUNTIME_METHODS=addFunction,HEAPU8)

  # The struct offsets the binding reads, from layout.c built for wasm32: layout.mjs writes
  # them into src/layout.ts, and the test checks that file against this build.
  add_executable(rant_web_layout "${PROJECT_SOURCE_DIR}/tests/support/layout.c")
  target_link_libraries(rant_web_layout PRIVATE rant_web)
  set_target_properties(rant_web_layout PROPERTIES SUFFIX ".cjs" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_options(rant_web_layout PRIVATE -sENVIRONMENT=node)

  add_executable(rant_web_bench bench/bench.cpp)
  target_compile_features(rant_web_bench PRIVATE cxx_std_17)
  target_link_libraries(rant_web_bench PRIVATE rant_web)
  set_target_properties(rant_web_bench PROPERTIES SUFFIX ".mjs"
                        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin/web")
  target_link_options(rant_web_bench PRIVATE
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createBench -sENVIRONMENT=web,node
    -sALLOW_MEMORY_GROWTH=1)
  # function names in the wasm, so a browser profile shows where the time goes
  option(RANT_WEB_PROFILE "Keep function names in the bench wasm" OFF)
  if(RANT_WEB_PROFILE)
    target_link_options(rant_web_bench PRIVATE --profiling-funcs)
  endif()

  find_program(RANT_NODE node)
  if(TARGET web_js)
    add_custom_target(web_all ALL DEPENDS web_js rant_wasm rant_web_layout)
    add_custom_target(web_layout
      COMMAND "${RANT_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/layout.mjs" "$<TARGET_FILE:rant_web_layout>"
      DEPENDS rant_web_layout VERBATIM)

    # The npm package a release attaches: the binding with its wasm and the net objects, and
    # under c/ what an Emscripten app compiles, the single headers included.
    set(_web_stage "${CMAKE_CURRENT_BINARY_DIR}/package")
    add_custom_target(web_package
      COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_web_stage}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_web_stage}/c"
      COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/package.json" "${PROJECT_SOURCE_DIR}/LICENCE.md" "${_web_stage}"
      COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_web_js}" "${_web_stage}/dist"
      COMMAND "${CMAKE_COMMAND}" -E copy
              "${CMAKE_CURRENT_SOURCE_DIR}/platform.c" "${CMAKE_CURRENT_SOURCE_DIR}/rant_web.h"
              "${CMAKE_CURRENT_SOURCE_DIR}/rant_web.js" "${PROJECT_SOURCE_DIR}/dist/rant.c"
              "${PROJECT_SOURCE_DIR}/dist/rant.h" "${_web_stage}/c"
      COMMAND "${CMAKE_COMMAND}" -E chdir "${_web_stage}" "${RANT_NPM}" pack --pack-destination "${PROJECT_SOURCE_DIR}/dist"
      VERBATIM)
    add_dependencies(web_package web_all)

    if(RANT_NODE)
      add_test(NAME web COMMAND "${RANT_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/test.mjs")
      add_test(NAME web.layout COMMAND "${RANT_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/layout.mjs" "$<TARGET_FILE:rant_web_layout>" --check)
      add_test(NAME web.binding COMMAND "${RANT_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/binding.mjs")
      add_test(NAME web.proof COMMAND "${RANT_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/proof.mjs")
      set_tests_properties(web web.layout web.binding web.proof PROPERTIES LABELS web TIMEOUT 120)
    endif()
  endif()
elseif(RANT_BUILD_TOOLS AND CMAKE_CXX_COMPILER)
  add_executable(rant_web_bench bench/bench.cpp "${PROJECT_SOURCE_DIR}/dist/rant.cpp")
  target_compile_features(rant_web_bench PRIVATE cxx_std_17)
  target_link_libraries(rant_web_bench PRIVATE rant rant_platform)
  add_dependencies(rant_web_bench rant_dist)
endif()
