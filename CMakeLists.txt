# Ramble build: amalgamate src/ -> dist/ single-headers, then build the host
# tools/tests/examples against dist/. Quick start: cmake -S . -B build && cmake
# --build build (or use the OS presets in CMakePresets.json). Embedded targets just
# link the header-only `ramble` target and set -DRAMBLE_BUILD_TOOLS=OFF (auto-off cross).

cmake_minimum_required(VERSION 3.15)

# The one version. Ramble.csproj, pyproject.toml and the Unity packer read the same file,
# and the release workflow refuses a tag that does not match it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RAMBLE_VERSION LIMIT_COUNT 1)
project(ramble VERSION ${RAMBLE_VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Another project consuming Ramble gets the library targets and nothing else: no packer, no
# tools, no explorer, no bridge. PROJECT_IS_TOP_LEVEL wants cmake 3.21, this is its test.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(RAMBLE_IS_TOP_LEVEL ON)
else()
  set(RAMBLE_IS_TOP_LEVEL OFF)
endif()

include(GNUInstallDirs)

# Static CRT: binaries must run on a machine with no toolchain and no VC++
# redistributable. Must come before any target or fetched subproject.
include("${CMAKE_CURRENT_SOURCE_DIR}/tools/static_runtime.cmake")

if(RAMBLE_IS_TOP_LEVEL)
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
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
  foreach(_cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${_cfg}" _CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_CURRENT_SOURCE_DIR}/bin")
  endforeach()

  # 1. Amalgamate src/ -> dist/. Re-runs only when a src file or the packer changes.
  #    dist/ is committed, so a consumer never packs and never needs src/.
  file(GLOB_RECURSE RAMBLE_SRC CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
       "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

  set(RAMBLE_DIST
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ramble_discovery.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ramble_transport.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ramble.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ramble.hpp"     # C++ wrapper with ramble.h embedded
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ramble.c"       # the C implementation anchor
      "${CMAKE_CURRENT_SOURCE_DIR}/dist/ramble.cpp")    # the C++ implementation anchor
      # (the C# wrapper csharp/Ramble.cs is hand-written over the prebuilt native lib, not generated)

  add_custom_command(
    OUTPUT  ${RAMBLE_DIST}
    COMMAND "${CMAKE_COMMAND}"
            -D "SRC=${CMAKE_CURRENT_SOURCE_DIR}/src"
            -D "OUT=${CMAKE_CURRENT_SOURCE_DIR}/dist"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack.cmake"
    DEPENDS ${RAMBLE_SRC} "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/cpp/ramble.hpp"
    COMMENT "Ramble: amalgamating src/ -> dist/ single-headers"
    VERBATIM)

  add_custom_target(dist ALL DEPENDS ${RAMBLE_DIST})
endif()

# 2. The portable core: header-only (just the dist/ include dir). The one target a
#    cross/embedded build consumes; platform libs are the consumer's own.
add_library(ramble INTERFACE)
add_library(ramble::ramble ALIAS ramble)
target_include_directories(ramble INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/dist>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
if(TARGET dist)
  add_dependencies(ramble dist)
endif()

# 3. Host tools, tests and examples (sockets/threads/timers, so desktop-only).
if(CMAKE_CROSSCOMPILING OR NOT RAMBLE_IS_TOP_LEVEL)
  set(_ramble_tools_default OFF)
else()
  set(_ramble_tools_default ON)
endif()
option(RAMBLE_BUILD_TOOLS "Build the Ramble host tools, tests and examples" ${_ramble_tools_default})

# A cross build links its own platform, so it gets neither of these.
if(RAMBLE_BUILD_TOOLS OR NOT CMAKE_CROSSCOMPILING)
  include("${CMAKE_CURRENT_SOURCE_DIR}/tools/ramble_platform.cmake")

  # The built library: the one target a consumer links. It compiles the amalgamation once
  # so nothing outside this tree ever defines RAMBLE_IMPLEMENTATION.
  add_library(ramble_host STATIC "${CMAKE_CURRENT_SOURCE_DIR}/dist/ramble.c")
  add_library(ramble::ramble_host ALIAS ramble_host)
  target_link_libraries(ramble_host PUBLIC ramble ramble_platform)
  if(TARGET dist)
    add_dependencies(ramble_host dist)
  endif()

  # The shared library the bindings load. RAMBLE_BUILD_SHARED exports exactly the public
  # entry points and hidden visibility keeps the internals in (spec/build.md).
  add_library(ramble_shared SHARED "${CMAKE_CURRENT_SOURCE_DIR}/dist/ramble.c")
  add_library(ramble::ramble_shared ALIAS ramble_shared)
  target_link_libraries(ramble_shared PUBLIC ramble PRIVATE ramble_platform)
  target_compile_definitions(ramble_shared PRIVATE RAMBLE_BUILD_SHARED INTERFACE RAMBLE_LINK_SHARED)
  set_target_properties(ramble_shared PROPERTIES OUTPUT_NAME ramble C_VISIBILITY_PRESET hidden
                        OSX_ARCHITECTURES "arm64;x86_64")
  if(TARGET dist)
    add_dependencies(ramble_shared dist)
  endif()

  # Filed by .NET runtime identifier under dist/native/, where every package build picks
  # it up. macOS is one universal binary, so its identifier is the portable osx.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(RAMBLE_ARCH arm64)
  else()
    set(RAMBLE_ARCH x64)
  endif()
  if(WIN32)
    set(RAMBLE_RID "win-${RAMBLE_ARCH}")
  elseif(APPLE)
    set(RAMBLE_RID osx)
  else()
    set(RAMBLE_RID "linux-${RAMBLE_ARCH}")
  endif()
  if(RAMBLE_IS_TOP_LEVEL)
    add_custom_command(TARGET ramble_shared POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/dist/native/${RAMBLE_RID}"
      COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:ramble_shared>"
              "${CMAKE_CURRENT_SOURCE_DIR}/dist/native/${RAMBLE_RID}/"
      VERBATIM)
  else()
    set_target_properties(ramble_shared PROPERTIES EXCLUDE_FROM_ALL ON)
  endif()

  # A wheel build (scikit-build-core, spec/build.md) wants the library next to the package.
  if(SKBUILD)
    install(TARGETS ramble_shared RUNTIME DESTINATION ramble LIBRARY DESTINATION ramble
            ARCHIVE DESTINATION ramble EXCLUDE_FROM_ALL)
  endif()
endif()

if(RAMBLE_BUILD_TOOLS)
  # One host program from one source file: link the core + platform libs and
  # wait for dist/ to be (re)generated first.
  # Each in tree program defines its own RAMBLE_IMPLEMENTATION flavour (some want NO_SHM or
  # transport only), so they take the headers and the OS libraries, never the built library.
  function(ramble_program name src)
    add_executable(${name} "${src}")
    target_link_libraries(${name} PRIVATE ramble ramble_platform)
    if(TARGET dist)
      add_dependencies(${name} dist)
    endif()
  endfunction()

  ramble_program(ramble_test    tests/ramble_test.c)
  ramble_program(example        examples/example.c)
  ramble_program(pattern_demo   examples/pattern_demo.c)
  ramble_program(pubsub         tools/pubsub.c)
  ramble_program(if_probe_check tools/if_probe_check.c)

  # SHM-disabled test build (matches the ramble_test_noshm in the tree).
  ramble_program(ramble_test_noshm tests/ramble_test.c)
  target_compile_definitions(ramble_test_noshm PRIVATE RAMBLE_NO_SHM)
endif()

# The Ramble Explorer GUI (explore/). It fetches/builds SDL3 + SDL3_ttf
option(RAMBLE_BUILD_EXPLORER "Build the Ramble Explorer GUI (fetches SDL3)" ${RAMBLE_IS_TOP_LEVEL})
if(RAMBLE_BUILD_EXPLORER)
  add_subdirectory(explore)
endif()

# The Ramble WebSocket bridge (bridge/): one WS connection = one node on the mesh.
# It fetches IXWebSocket + nlohmann/json and needs a C++17 compiler.
option(RAMBLE_BUILD_BRIDGE "Build the Ramble WebSocket bridge (fetches IXWebSocket)" ${RAMBLE_IS_TOP_LEVEL})
if(RAMBLE_BUILD_BRIDGE)
  add_subdirectory(bridge)
endif()

# The bridge's JS/TS client distributables (dist/ramble.mjs + ramble.d.ts + ramble.js),
# compiled from bridge/client/ramble.ts by tsc 5.5 (hermetic via npx). Off by default
# (needs Node); the outputs are committed. Also runs standalone:
#   node bridge/client/build.mjs
option(RAMBLE_BUILD_JS_CLIENT "Build the bridge JS client distributables (needs Node)" OFF)
if(RAMBLE_BUILD_JS_CLIENT)
  find_program(NODE_EXECUTABLE node REQUIRED)
  add_custom_target(js_client ALL
    COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/bridge/client/build.mjs"
    COMMENT "Building bridge JS client (ramble.ts -> dist/ramble.mjs + ramble.d.ts + ramble.js)"
    VERBATIM)
endif()

# 4. The packages, into dist/. unity_package needs nothing installed, nuget_package needs
#    dotnet and python_wheel needs Python, and packages is whichever of the three this
#    machine can build. The release workflow runs the same three per platform.
if(RAMBLE_IS_TOP_LEVEL AND TARGET ramble_shared)
  add_custom_target(unity_package
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/unity.cmake" VERBATIM)
  add_dependencies(unity_package ramble_shared)
  add_custom_target(packages DEPENDS unity_package)

  find_program(DOTNET_EXECUTABLE dotnet)
  if(DOTNET_EXECUTABLE)
    add_custom_target(nuget_package
      COMMAND "${DOTNET_EXECUTABLE}" pack "${CMAKE_CURRENT_SOURCE_DIR}/csharp/Ramble.csproj"
              -c Release --nologo -o "${CMAKE_CURRENT_SOURCE_DIR}/dist" VERBATIM)
    add_dependencies(nuget_package ramble_shared)
    add_dependencies(packages nuget_package)
  endif()

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_custom_target(python_wheel
      COMMAND "${Python3_EXECUTABLE}" -m pip wheel "${CMAKE_CURRENT_SOURCE_DIR}" --no-deps
              -w "${CMAKE_CURRENT_SOURCE_DIR}/dist" VERBATIM)
    add_dependencies(packages python_wheel)
  endif()
endif()

# 5. Install the single headers and a config package, so find_package(ramble) gives the
#    same ramble::ramble and ramble::ramble_host a subproject build does. A consumer that exports
#    its own targets against ramble:: turns this on, or the export refuses them.
option(RAMBLE_INSTALL "Install the headers and the CMake config package" ${RAMBLE_IS_TOP_LEVEL})
if(RAMBLE_INSTALL)
  include(CMakePackageConfigHelpers)
  set(RAMBLE_CMAKE_DIR "${CMAKE_INSTALL_DATADIR}/cmake/ramble")

  set(RAMBLE_INSTALL_TARGETS ramble)
  if(TARGET ramble_host)
    list(APPEND RAMBLE_INSTALL_TARGETS ramble_platform ramble_host ramble_shared)
  endif()

  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/dist/ramble.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/dist/ramble_discovery.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/dist/ramble_transport.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/dist/ramble.hpp"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

  install(TARGETS ${RAMBLE_INSTALL_TARGETS} EXPORT ramble-targets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
          LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
          RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  install(EXPORT ramble-targets FILE ramble-targets.cmake NAMESPACE ramble::
          DESTINATION "${RAMBLE_CMAKE_DIR}")

  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/ramble-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ramble-config.cmake"
    INSTALL_DESTINATION "${RAMBLE_CMAKE_DIR}")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ramble-config-version.cmake"
    COMPATIBILITY SameMinorVersion)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ramble-config.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/ramble-config-version.cmake"
          DESTINATION "${RAMBLE_CMAKE_DIR}")
endif()
