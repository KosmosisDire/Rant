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
 * Rant, the core library of RANT. Amalgamated from src/ by the CMake
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

# RANT_TRANSPORT_* bridge: pull in the sibling discovery header (it carries a
# real local #include, which must survive into the output).
set(BRIDGE [==[
#ifndef RANT_TRANSPORT_SANS_IO
  #if defined(RANT_TRANSPORT_IMPLEMENTATION) && !defined(RANT_DISCOVERY_IMPLEMENTATION)
  #define RANT_DISCOVERY_IMPLEMENTATION
  #endif
  #include "rant_discovery.h"   /* discovery: needed by the node runtime */
#endif

]==])

# rant.h's single user knob RANT_* mapped onto the per-module flags.
set(FLAGMAP [==[
#ifdef RANT_IMPLEMENTATION
  #ifndef RANT_DISCOVERY_IMPLEMENTATION
  #define RANT_DISCOVERY_IMPLEMENTATION
  #endif
  #ifndef RANT_TRANSPORT_IMPLEMENTATION
  #define RANT_TRANSPORT_IMPLEMENTATION
  #endif
#endif
#ifdef RANT_SANS_IO
  #ifndef RANT_DISCOVERY_SANS_IO
  #define RANT_DISCOVERY_SANS_IO
  #endif
  #ifndef RANT_TRANSPORT_SANS_IO
  #define RANT_TRANSPORT_SANS_IO
  #endif
#endif

]==])

# POSIX feature test preamble. Must precede the first system header so glibc exposes the
# socket API.
function(rant_posix_preamble f impl sansio)
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
function(rant_emit f name)
  file(READ "${SRC}/${name}" c)
  string(REGEX REPLACE "\r" "" c "${c}")   # normalize CRLF -> LF, deterministic output
  string(REGEX REPLACE "\n[ \t]*#include[ \t]*\"[^\"\n]*\"[^\n]*" "" c "\n${c}")
  string(REGEX REPLACE "^\n" "" c "${c}")
  if(NOT c MATCHES "\n$")
    set(c "${c}\n")
  endif()
  file(APPEND "${f}" "#pragma region ${name}\n${c}#pragma endregion\n")
endfunction()

# rant_discovery.h: discovery core plus runtime, one header.
function(build_discovery f)
  file(WRITE  "${f}" "${BANNER}")
  file(APPEND "${f}" "${REGION_GUARD}")
  rant_posix_preamble("${f}" RANT_DISCOVERY_IMPLEMENTATION RANT_DISCOVERY_SANS_IO)

  rant_emit("${f}" common/api.h)
  rant_emit("${f}" common/string.h)
  rant_emit("${f}" common/alloc.h)
  rant_emit("${f}" discovery/core.h)
  file(APPEND "${f}" "\n#ifndef RANT_DISCOVERY_SANS_IO\n")
  rant_emit("${f}" platform/core.h)
  rant_emit("${f}" discovery/runtime.h)
  file(APPEND "${f}" "#endif /* !RANT_DISCOVERY_SANS_IO */\n")

  file(APPEND "${f}" "\n#ifdef RANT_DISCOVERY_IMPLEMENTATION\n")
  rant_emit("${f}" common/bytes.h)
  rant_emit("${f}" common/arena.h)
  rant_emit("${f}" discovery/core.c)
  file(APPEND "${f}" "\n#ifndef RANT_DISCOVERY_SANS_IO\n")
  file(APPEND "${f}" "#ifndef RANT_PLAT_CUSTOM\n")
  rant_emit("${f}" platform/core.c)
  file(APPEND "${f}" "#endif /* !RANT_PLAT_CUSTOM */\n")
  rant_emit("${f}" discovery/runtime.c)
  file(APPEND "${f}" "#endif /* !RANT_DISCOVERY_SANS_IO */\n")
  file(APPEND "${f}" "#endif /* RANT_DISCOVERY_IMPLEMENTATION */\n")
  message(STATUS "wrote ${f}")
endfunction()

