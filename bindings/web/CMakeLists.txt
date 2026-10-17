# Rant built to wasm with Emscripten: rant_web is the C library over platform.c, which a C or
# C++ app links, with rant_web.js its JavaScript side. The net objects are TypeScript in src/,
# which npm compiles into dist/ in Rant's own build only: a project that fetched Rant takes
# them from the release's web package. The bench is built here too, and in a native build
# against the same sources, so both sides of a comparison run the same code.

find_program(RANT_NPM NAMES npm.cmd npm)

if(RANT_NPM AND RANT_IS_TOP_LEVEL)
  set(_web_js "${CMAKE_CURRENT_SOURCE_DIR}/dist")
  file(GLOB _web_ts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.ts")
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/.package-lock.json"
    COMMAND "${RANT_NPM}" ci --no-audit --no-fund
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/package-lock.json"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
  add_custom_command(
    OUTPUT "${_web_js}/net.js" "${_web_js}/udp.js" "${_web_js}/tunnel.js"
    COMMAND "${RANT_NPM}" run build
    DEPENDS ${_web_ts} "${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/.package-lock.json"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
  # part of every web build, which tests with it, and of a native one only for the package
  if(EMSCRIPTEN)
    set(_web_all ALL)
  endif()
  add_custom_target(web_js ${_web_all} DEPENDS "${_web_js}/net.js" "${_web_js}/udp.js" "${_web_js}/tunnel.js")

  # The npm package a release attaches: the net objects, and under c/ what an Emscripten app
  # compiles, the single headers included.
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
  add_dependencies(web_package web_js rant_dist)
endif()

if(EMSCRIPTEN)
  enable_language(CXX)

  add_library(rant_web STATIC "${PROJECT_SOURCE_DIR}/dist/rant.c" platform.c)
  add_library(rant::rant_web ALIAS rant_web)
  add_dependencies(rant_web rant_dist)
  target_link_libraries(rant_web PUBLIC rant)
  target_include_directories(rant_web PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  # One thread, no shared memory, no process stats, and platform.c is the OS layer. Public,
  # since the headers an app includes must agree with the library.
  target_compile_definitions(rant_web PUBLIC RANT_PLAT_CUSTOM RANT_NO_THREADS RANT_NO_SHM RANT_NO_PROC_STATS)
  target_link_options(rant_web INTERFACE "--js-library=${CMAKE_CURRENT_SOURCE_DIR}/rant_web.js")
  set_property(TARGET rant_web APPEND PROPERTY INTERFACE_LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/rant_web.js")

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
  if(RANT_NODE AND TARGET web_js)
    add_test(NAME web COMMAND "${RANT_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/test.mjs")
    set_tests_properties(web PROPERTIES LABELS web TIMEOUT 120)
  endif()
elseif(RANT_BUILD_TOOLS AND CMAKE_CXX_COMPILER)
  add_executable(rant_web_bench bench/bench.cpp "${PROJECT_SOURCE_DIR}/dist/rant.cpp")
  target_compile_features(rant_web_bench PRIVATE cxx_std_17)
  target_link_libraries(rant_web_bench PRIVATE rant rant_platform)
  add_dependencies(rant_web_bench rant_dist)
endif()
