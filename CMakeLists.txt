# Rant build: amalgamate src/ -> dist/ single-headers, then build the host
# tools and tests against dist/. Quick start: cmake -S . -B build && cmake
# --build build (or use the OS presets in CMakePresets.json). Embedded targets just
# link the header-only `rant` target and set -DRANT_BUILD_TOOLS=OFF (auto-off cross).

cmake_minimum_required(VERSION 3.15)

# The one version. Rant.csproj, pyproject.toml and the Unity packer read the same file,
# and the release workflow refuses a tag that does not match it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RANT_VERSION LIMIT_COUNT 1)
project(rant VERSION ${RANT_VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Another project consuming Rant gets the library targets and the packer that makes their
# headers, nothing else: no tools and no tests. PROJECT_IS_TOP_LEVEL wants cmake 3.21, this is its test.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(RANT_IS_TOP_LEVEL ON)
else()
  set(RANT_IS_TOP_LEVEL OFF)
endif()

include(GNUInstallDirs)

# Static CRT: binaries must run on a machine with no toolchain and no VC++
# redistributable. Must come before any target or fetched subproject.
include("${CMAKE_CURRENT_SOURCE_DIR}/tools/static_runtime.cmake")

if(RANT_IS_TOP_LEVEL)
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
endif()

# 1. Amalgamate src/ -> dist/, for a consumer too since dist/ is not committed. Configure
#    packs once if an output is missing, the build re packs when a src file changes.
file(GLOB_RECURSE RANT_SRC CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

set(RANT_DIST
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/rant_discovery.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/rant_transport.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/rant.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/rant.hpp"     # C++ wrapper with rant.h embedded
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/rant.c"       # the C implementation anchor
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/rant.cpp")    # the C++ implementation anchor
    # (the C# wrapper bindings/csharp/Rant.cs is hand-written over the prebuilt native lib, not generated)

set(RANT_PACK "${CMAKE_COMMAND}"
    -D "SRC=${CMAKE_CURRENT_SOURCE_DIR}/src"
    -D "OUT=${CMAKE_CURRENT_SOURCE_DIR}/dist"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack.cmake")

foreach(_rant_out IN LISTS RANT_DIST)
  if(NOT EXISTS "${_rant_out}")
    execute_process(COMMAND ${RANT_PACK} RESULT_VARIABLE _rant_pack)
    if(NOT _rant_pack EQUAL 0)
      message(FATAL_ERROR "Rant: packing src/ into dist/ failed")
    endif()
    break()
  endif()
endforeach()

add_custom_command(
  OUTPUT  ${RANT_DIST}
  COMMAND ${RANT_PACK}
  DEPENDS ${RANT_SRC} "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/rant.hpp"
  COMMENT "Rant: amalgamating src/ -> dist/ single-headers"
  VERBATIM)

# Not plain "dist", which a consumer project may well have already.
add_custom_target(rant_dist ALL DEPENDS ${RANT_DIST})

# 2. The portable core: header-only (just the dist/ include dir). The one target a
#    cross/embedded build consumes; platform libs are the consumer's own.
add_library(rant INTERFACE)
add_library(rant::rant ALIAS rant)
target_include_directories(rant INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/dist>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# cmake before 3.19 takes no dependency on an INTERFACE target, the configure pack covers it.
if(NOT CMAKE_VERSION VERSION_LESS 3.19)
  add_dependencies(rant rant_dist)
endif()

# 3. Host tools and tests (sockets/threads/timers, so desktop-only).
if(CMAKE_CROSSCOMPILING OR NOT RANT_IS_TOP_LEVEL)
  set(_rant_tools_default OFF)
else()
  set(_rant_tools_default ON)
endif()
option(RANT_BUILD_TOOLS "Build the Rant host tools and tests" ${_rant_tools_default})

# A cross build links its own platform, so it gets neither of these.
if(RANT_BUILD_TOOLS OR NOT CMAKE_CROSSCOMPILING)
  include("${CMAKE_CURRENT_SOURCE_DIR}/tools/rant_platform.cmake")

  # The built library: the one target a consumer links. It compiles the amalgamation once
  # so nothing outside this tree ever defines RANT_IMPLEMENTATION.
  add_library(rant_host STATIC "${CMAKE_CURRENT_SOURCE_DIR}/dist/rant.c")
  add_library(rant::rant_host ALIAS rant_host)
  target_link_libraries(rant_host PUBLIC rant rant_platform)
  add_dependencies(rant_host rant_dist)

  # The shared library the bindings load. RANT_BUILD_SHARED exports exactly the public
  # entry points and hidden visibility keeps the internals in (spec/build.md).
  add_library(rant_shared SHARED "${CMAKE_CURRENT_SOURCE_DIR}/dist/rant.c")
  add_library(rant::rant_shared ALIAS rant_shared)
  target_link_libraries(rant_shared PUBLIC rant PRIVATE rant_platform)
  target_compile_definitions(rant_shared PRIVATE RANT_BUILD_SHARED INTERFACE RANT_LINK_SHARED)
  set_target_properties(rant_shared PROPERTIES OUTPUT_NAME rant C_VISIBILITY_PRESET hidden
                        OSX_ARCHITECTURES "arm64;x86_64")
  add_dependencies(rant_shared rant_dist)

  # Filed by .NET runtime identifier under dist/native/, where every package build picks
  # it up. macOS is one universal binary, so its identifier is the portable osx.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(RANT_ARCH arm64)
  else()
    set(RANT_ARCH x64)
  endif()
  if(WIN32)
    set(RANT_RID "win-${RANT_ARCH}")
  elseif(APPLE)
    set(RANT_RID osx)
  else()
    set(RANT_RID "linux-${RANT_ARCH}")
  endif()
  if(RANT_IS_TOP_LEVEL)
    add_custom_command(TARGET rant_shared POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/dist/native/${RANT_RID}"
      COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:rant_shared>"
              "${CMAKE_CURRENT_SOURCE_DIR}/dist/native/${RANT_RID}/"
      VERBATIM)
  else()
    set_target_properties(rant_shared PROPERTIES EXCLUDE_FROM_ALL ON)
  endif()

  # A wheel build (scikit-build-core, spec/build.md) wants the library next to the package.
  if(SKBUILD)
    install(TARGETS rant_shared RUNTIME DESTINATION rant LIBRARY DESTINATION rant
            ARCHIVE DESTINATION rant EXCLUDE_FROM_ALL)
  endif()
endif()

if(RANT_BUILD_TOOLS)
  # One host program from one source file: link the core + platform libs and
  # wait for dist/ to be (re)generated first.
  # Each in tree program defines its own RANT_IMPLEMENTATION flavour (some want NO_SHM or
  # transport only), so they take the headers and the OS libraries, never the built library.
  function(rant_program name src)
    add_executable(${name} "${src}")
    target_link_libraries(${name} PRIVATE rant rant_platform)
    add_dependencies(${name} rant_dist)
  endfunction()

  rant_program(rant_test        tests/rant_test.c)
  rant_program(pubsub           tools/pubsub.c)
  rant_program(if_probe_check tools/if_probe_check.c)

  # SHM-disabled test build (matches the rant_test_noshm in the tree).
  rant_program(rant_test_noshm tests/rant_test.c)
  target_compile_definitions(rant_test_noshm PRIVATE RANT_NO_SHM)
endif()

# 4. The packages, into dist/. unity_package needs nothing installed, nuget_package needs
#    dotnet and python_wheel needs Python, and packages is whichever of the three this
#    machine can build. The release workflow runs the same three per platform.
if(RANT_IS_TOP_LEVEL AND TARGET rant_shared)
  add_custom_target(unity_package
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/unity.cmake" VERBATIM)
  add_dependencies(unity_package rant_shared)
  add_custom_target(packages DEPENDS unity_package)

  find_program(DOTNET_EXECUTABLE dotnet)
  if(DOTNET_EXECUTABLE)
    add_custom_target(nuget_package
      COMMAND "${DOTNET_EXECUTABLE}" pack "${CMAKE_CURRENT_SOURCE_DIR}/bindings/csharp/Rant.csproj"
              -c Release --nologo -o "${CMAKE_CURRENT_SOURCE_DIR}/dist" VERBATIM)
    add_dependencies(nuget_package rant_shared)
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

# 5. Install the single headers and a config package, so find_package(rant) gives the
#    same rant::rant and rant::rant_host a subproject build does. A consumer that exports
#    its own targets against rant:: turns this on, or the export refuses them.
option(RANT_INSTALL "Install the headers and the CMake config package" ${RANT_IS_TOP_LEVEL})
if(RANT_INSTALL)
  include(CMakePackageConfigHelpers)
  set(RANT_CMAKE_DIR "${CMAKE_INSTALL_DATADIR}/cmake/rant")

  set(RANT_INSTALL_TARGETS rant)
  if(TARGET rant_host)
    list(APPEND RANT_INSTALL_TARGETS rant_platform rant_host rant_shared)
  endif()

  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/dist/rant.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/dist/rant_discovery.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/dist/rant_transport.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/dist/rant.hpp"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

  install(TARGETS ${RANT_INSTALL_TARGETS} EXPORT rant-targets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
          LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
          RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  install(EXPORT rant-targets FILE rant-targets.cmake NAMESPACE rant::
          DESTINATION "${RANT_CMAKE_DIR}")

  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/rant-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/rant-config.cmake"
    INSTALL_DESTINATION "${RANT_CMAKE_DIR}")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/rant-config-version.cmake"
    COMPATIBILITY SameMinorVersion)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/rant-config.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/rant-config-version.cmake"
          DESTINATION "${RANT_CMAKE_DIR}")
endif()
