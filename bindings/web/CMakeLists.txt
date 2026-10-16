# Rant built to wasm with Emscripten: rant_web is the C library over platform.c, which a C
# or C++ app links, with net.js its JavaScript side. The bench is built here too, and in a
# native build against the same sources, so both sides of a comparison run the same code.

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
  target_link_options(rant_web INTERFACE "--js-library=${CMAKE_CURRENT_SOURCE_DIR}/net.js")
  set_property(TARGET rant_web APPEND PROPERTY INTERFACE_LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/net.js")

  add_executable(rant_web_bench bench/bench.cpp)
  target_compile_features(rant_web_bench PRIVATE cxx_std_17)
  target_link_libraries(rant_web_bench PRIVATE rant_web)
  set_target_properties(rant_web_bench PROPERTIES SUFFIX ".mjs"
                        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin/web")
  target_link_options(rant_web_bench PRIVATE
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createBench -sENVIRONMENT=web,node
    -sALLOW_MEMORY_GROWTH=1)
else()
  if(NOT CMAKE_CXX_COMPILER)
    return()
  endif()
  add_executable(rant_web_bench bench/bench.cpp "${PROJECT_SOURCE_DIR}/dist/rant.cpp")
  target_compile_features(rant_web_bench PRIVATE cxx_std_17)
  target_link_libraries(rant_web_bench PRIVATE rant rant_platform)
  add_dependencies(rant_web_bench rant_dist)
endif()
