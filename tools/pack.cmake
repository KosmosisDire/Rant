# The amalgamator: concatenates the src/ layers in dependency order into the dist/ headers
# with local includes dropped. docs/building.md has the by hand command and the flags.

cmake_minimum_required(VERSION 3.15)

if(NOT DEFINED SRC)
  set(SRC "${CMAKE_CURRENT_LIST_DIR}/../src")
endif()
if(NOT DEFINED OUT)
  set(OUT "${CMAKE_CURRENT_LIST_DIR}/../dist")
endif()

set(BANNER [==[
/* GENERATED single-header build. DO NOT EDIT.
 * DART = Discovery And Realtime Transport. Amalgamated from src/ by the CMake
 * build (tools/pack.cmake). Edit the split sources in src/ and rebuild (or run
 * tools/pack.cmake) to regenerate. See the flag scheme in tools/pack.cmake.
 */
]==])

# Foldable section markers: editors collapse #pragma region blocks. The guard silences
# -Wunknown-pragmas on toolchains that do not know the markers.
set(REGION_GUARD [==[
#if defined(__GNUC__)   /* let the section markers below fold quietly */
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
]==])

# DART_TRANSPORT_* bridge: pull in the sibling discovery header (it carries a
# real local #include, which must survive into the output).
set(BRIDGE [==[
#ifndef DART_TRANSPORT_SANS_IO
  #if defined(DART_TRANSPORT_IMPLEMENTATION) && !defined(DART_DISCOVERY_IMPLEMENTATION)
  #define DART_DISCOVERY_IMPLEMENTATION
  #endif
  #include "dart_discovery.h"   /* discovery: needed by the node runtime */
#endif

]==])

# dart.h's single user knob DART_* mapped onto the per-module flags.
set(FLAGMAP [==[
#ifdef DART_IMPLEMENTATION
  #ifndef DART_DISCOVERY_IMPLEMENTATION
  #define DART_DISCOVERY_IMPLEMENTATION
  #endif
  #ifndef DART_TRANSPORT_IMPLEMENTATION
  #define DART_TRANSPORT_IMPLEMENTATION
  #endif
#endif
#ifdef DART_SANS_IO
  #ifndef DART_DISCOVERY_SANS_IO
  #define DART_DISCOVERY_SANS_IO
  #endif
  #ifndef DART_TRANSPORT_SANS_IO
  #define DART_TRANSPORT_SANS_IO
  #endif
#endif

]==])

# POSIX feature test preamble. Must precede the first system header so glibc exposes the
# socket API.
function(dart_posix_preamble f impl sansio)
  set(t [==[
#if defined(@IMPL@) && !defined(@SANSIO@) && !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
#endif

]==])
  string(REPLACE "@IMPL@" "${impl}" t "${t}")
  string(REPLACE "@SANSIO@" "${sansio}" t "${t}")
  file(APPEND "${f}" "${t}")
endfunction()

# Append src/NAME to file F inside a foldable region with local includes dropped. The
# strip is anchored to a line start, so an include in a comment or string is never touched.
function(dart_emit f name)
  file(READ "${SRC}/${name}" c)
  string(REGEX REPLACE "\r" "" c "${c}")   # normalize CRLF -> LF, deterministic output
  string(REGEX REPLACE "\n[ \t]*#include[ \t]*\"[^\"\n]*\"[^\n]*" "" c "\n${c}")
  string(REGEX REPLACE "^\n" "" c "${c}")
  if(NOT c MATCHES "\n$")
    set(c "${c}\n")
  endif()
  file(APPEND "${f}" "#pragma region ${name}\n${c}#pragma endregion\n")
endfunction()

# dart_discovery.h: discovery core plus runtime, one header.
function(build_discovery f)
  file(WRITE  "${f}" "${BANNER}")
  file(APPEND "${f}" "${REGION_GUARD}")
  dart_posix_preamble("${f}" DART_DISCOVERY_IMPLEMENTATION DART_DISCOVERY_SANS_IO)

  dart_emit("${f}" common/api.h)
  dart_emit("${f}" common/string.h)
  dart_emit("${f}" common/alloc.h)
  dart_emit("${f}" discovery/core.h)
  file(APPEND "${f}" "\n#ifndef DART_DISCOVERY_SANS_IO\n")
  dart_emit("${f}" platform/core.h)
  dart_emit("${f}" discovery/runtime.h)
  file(APPEND "${f}" "#endif /* !DART_DISCOVERY_SANS_IO */\n")

  file(APPEND "${f}" "\n#ifdef DART_DISCOVERY_IMPLEMENTATION\n")
  dart_emit("${f}" common/bytes.h)
  dart_emit("${f}" common/arena.h)
  dart_emit("${f}" discovery/core.c)
  file(APPEND "${f}" "\n#ifndef DART_DISCOVERY_SANS_IO\n")
  file(APPEND "${f}" "#ifndef DART_PLAT_CUSTOM\n")
  dart_emit("${f}" platform/core.c)
  file(APPEND "${f}" "#endif /* !DART_PLAT_CUSTOM */\n")
  dart_emit("${f}" discovery/runtime.c)
  file(APPEND "${f}" "#endif /* !DART_DISCOVERY_SANS_IO */\n")
  file(APPEND "${f}" "#endif /* DART_DISCOVERY_IMPLEMENTATION */\n")
  message(STATUS "wrote ${f}")
