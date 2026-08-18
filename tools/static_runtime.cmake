# Self-contained executables: link the C/C++ runtime statically, so a binary copied
# to another machine runs with no toolchain and no redistributable installed.
# Include this BEFORE any target or FetchContent subproject is declared, so every
# object (SDL3, freetype, IXWebSocket, DART) agrees on one runtime.
#
# MSVC's default /MD needs VCRUNTIME140.dll + the VC++ redistributable, and /MDd
# needs VCRUNTIME140D.dll + ucrtbased.dll, which ship ONLY with Visual Studio and
# may not legally be redistributed. That is the "vcruntime dll not found" a copied
# dart_explorer.exe hits.

if(DART_STATIC_RUNTIME_INCLUDED)
  return()
endif()
set(DART_STATIC_RUNTIME_INCLUDED ON)

option(DART_STATIC_RUNTIME "Link the C/C++ runtime statically (portable binaries)" ON)
if(NOT DART_STATIC_RUNTIME)
  return()
endif()

# CMP0091 is what makes MSVC_RUNTIME_LIBRARY authoritative. Force it NEW for
# subprojects too: vendored freetype still asks for cmake 3.0, which would leave the
# policy OLD and quietly build its objects /MD from the legacy flags.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Same idea for the GNU toolchains: libgcc, libstdc++ and libwinpthread are shared
# by default. MinGW takes full -static (there is no system copy of them on Windows).
if(MINGW)
  set(_dart_static_link "-static")
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
  set(_dart_static_link "-static-libgcc -static-libstdc++")   # glibc stays shared
endif()
if(_dart_static_link)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${_dart_static_link}")
endif()
