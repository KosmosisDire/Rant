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
 * Ramble, the networking layer of RANT. Amalgamated from src/ by the CMake
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

# RAMBLE_TRANSPORT_* bridge: pull in the sibling discovery header (it carries a
# real local #include, which must survive into the output).
set(BRIDGE [==[
#ifndef RAMBLE_TRANSPORT_SANS_IO
  #if defined(RAMBLE_TRANSPORT_IMPLEMENTATION) && !defined(RAMBLE_DISCOVERY_IMPLEMENTATION)
  #define RAMBLE_DISCOVERY_IMPLEMENTATION
  #endif
  #include "ramble_discovery.h"   /* discovery: needed by the node runtime */
#endif

]==])

# ramble.h's single user knob RAMBLE_* mapped onto the per-module flags.
set(FLAGMAP [==[
#ifdef RAMBLE_IMPLEMENTATION
  #ifndef RAMBLE_DISCOVERY_IMPLEMENTATION
  #define RAMBLE_DISCOVERY_IMPLEMENTATION
  #endif
  #ifndef RAMBLE_TRANSPORT_IMPLEMENTATION
  #define RAMBLE_TRANSPORT_IMPLEMENTATION
  #endif
#endif
#ifdef RAMBLE_SANS_IO
  #ifndef RAMBLE_DISCOVERY_SANS_IO
  #define RAMBLE_DISCOVERY_SANS_IO
  #endif
  #ifndef RAMBLE_TRANSPORT_SANS_IO
  #define RAMBLE_TRANSPORT_SANS_IO
  #endif
#endif

]==])

# POSIX feature test preamble. Must precede the first system header so glibc exposes the
# socket API.
function(ramble_posix_preamble f impl sansio)
  set(t [==[
#if defined(@IMPL@) && !defined(@SANSIO@) && !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
  #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE 1
  #endif
  #ifndef _DARWIN_C_SOURCE
  #define _DARWIN_C_SOURCE 1   /* Darwin hides its own extensions under _POSIX_C_SOURCE without this */
  #endif
#endif

]==])
  string(REPLACE "@IMPL@" "${impl}" t "${t}")
  string(REPLACE "@SANSIO@" "${sansio}" t "${t}")
  file(APPEND "${f}" "${t}")
endfunction()

# Append src/NAME to file F inside a foldable region with local includes dropped. The
# strip is anchored to a line start, so an include in a comment or string is never touched.
function(ramble_emit f name)
  file(READ "${SRC}/${name}" c)
  string(REGEX REPLACE "\r" "" c "${c}")   # normalize CRLF -> LF, deterministic output
  string(REGEX REPLACE "\n[ \t]*#include[ \t]*\"[^\"\n]*\"[^\n]*" "" c "\n${c}")
  string(REGEX REPLACE "^\n" "" c "${c}")
  if(NOT c MATCHES "\n$")
    set(c "${c}\n")
  endif()
  file(APPEND "${f}" "#pragma region ${name}\n${c}#pragma endregion\n")
endfunction()

# ramble_discovery.h: discovery core plus runtime, one header.
function(build_discovery f)
  file(WRITE  "${f}" "${BANNER}")
  file(APPEND "${f}" "${REGION_GUARD}")
  ramble_posix_preamble("${f}" RAMBLE_DISCOVERY_IMPLEMENTATION RAMBLE_DISCOVERY_SANS_IO)

  ramble_emit("${f}" common/api.h)
  ramble_emit("${f}" common/string.h)
  ramble_emit("${f}" common/alloc.h)
  ramble_emit("${f}" discovery/core.h)
  file(APPEND "${f}" "\n#ifndef RAMBLE_DISCOVERY_SANS_IO\n")
  ramble_emit("${f}" platform/core.h)
  ramble_emit("${f}" discovery/runtime.h)
  file(APPEND "${f}" "#endif /* !RAMBLE_DISCOVERY_SANS_IO */\n")

  file(APPEND "${f}" "\n#ifdef RAMBLE_DISCOVERY_IMPLEMENTATION\n")
  ramble_emit("${f}" common/bytes.h)
  ramble_emit("${f}" common/arena.h)
  ramble_emit("${f}" discovery/core.c)
  file(APPEND "${f}" "\n#ifndef RAMBLE_DISCOVERY_SANS_IO\n")
  file(APPEND "${f}" "#ifndef RAMBLE_PLAT_CUSTOM\n")
  ramble_emit("${f}" platform/core.c)
  file(APPEND "${f}" "#endif /* !RAMBLE_PLAT_CUSTOM */\n")
  ramble_emit("${f}" discovery/runtime.c)
  file(APPEND "${f}" "#endif /* !RAMBLE_DISCOVERY_SANS_IO */\n")
  file(APPEND "${f}" "#endif /* RAMBLE_DISCOVERY_IMPLEMENTATION */\n")
  message(STATUS "wrote ${f}")
endfunction()

