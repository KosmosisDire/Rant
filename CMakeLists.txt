# DART build: amalgamate src/ -> dist/ single-headers, then build the host
# tools/tests/examples against dist/. Quick start: cmake -S . -B build && cmake
# --build build (or use the OS presets in CMakePresets.json). Embedded targets just
# link the header-only `dart` target and set -DDART_BUILD_TOOLS=OFF (auto-off cross).

cmake_minimum_required(VERSION 3.15)
project(dart C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Static CRT: binaries must run on a machine with no toolchain and no VC++
# redistributable. Must come before any target or fetched subproject.
include("${CMAKE_CURRENT_SOURCE_DIR}/tools/static_runtime.cmake")

# Multi-config: default a bare build to Release (Ninja MC only; VS rejects the var
# and its CLI hardcodes Debug, so the Windows build preset pins Release instead).
if(CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_CONFIGURATION_TYPES "Release;Debug;RelWithDebInfo;MinSizeRel"
      CACHE STRING "Build configs (Release first = the default)" FORCE)
  if(CMAKE_GENERATOR STREQUAL "Ninja Multi-Config")
    set(CMAKE_DEFAULT_BUILD_TYPE Release CACHE STRING "Config a bare multi-config build uses")
  else()
    unset(CMAKE_DEFAULT_BUILD_TYPE CACHE)
  endif()
endif()

# All executables land in bin/, whatever the config. Multi-config generators would
# otherwise append a per-config subdir (bin/Release, bin/Debug), so pin every config
# to bin/ directly: switching Release<->Debug overwrites the same files in place.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
foreach(_cfg ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${_cfg}" _CFG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_SOURCE_DIR}/bin")
endforeach()

# 1. Amalgamate src/ -> dist/. Re-runs only when a src file or the packer changes.
file(GLOB_RECURSE DART_SRC CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

set(DART_DIST
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/dart_discovery.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/dart_transport.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/dart.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/dart.hpp"     # C++ wrapper with dart.h embedded
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/dart.py")     # Python wrapper with dart.h embedded
    # (the C# wrapper csharp/Dart.cs is hand-written over the prebuilt native lib, not generated)

add_custom_command(
  OUTPUT  ${DART_DIST}
  COMMAND "${CMAKE_COMMAND}"
          -D "SRC=${CMAKE_CURRENT_SOURCE_DIR}/src"
          -D "OUT=${CMAKE_CURRENT_SOURCE_DIR}/dist"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack.cmake"
  DEPENDS ${DART_SRC} "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/cpp/dart.hpp"
          "${CMAKE_CURRENT_SOURCE_DIR}/python/dart.py.in"
  COMMENT "DART: amalgamating src/ -> dist/ single-headers"
  VERBATIM)

add_custom_target(dist ALL DEPENDS ${DART_DIST})

# 2. The portable core: header-only (just the dist/ include dir). The one target a
#    cross/embedded build consumes; platform libs are the consumer's own.
add_library(dart INTERFACE)
add_dependencies(dart dist)
target_include_directories(dart INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/dist")

# 3. Host tools, tests and examples (sockets/threads/timers, so desktop-only).
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
  dart_program(pattern_demo   examples/pattern_demo.c)
  dart_program(pubsub         tools/pubsub.c)
  dart_program(if_probe_check tools/if_probe_check.c)

  # SHM-disabled test build (matches the dart_test_noshm in the tree).
  dart_program(dart_test_noshm tests/dart_test.c)
  target_compile_definitions(dart_test_noshm PRIVATE DART_NO_SHM)
endif()

# The DART Explorer GUI (explore/). It fetches/builds SDL3 + SDL3_ttf
option(DART_BUILD_EXPLORER "Build the DART Explorer GUI (fetches SDL3)" ON)
if(DART_BUILD_EXPLORER)
  add_subdirectory(explore)
endif()

# The DART WebSocket bridge (bridge/): one WS connection = one node on the mesh.
# It fetches IXWebSocket + nlohmann/json and needs a C++17 compiler.
option(DART_BUILD_BRIDGE "Build the DART WebSocket bridge (fetches IXWebSocket)" ON)
if(DART_BUILD_BRIDGE)
  add_subdirectory(bridge)
endif()

# The bridge's JS/TS client distributables (dist/dart.mjs + dart.d.ts + dart.js),
# compiled from bridge/client/dart.ts by tsc 5.5 (hermetic via npx). Off by default
# (needs Node); the outputs are committed. Also runs standalone:
#   node bridge/client/build.mjs
option(DART_BUILD_JS_CLIENT "Build the bridge JS client distributables (needs Node)" OFF)
if(DART_BUILD_JS_CLIENT)
  find_program(NODE_EXECUTABLE node REQUIRED)
  add_custom_target(js_client ALL
    COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/bridge/client/build.mjs"
    COMMENT "Building bridge JS client (dart.ts -> dist/dart.mjs + dart.d.ts + dart.js)"
    VERBATIM)
endif()