endfunction()

# dart_transport.h: transport core plus node runtime, one header. The node impl
# needs discovery, so this header includes the sibling dart_discovery.h.
function(build_transport f)
  file(WRITE  "${f}" "${BANNER}")
  file(APPEND "${f}" "${REGION_GUARD}")
  dart_posix_preamble("${f}" DART_TRANSPORT_IMPLEMENTATION DART_TRANSPORT_SANS_IO)
  file(APPEND "${f}" "${BRIDGE}")

  dart_emit("${f}" common/api.h)
  dart_emit("${f}" common/string.h)
  dart_emit("${f}" common/alloc.h)
  dart_emit("${f}" transport/core.h)
  file(APPEND "${f}" "\n#ifndef DART_TRANSPORT_SANS_IO\n")
  dart_emit("${f}" serialize/schema.h)
  file(APPEND "${f}" "#ifndef DART_NO_STDTYPES
")
  dart_emit("${f}" serialize/stdtypes.h)
  file(APPEND "${f}" "#endif /* !DART_NO_STDTYPES */
")
  dart_emit("${f}" node/core.h)
  dart_emit("${f}" node/runtime.h)
  dart_emit("${f}" shm/core.h)
  file(APPEND "${f}" "#ifndef DART_NO_PATTERNS\n")
  dart_emit("${f}" patterns/core.h)
  file(APPEND "${f}" "#endif /* !DART_NO_PATTERNS */\n")
  file(APPEND "${f}" "#endif /* !DART_TRANSPORT_SANS_IO */\n")

  file(APPEND "${f}" "\n#ifdef DART_TRANSPORT_IMPLEMENTATION\n")
  dart_emit("${f}" common/bytes.h)
  dart_emit("${f}" common/arena.h)
  dart_emit("${f}" common/hash.h)
  dart_emit("${f}" transport/internal.h)
  dart_emit("${f}" transport/wire.c)
  dart_emit("${f}" transport/sched.c)
  dart_emit("${f}" transport/writer.c)
  dart_emit("${f}" transport/reader.c)
  dart_emit("${f}" transport/core.c)
  file(APPEND "${f}" "\n#ifndef DART_TRANSPORT_SANS_IO\n")
  dart_emit("${f}" serialize/schema.c)
  file(APPEND "${f}" "#ifndef DART_NO_STDTYPES
")
  dart_emit("${f}" serialize/stdtypes.c)
  file(APPEND "${f}" "#endif /* !DART_NO_STDTYPES */
")
  dart_emit("${f}" shm/core.c)
  dart_emit("${f}" node/core.c)
  dart_emit("${f}" node/runtime.c)
  file(APPEND "${f}" "#ifndef DART_NO_PATTERNS\n")
  dart_emit("${f}" patterns/core.c)
  file(APPEND "${f}" "#endif /* !DART_NO_PATTERNS */\n")
  file(APPEND "${f}" "#endif /* !DART_TRANSPORT_SANS_IO */\n")
  file(APPEND "${f}" "#endif /* DART_TRANSPORT_IMPLEMENTATION */\n")
  message(STATUS "wrote ${f}")
endfunction()

# dart.h: discovery, transport, and node all inlined into one file.
function(build_combined f)
  file(WRITE  "${f}" "${BANNER}")
  file(APPEND "${f}" "${REGION_GUARD}")
  file(APPEND "${f}" "${FLAGMAP}")
  dart_posix_preamble("${f}" DART_DISCOVERY_IMPLEMENTATION DART_DISCOVERY_SANS_IO)

  dart_emit("${f}" common/api.h)
  dart_emit("${f}" common/string.h)
  dart_emit("${f}" common/alloc.h)
  dart_emit("${f}" discovery/core.h)
  file(APPEND "${f}" "\n#ifndef DART_DISCOVERY_SANS_IO\n")
  dart_emit("${f}" platform/core.h)
  dart_emit("${f}" discovery/runtime.h)
  file(APPEND "${f}" "#endif /* !DART_DISCOVERY_SANS_IO */\n")
  dart_emit("${f}" transport/core.h)
  file(APPEND "${f}" "\n#ifndef DART_TRANSPORT_SANS_IO\n")
  dart_emit("${f}" serialize/schema.h)
  file(APPEND "${f}" "#ifndef DART_NO_STDTYPES
")
  dart_emit("${f}" serialize/stdtypes.h)
  file(APPEND "${f}" "#endif /* !DART_NO_STDTYPES */
")
  dart_emit("${f}" node/core.h)
  dart_emit("${f}" node/runtime.h)
  dart_emit("${f}" shm/core.h)
  file(APPEND "${f}" "#ifndef DART_NO_PATTERNS\n")
  dart_emit("${f}" patterns/core.h)
  file(APPEND "${f}" "#endif /* !DART_NO_PATTERNS */\n")
  file(APPEND "${f}" "#endif /* !DART_TRANSPORT_SANS_IO */\n")

  file(APPEND "${f}" "\n#ifdef DART_DISCOVERY_IMPLEMENTATION\n")
  dart_emit("${f}" common/bytes.h)
  dart_emit("${f}" common/arena.h)
  dart_emit("${f}" discovery/core.c)
  file(APPEND "${f}" "\n#ifndef DART_DISCOVERY_SANS_IO\n")
  file(APPEND "${f}" "#ifndef DART_PLAT_CUSTOM\n")
  dart_emit("${f}" platform/core.c)
  file(APPEND "${f}" "#endif /* !DART_PLAT_CUSTOM */\n")
  dart_emit("${f}" discovery/runtime.c)
  file(APPEND "${f}" "#endif /* !DART_DISCOVERY_SANS_IO */\n")
  file(APPEND "${f}" "#endif /* DART_DISCOVERY_IMPLEMENTATION */\n")

  file(APPEND "${f}" "\n#ifdef DART_TRANSPORT_IMPLEMENTATION\n")
  dart_emit("${f}" common/bytes.h)
  dart_emit("${f}" common/arena.h)
  dart_emit("${f}" common/hash.h)
  dart_emit("${f}" transport/internal.h)
  dart_emit("${f}" transport/wire.c)
  dart_emit("${f}" transport/sched.c)
  dart_emit("${f}" transport/writer.c)
  dart_emit("${f}" transport/reader.c)
  dart_emit("${f}" transport/core.c)
  file(APPEND "${f}" "\n#ifndef DART_TRANSPORT_SANS_IO\n")
  dart_emit("${f}" serialize/schema.c)
  file(APPEND "${f}" "#ifndef DART_NO_STDTYPES
")
  dart_emit("${f}" serialize/stdtypes.c)
  file(APPEND "${f}" "#endif /* !DART_NO_STDTYPES */
")
  dart_emit("${f}" shm/core.c)
  dart_emit("${f}" node/core.c)
  dart_emit("${f}" node/runtime.c)
  file(APPEND "${f}" "#ifndef DART_NO_PATTERNS\n")
  dart_emit("${f}" patterns/core.c)
  file(APPEND "${f}" "#endif /* !DART_NO_PATTERNS */\n")
  file(APPEND "${f}" "#endif /* !DART_TRANSPORT_SANS_IO */\n")
  file(APPEND "${f}" "#endif /* DART_TRANSPORT_IMPLEMENTATION */\n")
  message(STATUS "wrote ${f}")
endfunction()

# dart.hpp: the C++ wrapper with dist/dart.h spliced in at its @DART_EMBED@ marker, so the
# shipped header is one self contained file serving both the C++ and the C side.
function(build_cpp f dart_h)
  set(tmpl "${CMAKE_CURRENT_LIST_DIR}/../cpp/dart.hpp")
  file(READ "${tmpl}" hpp)
  string(REGEX REPLACE "\r" "" hpp "${hpp}")   # normalize CRLF -> LF, deterministic output
  file(READ "${dart_h}" dh)
  string(REGEX REPLACE "\r" "" dh "${dh}")
  string(REPLACE "#include \"dart.h\"   /* @DART_EMBED@ */" "${dh}" hpp "${hpp}")
  file(WRITE "${f}" "${hpp}")
  message(STATUS "wrote ${f}")
endfunction()

# dart.py: the Python wrapper with dist/dart.h embedded as a string at its @DART_EMBED@
# marker. The C is escaped into one logical line so any content is safe.
function(build_py f dart_h)
  set(tmpl "${CMAKE_CURRENT_LIST_DIR}/../python/dart.py.in")
  file(READ "${dart_h}" dh)
  string(REGEX REPLACE "\r" "" dh "${dh}")   # normalize CRLF -> LF first
  string(REPLACE "\\" "\\\\" dh "${dh}")      # escape backslashes
  string(REPLACE "\"" "\\\"" dh "${dh}")      # escape double quotes
  string(REPLACE "\n" "\\n"  dh "${dh}")      # newlines -> \n (keep it one line)
  file(READ "${tmpl}" py)
  string(REGEX REPLACE "\r" "" py "${py}")
  string(REPLACE "@DART_EMBED@" "${dh}" py "${py}")
  file(WRITE "${f}" "${py}")
  message(STATUS "wrote ${f}")
endfunction()

# The C# wrapper is a hand written P/Invoke layer over the prebuilt native library, so
# nothing is generated for it.

build_discovery("${OUT}/dart_discovery.h")
build_transport("${OUT}/dart_transport.h")
build_combined("${OUT}/dart.h")
build_cpp("${OUT}/dart.hpp" "${OUT}/dart.h")
build_py("${OUT}/dart.py" "${OUT}/dart.h")
message(STATUS "pack: done (${SRC} -> ${OUT})")