# ramble_transport.h: transport core plus node runtime, one header. The node impl
# needs discovery, so this header includes the sibling ramble_discovery.h.
function(build_transport f)
  file(WRITE  "${f}" "${BANNER}")
  file(APPEND "${f}" "${REGION_GUARD}")
  ramble_posix_preamble("${f}" RAMBLE_TRANSPORT_IMPLEMENTATION RAMBLE_TRANSPORT_SANS_IO)
  file(APPEND "${f}" "${BRIDGE}")

  ramble_emit("${f}" common/api.h)
  ramble_emit("${f}" common/string.h)
  ramble_emit("${f}" common/alloc.h)
  ramble_emit("${f}" transport/core.h)
  file(APPEND "${f}" "\n#ifndef RAMBLE_TRANSPORT_SANS_IO\n")
  ramble_emit("${f}" serialize/schema.h)
  file(APPEND "${f}" "#ifndef RAMBLE_NO_STDTYPES
")
  ramble_emit("${f}" serialize/stdtypes.h)
  file(APPEND "${f}" "#endif /* !RAMBLE_NO_STDTYPES */
")
  ramble_emit("${f}" node/core.h)
  ramble_emit("${f}" node/runtime.h)
  ramble_emit("${f}" shm/core.h)
  file(APPEND "${f}" "#ifndef RAMBLE_NO_PATTERNS\n")
  ramble_emit("${f}" patterns/core.h)
  file(APPEND "${f}" "#endif /* !RAMBLE_NO_PATTERNS */\n")
  file(APPEND "${f}" "#endif /* !RAMBLE_TRANSPORT_SANS_IO */\n")

  file(APPEND "${f}" "\n#ifdef RAMBLE_TRANSPORT_IMPLEMENTATION\n")
  ramble_emit("${f}" common/bytes.h)
  ramble_emit("${f}" common/arena.h)
  ramble_emit("${f}" common/hash.h)
  ramble_emit("${f}" transport/internal.h)
  ramble_emit("${f}" transport/wire.c)
  ramble_emit("${f}" transport/sched.c)
  ramble_emit("${f}" transport/writer.c)
  ramble_emit("${f}" transport/reader.c)
  ramble_emit("${f}" transport/core.c)
  file(APPEND "${f}" "\n#ifndef RAMBLE_TRANSPORT_SANS_IO\n")
  ramble_emit("${f}" serialize/schema.c)
  file(APPEND "${f}" "#ifndef RAMBLE_NO_STDTYPES
")
  ramble_emit("${f}" serialize/stdtypes.c)
  file(APPEND "${f}" "#endif /* !RAMBLE_NO_STDTYPES */
")
  ramble_emit("${f}" shm/core.c)
  ramble_emit("${f}" node/core.c)
  ramble_emit("${f}" node/runtime.c)
  file(APPEND "${f}" "#ifndef RAMBLE_NO_PATTERNS\n")
  ramble_emit("${f}" patterns/core.c)
  file(APPEND "${f}" "#endif /* !RAMBLE_NO_PATTERNS */\n")
  file(APPEND "${f}" "#endif /* !RAMBLE_TRANSPORT_SANS_IO */\n")
  file(APPEND "${f}" "#endif /* RAMBLE_TRANSPORT_IMPLEMENTATION */\n")
  message(STATUS "wrote ${f}")
endfunction()

# ramble.h: discovery, transport, and node all inlined into one file.
function(build_combined f)
  file(WRITE  "${f}" "${BANNER}")
  file(APPEND "${f}" "${REGION_GUARD}")
  file(APPEND "${f}" "${FLAGMAP}")
  ramble_posix_preamble("${f}" RAMBLE_DISCOVERY_IMPLEMENTATION RAMBLE_DISCOVERY_SANS_IO)

  ramble_emit("${f}" common/api.h)
  ramble_emit("${f}" common/string.h)
  ramble_emit("${f}" common/alloc.h)
  ramble_emit("${f}" discovery/core.h)
  file(APPEND "${f}" "\n#ifndef RAMBLE_DISCOVERY_SANS_IO\n")
  ramble_emit("${f}" platform/core.h)
  ramble_emit("${f}" discovery/runtime.h)
  file(APPEND "${f}" "#endif /* !RAMBLE_DISCOVERY_SANS_IO */\n")
  ramble_emit("${f}" transport/core.h)
  file(APPEND "${f}" "\n#ifndef RAMBLE_TRANSPORT_SANS_IO\n")
  ramble_emit("${f}" serialize/schema.h)
  file(APPEND "${f}" "#ifndef RAMBLE_NO_STDTYPES
")
  ramble_emit("${f}" serialize/stdtypes.h)
  file(APPEND "${f}" "#endif /* !RAMBLE_NO_STDTYPES */
")
  ramble_emit("${f}" node/core.h)
  ramble_emit("${f}" node/runtime.h)
  ramble_emit("${f}" shm/core.h)
  file(APPEND "${f}" "#ifndef RAMBLE_NO_PATTERNS\n")
  ramble_emit("${f}" patterns/core.h)
  file(APPEND "${f}" "#endif /* !RAMBLE_NO_PATTERNS */\n")
  file(APPEND "${f}" "#endif /* !RAMBLE_TRANSPORT_SANS_IO */\n")

  file(APPEND "${f}" "\n#ifdef RAMBLE_DISCOVERY_IMPLEMENTATION\n")
  ramble_emit("${f}" common/bytes.h)
  ramble_emit("${f}" common/arena.h)
  ramble_emit("${f}" discovery/core.c)
  file(APPEND "${f}" "\n#ifndef RAMBLE_DISCOVERY_SANS_IO\n")
  file(APPEND "${f}" "#ifndef RAMBLE_PLAT_CUSTOM\n")
  ramble_emit("${f}" platform/core.c)
  file(APPEND "${f}" "#endif /* !RAMBLE_PLAT_CUSTOM */\n")
  ramble_emit("${f}" discovery/runtime.c)
  file(APPEND "${f}" "#endif /* !RAMBLE_DISCOVERY_SANS_IO */\n")
  file(APPEND "${f}" "#endif /* RAMBLE_DISCOVERY_IMPLEMENTATION */\n")

  file(APPEND "${f}" "\n#ifdef RAMBLE_TRANSPORT_IMPLEMENTATION\n")
  ramble_emit("${f}" common/bytes.h)
  ramble_emit("${f}" common/arena.h)
  ramble_emit("${f}" common/hash.h)
  ramble_emit("${f}" transport/internal.h)
  ramble_emit("${f}" transport/wire.c)
  ramble_emit("${f}" transport/sched.c)
  ramble_emit("${f}" transport/writer.c)
  ramble_emit("${f}" transport/reader.c)
  ramble_emit("${f}" transport/core.c)
  file(APPEND "${f}" "\n#ifndef RAMBLE_TRANSPORT_SANS_IO\n")
  ramble_emit("${f}" serialize/schema.c)
  file(APPEND "${f}" "#ifndef RAMBLE_NO_STDTYPES
")
  ramble_emit("${f}" serialize/stdtypes.c)
  file(APPEND "${f}" "#endif /* !RAMBLE_NO_STDTYPES */
")
  ramble_emit("${f}" shm/core.c)
  ramble_emit("${f}" node/core.c)
  ramble_emit("${f}" node/runtime.c)
  file(APPEND "${f}" "#ifndef RAMBLE_NO_PATTERNS\n")
  ramble_emit("${f}" patterns/core.c)
  file(APPEND "${f}" "#endif /* !RAMBLE_NO_PATTERNS */\n")
  file(APPEND "${f}" "#endif /* !RAMBLE_TRANSPORT_SANS_IO */\n")
  file(APPEND "${f}" "#endif /* RAMBLE_TRANSPORT_IMPLEMENTATION */\n")
  message(STATUS "wrote ${f}")
endfunction()

# ramble.hpp: the C++ wrapper with dist/ramble.h spliced in at its @RAMBLE_EMBED@ marker, so the
# shipped header is one self contained file serving both the C++ and the C side.
function(build_cpp f ramble_h)
  set(tmpl "${CMAKE_CURRENT_LIST_DIR}/../cpp/ramble.hpp")
  file(READ "${tmpl}" hpp)
  string(REGEX REPLACE "\r" "" hpp "${hpp}")   # normalize CRLF -> LF, deterministic output
  file(READ "${ramble_h}" dh)
  string(REGEX REPLACE "\r" "" dh "${dh}")
  string(REPLACE "#include \"ramble.h\"   /* @RAMBLE_EMBED@ */" "${dh}" hpp "${hpp}")
  file(WRITE "${f}" "${hpp}")
  message(STATUS "wrote ${f}")
endfunction()

# ramble.c and ramble.cpp: the implementation anchors. A consumer that links the built library
# never writes one, and the CMake target and the native plugin builds compile these.
function(build_anchor f header)
  file(WRITE "${f}" "/* GENERATED. The implementation anchor: exactly one translation unit
"
                    " * defines RAMBLE_IMPLEMENTATION so the amalgamation emits the library. */
"
                    "#define RAMBLE_IMPLEMENTATION
"
                    "#include \"${header}\"
")
  message(STATUS "wrote ${f}")
endfunction()

# The C# wrapper is a hand written P/Invoke layer over the prebuilt native library, so
# nothing is generated for it.

build_discovery("${OUT}/ramble_discovery.h")
build_transport("${OUT}/ramble_transport.h")
build_combined("${OUT}/ramble.h")
build_cpp("${OUT}/ramble.hpp" "${OUT}/ramble.h")
build_anchor("${OUT}/ramble.c" "ramble.h")
build_anchor("${OUT}/ramble.cpp" "ramble.hpp")
message(STATUS "pack: done (${SRC} -> ${OUT})")