# rant_transport.h: transport core plus node runtime, one header. The node impl
# needs discovery, so this header includes the sibling rant_discovery.h.
function(build_transport f)
  file(WRITE  "${f}" "${BANNER}")
  file(APPEND "${f}" "${REGION_GUARD}")
  rant_posix_preamble("${f}" RANT_TRANSPORT_IMPLEMENTATION RANT_TRANSPORT_SANS_IO)
  file(APPEND "${f}" "${BRIDGE}")

  rant_emit("${f}" common/api.h)
  rant_emit("${f}" common/string.h)
  rant_emit("${f}" common/alloc.h)
  rant_emit("${f}" transport/core.h)
  file(APPEND "${f}" "\n#ifndef RANT_TRANSPORT_SANS_IO\n")
  rant_emit("${f}" serialize/schema.h)
  file(APPEND "${f}" "#ifndef RANT_NO_STDTYPES
")
  rant_emit("${f}" serialize/stdtypes.h)
  file(APPEND "${f}" "#endif /* !RANT_NO_STDTYPES */
")
  rant_emit("${f}" node/core.h)
  rant_emit("${f}" node/runtime.h)
  rant_emit("${f}" shm/core.h)
  file(APPEND "${f}" "#ifndef RANT_NO_PATTERNS\n")
  rant_emit("${f}" patterns/core.h)
  file(APPEND "${f}" "#endif /* !RANT_NO_PATTERNS */\n")
  file(APPEND "${f}" "#endif /* !RANT_TRANSPORT_SANS_IO */\n")

  file(APPEND "${f}" "\n#ifdef RANT_TRANSPORT_IMPLEMENTATION\n")
  rant_emit("${f}" common/bytes.h)
  rant_emit("${f}" common/arena.h)
  rant_emit("${f}" common/hash.h)
  rant_emit("${f}" transport/internal.h)
  rant_emit("${f}" transport/wire.c)
  rant_emit("${f}" transport/sched.c)
  rant_emit("${f}" transport/writer.c)
  rant_emit("${f}" transport/reader.c)
  rant_emit("${f}" transport/core.c)
  file(APPEND "${f}" "\n#ifndef RANT_TRANSPORT_SANS_IO\n")
  rant_emit("${f}" serialize/schema.c)
  file(APPEND "${f}" "#ifndef RANT_NO_STDTYPES
")
  rant_emit("${f}" serialize/stdtypes.c)
  file(APPEND "${f}" "#endif /* !RANT_NO_STDTYPES */
")
  rant_emit("${f}" shm/core.c)
  rant_emit("${f}" node/core.c)
  rant_emit("${f}" node/runtime.c)
  file(APPEND "${f}" "#ifndef RANT_NO_PATTERNS\n")
  rant_emit("${f}" patterns/core.c)
  file(APPEND "${f}" "#endif /* !RANT_NO_PATTERNS */\n")
  file(APPEND "${f}" "#endif /* !RANT_TRANSPORT_SANS_IO */\n")
  file(APPEND "${f}" "#endif /* RANT_TRANSPORT_IMPLEMENTATION */\n")
  message(STATUS "wrote ${f}")
endfunction()

# rant.h: discovery, transport, and node all inlined into one file.
function(build_combined f)
  file(WRITE  "${f}" "${BANNER}")
  file(APPEND "${f}" "${REGION_GUARD}")
  file(APPEND "${f}" "${FLAGMAP}")
  rant_posix_preamble("${f}" RANT_DISCOVERY_IMPLEMENTATION RANT_DISCOVERY_SANS_IO)

  rant_emit("${f}" common/api.h)
  rant_emit("${f}" common/string.h)
  rant_emit("${f}" common/alloc.h)
  rant_emit("${f}" discovery/core.h)
  file(APPEND "${f}" "\n#ifndef RANT_DISCOVERY_SANS_IO\n")
  rant_emit("${f}" platform/core.h)
  rant_emit("${f}" discovery/runtime.h)
  file(APPEND "${f}" "#endif /* !RANT_DISCOVERY_SANS_IO */\n")
  rant_emit("${f}" transport/core.h)
  file(APPEND "${f}" "\n#ifndef RANT_TRANSPORT_SANS_IO\n")
  rant_emit("${f}" serialize/schema.h)
  file(APPEND "${f}" "#ifndef RANT_NO_STDTYPES
")
  rant_emit("${f}" serialize/stdtypes.h)
  file(APPEND "${f}" "#endif /* !RANT_NO_STDTYPES */
")
  rant_emit("${f}" node/core.h)
  rant_emit("${f}" node/runtime.h)
  rant_emit("${f}" shm/core.h)
  file(APPEND "${f}" "#ifndef RANT_NO_PATTERNS\n")
  rant_emit("${f}" patterns/core.h)
  file(APPEND "${f}" "#endif /* !RANT_NO_PATTERNS */\n")
  file(APPEND "${f}" "#endif /* !RANT_TRANSPORT_SANS_IO */\n")

  file(APPEND "${f}" "\n#ifdef RANT_DISCOVERY_IMPLEMENTATION\n")
  rant_emit("${f}" common/bytes.h)
  rant_emit("${f}" common/arena.h)
  rant_emit("${f}" discovery/core.c)
  file(APPEND "${f}" "\n#ifndef RANT_DISCOVERY_SANS_IO\n")
  file(APPEND "${f}" "#ifndef RANT_PLAT_CUSTOM\n")
  rant_emit("${f}" platform/core.c)
  file(APPEND "${f}" "#endif /* !RANT_PLAT_CUSTOM */\n")
  rant_emit("${f}" discovery/runtime.c)
  file(APPEND "${f}" "#endif /* !RANT_DISCOVERY_SANS_IO */\n")
  file(APPEND "${f}" "#endif /* RANT_DISCOVERY_IMPLEMENTATION */\n")

  file(APPEND "${f}" "\n#ifdef RANT_TRANSPORT_IMPLEMENTATION\n")
  rant_emit("${f}" common/bytes.h)
  rant_emit("${f}" common/arena.h)
  rant_emit("${f}" common/hash.h)
  rant_emit("${f}" transport/internal.h)
  rant_emit("${f}" transport/wire.c)
  rant_emit("${f}" transport/sched.c)
  rant_emit("${f}" transport/writer.c)
  rant_emit("${f}" transport/reader.c)
  rant_emit("${f}" transport/core.c)
  file(APPEND "${f}" "\n#ifndef RANT_TRANSPORT_SANS_IO\n")
  rant_emit("${f}" serialize/schema.c)
  file(APPEND "${f}" "#ifndef RANT_NO_STDTYPES
")
  rant_emit("${f}" serialize/stdtypes.c)
  file(APPEND "${f}" "#endif /* !RANT_NO_STDTYPES */
")
  rant_emit("${f}" shm/core.c)
  rant_emit("${f}" node/core.c)
  rant_emit("${f}" node/runtime.c)
  file(APPEND "${f}" "#ifndef RANT_NO_PATTERNS\n")
  rant_emit("${f}" patterns/core.c)
  file(APPEND "${f}" "#endif /* !RANT_NO_PATTERNS */\n")
  file(APPEND "${f}" "#endif /* !RANT_TRANSPORT_SANS_IO */\n")
  file(APPEND "${f}" "#endif /* RANT_TRANSPORT_IMPLEMENTATION */\n")
  message(STATUS "wrote ${f}")
endfunction()

# rant.hpp: the C++ wrapper with dist/rant.h spliced in at its @RANT_EMBED@ marker, so the
# shipped header is one self contained file serving both the C++ and the C side.
function(build_cpp f rant_h)
  set(tmpl "${CMAKE_CURRENT_LIST_DIR}/../bindings/cpp/rant.hpp")
  file(READ "${tmpl}" hpp)
  string(REGEX REPLACE "\r" "" hpp "${hpp}")   # normalize CRLF -> LF, deterministic output
  file(READ "${rant_h}" dh)
  string(REGEX REPLACE "\r" "" dh "${dh}")
  string(REPLACE "#include \"rant.h\"   /* @RANT_EMBED@ */" "${dh}" hpp "${hpp}")
  file(WRITE "${f}" "${hpp}")
  message(STATUS "wrote ${f}")
endfunction()

# rant.c and rant.cpp: the implementation anchors. A consumer that links the built library
# never writes one, and the CMake target and the native plugin builds compile these.
function(build_anchor f header)
  file(WRITE "${f}" "/* GENERATED. The implementation anchor: exactly one translation unit
"
                    " * defines RANT_IMPLEMENTATION so the amalgamation emits the library. */
"
                    "#define RANT_IMPLEMENTATION
"
                    "#include \"${header}\"
")
  message(STATUS "wrote ${f}")
endfunction()

# The C# wrapper is a hand written P/Invoke layer over the prebuilt native library, so
# nothing is generated for it.

build_discovery("${OUT}/rant_discovery.h")
build_transport("${OUT}/rant_transport.h")
build_combined("${OUT}/rant.h")
build_cpp("${OUT}/rant.hpp" "${OUT}/rant.h")
build_anchor("${OUT}/rant.c" "rant.h")
build_anchor("${OUT}/rant.cpp" "rant.hpp")
message(STATUS "pack: done (${SRC} -> ${OUT})")
