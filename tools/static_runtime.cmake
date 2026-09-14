# Link the C and C++ runtime statically so a copied binary runs with no redistributable.
# Include it before any target or subproject so every object agrees (spec/build.md).

if(RAMBLE_STATIC_RUNTIME_INCLUDED)
  return()
endif()
set(RAMBLE_STATIC_RUNTIME_INCLUDED ON)

option(RAMBLE_STATIC_RUNTIME "Link the C/C++ runtime statically (portable binaries)" ON)
if(NOT RAMBLE_STATIC_RUNTIME)
  return()
endif()

# CMP0091 makes MSVC_RUNTIME_LIBRARY authoritative. Forced NEW for subprojects too, since
# vendored freetype asks for cmake 3.0 and would quietly build /MD under the OLD policy.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The GNU toolchains share libgcc, libstdc++ and libwinpthread by default. MinGW takes a
# full -static since Windows has no system copy of them.
if(MINGW)
  set(_ramble_static_link "-static")
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
  set(_ramble_static_link "-static-libgcc -static-libstdc++")   # glibc stays shared
endif()
if(_ramble_static_link)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${_ramble_static_link}")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${_ramble_static_link}")
endif()
