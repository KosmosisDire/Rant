# DART: Discovery And Realtime Transport -- build coordinator.
#
# Two things happen here:
#   1. Amalgamate the split src/ layers into the dist/ single-headers
#      (tools/pack.cmake, a pure-CMake port of the old tools/pack.c -- no
#      separate amalgamator to compile).
#   2. Build the host tools, tests and examples against dist/.
#
# Quick start (any generator):
#   cmake -S . -B build
#   cmake --build build
#
# Targets: dart_test, dart_test_noshm, example, ideal, pubsub, collide,
# if_probe_check, plus `dist` (regenerate the single-headers).
#
# Porting to a new platform (Arduino, ESP32, VxWorks, ...): such targets don't
# build the host programs -- they just drop a dist/ header into their own
# project. Point their build at the header-only `dart` target (or the dist/
# folder) and set -DDART_BUILD_TOOLS=OFF. Cross builds default it OFF already.

cmake_minimum_required(VERSION 3.15)
project(dart C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Drop the built executables in the repo root so they're easy to run in place
# (the build tree stays wherever you configured it, e.g. build/).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# 1. Amalgamate src/ -> dist/. Re-runs only when a src file (or the packer)
#    changes; CONFIGURE_DEPENDS reglobs so new src files are picked up.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE DART_SRC CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

set(DART_DIST
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/dart_discovery.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/dart_transport.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/dart.h")

add_custom_command(
  OUTPUT  ${DART_DIST}
  COMMAND "${CMAKE_COMMAND}"
          -D "SRC=${CMAKE_CURRENT_SOURCE_DIR}/src"
          -D "OUT=${CMAKE_CURRENT_SOURCE_DIR}/dist"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack.cmake"
  DEPENDS ${DART_SRC} "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack.cmake"
  COMMENT "DART: amalgamating src/ -> dist/ single-headers"
  VERBATIM)

add_custom_target(dist ALL DEPENDS ${DART_DIST})

# ---------------------------------------------------------------------------
# 2. The portable core: header-only, just the dist/ include dir. This is the
#    one target a cross / embedded build consumes -- link it and
#    #include "dart.h" with DART_IMPLEMENTATION in one TU. No sources and no
#    platform libs live here (the consumer's own toolchain provides those), so
#    a new platform adds nothing to this file.
# ---------------------------------------------------------------------------
add_library(dart INTERFACE)
add_dependencies(dart dist)
target_include_directories(dart INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/dist")

# ---------------------------------------------------------------------------
# 3. Host tools, tests and examples. They use sockets/threads/timers, so they
#    are desktop-only (Windows + POSIX today).
# ---------------------------------------------------------------------------
if(CMAKE_CROSSCOMPILING)
  set(_dart_tools_default OFF)
else()
  set(_dart_tools_default ON)
endif()
option(DART_BUILD_TOOLS "Build the DART host tools, tests and examples" ${_dart_tools_default})

if(DART_BUILD_TOOLS)
  # Platform link libraries layered on top of the header-only core.
  add_library(dart_host INTERFACE)
  target_link_libraries(dart_host INTERFACE dart)

  find_package(Threads REQUIRED)
  target_link_libraries(dart_host INTERFACE Threads::Threads)

  if(WIN32)
    # ws2_32: sockets   bcrypt: entropy   winmm: timeBeginPeriod (dart_test)
    target_link_libraries(dart_host INTERFACE ws2_32 bcrypt winmm)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(dart_host INTERFACE rt)   # shm_open for the SHM fast path
  endif()

  # One host program from one source file: link the core + platform libs and
  # wait for dist/ to be (re)generated first.
  function(dart_program name src)
    add_executable(${name} "${src}")
    target_link_libraries(${name} PRIVATE dart_host)
    add_dependencies(${name} dist)
  endfunction()

  dart_program(dart_test      tests/dart_test.c)
  dart_program(example        examples/example.c)
  dart_program(pubsub         tools/pubsub.c)
  dart_program(if_probe_check tools/if_probe_check.c)

  # SHM-disabled test build (matches the dart_test_noshm in the tree).
  dart_program(dart_test_noshm tests/dart_test.c)
  target_compile_definitions(dart_test_noshm PRIVATE DART_NO_SHM)
endif()

# ---------------------------------------------------------------------------
# 4. The DART Explorer GUI (explore/). A sizable project in its own right, so it
#    keeps its own CMakeLists; we just pull it in so it builds alongside the host
#    programs and drops dart_explorer in the repo root like the rest. OFF by
#    default: it fetches and builds SDL3 + SDL3_ttf from source (network + a slow
#    first build), which the core/tools build has no need for. Enable with
#    -DDART_BUILD_EXPLORER=ON. When built this way it consumes the `dart` target
#    above (dist include + regeneration); it also still builds standalone
#    (cmake -S explore -B explore/build).
# ---------------------------------------------------------------------------
option(DART_BUILD_EXPLORER "Build the DART Explorer GUI (fetches SDL3)" OFF)
if(DART_BUILD_EXPLORER)
  add_subdirectory(explore)
endif()
