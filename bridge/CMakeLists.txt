# The bridge: one WebSocket connection is one node on the mesh, with a WebRTC data path.
# CMake fetches IXWebSocket, nlohmann/json and libdatachannel over MbedTLS (spec/bridge.md).

cmake_minimum_required(VERSION 3.21)
project(dart_bridge C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT (see the module): a copied exe must run with no VC++ redistributable.
include("${CMAKE_CURRENT_SOURCE_DIR}/../tools/static_runtime.cmake")

option(DART_BRIDGE_WEBRTC "WebRTC data path (fetches libdatachannel + MbedTLS)" ON)

# Standalone: own the output dir and define a minimal dart target over ../dist. Under the
# root build the dart target already exists.
if(NOT TARGET dart)
  if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Release;Debug;RelWithDebInfo;MinSizeRel"
        CACHE STRING "Build configs (Release first = the default)" FORCE)
  endif()
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
  # land binaries in bin/ like the root build, every config shares it (docs/building.md)
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

if(DART_BRIDGE_WEBRTC)
  # MbedTLS: the DTLS (and SRTP crypto) backend. Pure CMake, no perl/python at build
  # time (the release tarball ships its generated sources). Static, no programs/tests.
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(GEN_FILES OFF CACHE BOOL "" FORCE)
  set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
  # the default config leaves DTLS-SRTP off and the user config re enables it. A PUBLIC
  # define, so libdatachannel and libSRTP compile against the same configuration
  set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls_user_config.h" CACHE FILEPATH "" FORCE)
  FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(mbedtls)

  # libdatachannel with its bundled libjuice, usrsctp and libSRTP as submodules. Its WebSocket,
  # examples and tests are off, media stays on. Our FindMbedTLS shadows the finders it ships.
  list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  set(CMAKE_SKIP_INSTALL_RULES ON)   # nothing here installs, and its export set would name the fetched MbedTLS
  set(BUILD_SHARED_LIBS OFF)
  set(USE_MBEDTLS  ON  CACHE BOOL "" FORCE)
  set(NO_WEBSOCKET ON  CACHE BOOL "" FORCE)
  set(NO_EXAMPLES  ON  CACHE BOOL "" FORCE)
  set(NO_TESTS     ON  CACHE BOOL "" FORCE)
  set(NO_MEDIA     OFF CACHE BOOL "" FORCE)
  set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
  set(ENABLE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG        v0.23.2
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES "deps/libjuice" "deps/usrsctp" "deps/plog" "deps/libsrtp"
    GIT_SUBMODULES_RECURSE FALSE)
  FetchContent_MakeAvailable(libdatachannel)
endif()

# Two C++ translation units: bridge.cpp is the server, dart.cpp is the implementation
# anchor defining DART_IMPLEMENTATION.
add_executable(dart_bridge bridge.cpp dart.cpp)
target_link_libraries(dart_bridge PRIVATE dart dart_platform
                      ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json)
if(DART_BRIDGE_WEBRTC)
  target_compile_definitions(dart_bridge PRIVATE DART_BRIDGE_WEBRTC=1)
  target_link_libraries(dart_bridge PRIVATE LibDataChannel::LibDataChannelStatic)
endif()

# regenerate dist/ before building when the root build owns that target
if(TARGET dist)
  add_dependencies(dart_bridge dist)
endif()
