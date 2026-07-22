# DART WebSocket bridge: one WS connection = one node on the mesh (see PROTOCOL.md).
# CMake fetches IXWebSocket (BSD-3, TLS/zlib off = zero dependencies) and nlohmann/json
# (control plane only); DART comes from ../dist. Builds standalone
# (cmake -S bridge -B bridge/build) or from the root (-DDART_BUILD_BRIDGE=ON).

cmake_minimum_required(VERSION 3.19)
project(dart_bridge C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Standalone: own the output dir and define a minimal `dart` -> ../dist. Under the
# root build the `dart` target already exists (with dist include + regeneration).
if(NOT TARGET dart)
  if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Release;Debug;RelWithDebInfo;MinSizeRel"
        CACHE STRING "Build configs (Release first = the default)" FORCE)
  endif()
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
  # land binaries in bin/, same as the root build; every config shares it (see CLAUDE.md)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${REPO_ROOT}/bin")
  foreach(_cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${_cfg}" _CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${REPO_ROOT}/bin")
  endforeach()
  add_library(dart INTERFACE)
  target_include_directories(dart INTERFACE "${REPO_ROOT}/dist")
endif()

include(FetchContent)

# IXWebSocket: thread-per-connection WebSocket server. TLS and zlib off makes it a
# zero-dependency static lib (no OpenSSL, no permessage-deflate: we want raw speed).
set(USE_TLS  OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ixwebsocket
  URL https://github.com/machinezone/IXWebSocket/archive/refs/tags/v12.0.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# nlohmann/json, header-only: the JSON control plane (the data plane never touches it).
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(ixwebsocket nlohmann_json)

# Two C++ TUs: bridge.cpp is the server; dart.cpp is the DART implementation anchor
# (it defines DART_IMPLEMENTATION; the C library compiles cleanly as C++).
add_executable(dart_bridge bridge.cpp dart.cpp)
target_link_libraries(dart_bridge PRIVATE dart ixwebsocket::ixwebsocket
                      nlohmann_json::nlohmann_json)

find_package(Threads REQUIRED)
target_link_libraries(dart_bridge PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(dart_bridge PRIVATE ws2_32 bcrypt winmm)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(dart_bridge PRIVATE rt)   # shm_open for the SHM fast path
endif()

# regenerate dist/ before building when the root build owns that target
if(TARGET dist)
  add_dependencies(dart_bridge dist)
endif()
