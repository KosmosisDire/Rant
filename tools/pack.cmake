# The amalgamator: concatenates the src/ layers in dependency order into dist/rant.h
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
 * tools/pack.cmake) to regenerate.
 */
]==])

# Foldable section markers: editors collapse #pragma region blocks. The guard silences
# -Wunknown-pragmas on toolchains that do not know the markers.
set(REGION_GUARD [==[
#if defined(__GNUC__)   /* let the section markers below fold quietly */
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
]==])

# POSIX feature test preamble. Must precede the first system header so glibc exposes the
# socket API.
set(POSIX_PREAMBLE [==[
#if defined(RANT_IMPLEMENTATION) && !defined(_WIN32)
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

# rant.h: every layer in dependency order. Declarations first, then the implementation
# under RANT_IMPLEMENTATION.
function(build_header f)
  file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" version LIMIT_COUNT 1)
  file(WRITE  "${f}" "${BANNER}")
  file(APPEND "${f}" "${REGION_GUARD}")
  file(APPEND "${f}" "${POSIX_PREAMBLE}")
  file(APPEND "${f}" "#define RANT_VERSION \"${version}\"   /* the header's version, from VERSION */\n\n")

  rant_emit("${f}" common/api.h)
  rant_emit("${f}" common/features.h)
  rant_emit("${f}" common/types.h)
  rant_emit("${f}" common/string.h)
  rant_emit("${f}" common/alloc.h)
  rant_emit("${f}" discovery/core.h)
  rant_emit("${f}" platform/core.h)
  file(APPEND "${f}" "#ifdef RANT_TUNNEL\n")
  rant_emit("${f}" tunnel/core.h)
  file(APPEND "${f}" "#endif /* RANT_TUNNEL */\n")
  rant_emit("${f}" tunnel/runtime.h)
  rant_emit("${f}" discovery/runtime.h)
  rant_emit("${f}" transport/core.h)
  rant_emit("${f}" serialize/schema.h)
  file(APPEND "${f}" "#ifndef RANT_NO_STDTYPES\n")
  rant_emit("${f}" common/stdtypes.h)
  rant_emit("${f}" serialize/stdroster.h)
  rant_emit("${f}" serialize/stdtypes.h)
  file(APPEND "${f}" "#endif /* !RANT_NO_STDTYPES */\n")
  rant_emit("${f}" node/core.h)
  rant_emit("${f}" node/runtime.h)
  rant_emit("${f}" shm/core.h)
  file(APPEND "${f}" "#ifndef RANT_NO_PATTERNS\n")
  rant_emit("${f}" patterns/core.h)
  file(APPEND "${f}" "#endif /* !RANT_NO_PATTERNS */\n")

  file(APPEND "${f}" "\n#ifdef RANT_IMPLEMENTATION\n")
  rant_emit("${f}" common/bytes.h)
  rant_emit("${f}" common/arena.h)
  rant_emit("${f}" common/hash.h)
  rant_emit("${f}" discovery/records.h)
  rant_emit("${f}" discovery/records.c)
  rant_emit("${f}" discovery/core.c)
  file(APPEND "${f}" "#ifndef RANT_PLAT_CUSTOM\n")
  rant_emit("${f}" platform/core.c)
  file(APPEND "${f}" "#endif /* !RANT_PLAT_CUSTOM */\n")
  file(APPEND "${f}" "#ifdef RANT_TUNNEL\n")
  rant_emit("${f}" tunnel/core.c)
  file(APPEND "${f}" "#endif /* RANT_TUNNEL */\n")
  rant_emit("${f}" tunnel/runtime.c)
  rant_emit("${f}" discovery/runtime.c)
  rant_emit("${f}" transport/internal.h)
  rant_emit("${f}" transport/wire.c)
  rant_emit("${f}" transport/sched.c)
  rant_emit("${f}" transport/writer.c)
  rant_emit("${f}" transport/reader.c)
  rant_emit("${f}" transport/core.c)
  rant_emit("${f}" serialize/internal.h)
  rant_emit("${f}" serialize/schema.c)
  rant_emit("${f}" serialize/text.c)
  rant_emit("${f}" serialize/doc.c)
  rant_emit("${f}" serialize/match.c)
  rant_emit("${f}" serialize/message.c)
  rant_emit("${f}" serialize/map.c)
  file(APPEND "${f}" "#ifndef RANT_NO_STDTYPES\n")
  rant_emit("${f}" serialize/stdtable.c)
  rant_emit("${f}" serialize/stdtypes.c)
  file(APPEND "${f}" "#endif /* !RANT_NO_STDTYPES */\n")
  rant_emit("${f}" shm/core.c)
  rant_emit("${f}" node/core.c)
  rant_emit("${f}" node/runtime.c)
  file(APPEND "${f}" "#ifndef RANT_NO_PATTERNS\n")
  rant_emit("${f}" patterns/core.c)
  file(APPEND "${f}" "#endif /* !RANT_NO_PATTERNS */\n")
  file(APPEND "${f}" "#endif /* RANT_IMPLEMENTATION */\n")
  message(STATUS "wrote ${f}")
endfunction()

# rant.hpp: the C++ wrapper with each file spliced in at its @RANT_EMBED@ marker, so the
# shipped header is one self contained file serving both the C++ and the C side.
function(build_cpp f rant_h)
  set(cpp "${CMAKE_CURRENT_LIST_DIR}/../bindings/cpp")
  file(READ "${cpp}/rant.hpp" hpp)
  string(REGEX REPLACE "\r" "" hpp "${hpp}")   # normalize CRLF -> LF, deterministic output
  foreach(part "rant.h" "rant_types.hpp" "rant_reflect.inc")
    if(part STREQUAL "rant.h")
      file(READ "${rant_h}" body)
    else()
      file(READ "${cpp}/${part}" body)
    endif()
    string(REGEX REPLACE "\r" "" body "${body}")
    string(REPLACE "#include \"${part}\"   /* @RANT_EMBED@ */" "${body}" hpp "${hpp}")
  endforeach()
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

# rant_types.h: the standard types alone, under the same guard as their copy in rant.h, so a
# library built on the types alone and Rant both compile into one program.
function(build_types f)
  file(READ "${SRC}/common/stdtypes.h" c)
  string(REGEX REPLACE "\r" "" c "${c}")   # normalize CRLF -> LF, deterministic output
  string(REPLACE "Rant, the core library of RANT" "The standard types of Rant, the core library of RANT" b "${BANNER}")
  file(WRITE "${f}" "${b}${c}")
  message(STATUS "wrote ${f}")
endfunction()

# rant_types.hpp: the C++ standard types alone, under the same guard as their copy in rant.hpp.
function(build_types_cpp f)
  file(READ "${CMAKE_CURRENT_LIST_DIR}/../bindings/cpp/rant_types.hpp" c)
  string(REGEX REPLACE "\r" "" c "${c}")
  string(REPLACE "Rant, the core library of RANT" "The C++ standard types of Rant, the core library of RANT" b "${BANNER}")
  file(WRITE "${f}" "${b}${c}")
  message(STATUS "wrote ${f}")
endfunction()

# The C# wrapper is a hand written P/Invoke layer over the prebuilt native library, so
# nothing is generated for it.

build_header("${OUT}/rant.h")
build_types("${OUT}/rant_types.h")
build_cpp("${OUT}/rant.hpp" "${OUT}/rant.h")
build_types_cpp("${OUT}/rant_types.hpp")
build_anchor("${OUT}/rant.c" "rant.h")
build_anchor("${OUT}/rant.cpp" "rant.hpp")
message(STATUS "pack: done (${SRC} -> ${OUT})")
