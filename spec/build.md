# Build system

The root `CMakeLists.txt` amalgamates `src/` into `dist/` and builds the host tools, tests
and examples. The header only portable core is an INTERFACE target `dart`. Host programs
link `dart_host`, which carries the platform libraries (Windows: ws2_32, bcrypt, winmm.
Linux: rt) plus Threads. Both carry a `dart::` alias, and that is what another project
links, whether it adds DART as a subproject or finds the installed package.

- `tools/pack.cmake` runs through an `add_custom_command` whose OUTPUT is the three dist
  headers, so it re packs only when a `src/` file or pack.cmake changes. It also splices
  `dart.h` into `dist/dart.hpp` and `dist/dart.py`.
- CMake `file(WRITE)` turns `\n` into CRLF on Windows while `file(READ)` strips CR. The
  packer strips `\r` after read so its logic is deterministic, and `.gitattributes` pins
  every text file to LF so the committed headers do not churn between machines.
- The packer strips local `#include "..."` lines with a newline anchored regex, so an
  include inside a comment or string is never touched. Each source file is wrapped in
  `#pragma region` markers so editors fold it, with `-Wunknown-pragmas` silenced on GCC.
- `tools/static_runtime.cmake` links the C and C++ runtime statically so a copied binary
  runs with no redistributable. MSVC's default `/MD` needs VCRUNTIME140.dll and the debug
  runtime ships only with Visual Studio, which is the "vcruntime dll not found" a copied
  explorer hits. It must be included before any target or FetchContent subproject so every
  object agrees on one runtime, and it forces policy CMP0091 NEW because vendored freetype
  asks for cmake 3.0 and would otherwise build `/MD` from the legacy flags. MinGW takes a
  full `-static`, other GNU toolchains static libgcc and libstdc++ with glibc shared. Every
  variable it sets is a plain one in DART's own directory scope, so a project that adds
  DART as a subproject keeps the runtime it picked and only DART's targets go static.
- `CMakePresets.json` has `windows` and `linux` configure presets, both with the explorer
  on and `binaryDir` `build`. The Windows preset pins no generator and no architecture so
  it works with any installed Visual Studio. Build presets `windows` and `linux` pin
  Release, `windows-debug` and `linux-debug` pin Debug.
- Multi config generators append a per config subdirectory to the output dir, so the root
  pins every config's output dir to `bin/`. The VS generator's `cmake --build` defaults to
  Debug whatever the config order, which is why the build preset pins Release.
  `CMAKE_DEFAULT_BUILD_TYPE` is rejected by the VS generator, so it is guarded to Ninja
  Multi-Config with an `unset(... CACHE)` else branch.
- `DART_BUILD_TOOLS=OFF` (auto when cross compiling or when DART is a subproject) drops
  the programs. `dart_platform` and `dart_host` survive it, because a consumer wants the
  library without the tests. A cross build gets `dart` alone: the platform is its own.
- Three targets, one job each. `dart` is the `dist/` include directory and nothing else.
  `dart_platform` is the OS libraries, in `tools/dart_platform.cmake` so the root build and
  a standalone `explore/` or `bridge/` configure share one list. `dart_host` is a static library over
  `dist/dart.c`, so a consumer defines no `DART_IMPLEMENTATION` and writes no anchor.
  The in tree programs link `dart` and `dart_platform` instead, never `dart_host`: each
  compiles its own flavour of the amalgamation, some with `DART_NO_SHM` or transport only,
  and linking the built library too would define every symbol twice.
- `dist/dart.c` and `dist/dart.cpp` are the generated anchors, two lines each. They are
  what `dart_host`, the native plugin builds and the bridge compile, so the define lives
  in one generated place instead of a hand written file per consumer.
- Everything but the library is top level only: the packer target, the programs, the
  explorer, the bridge, the `bin/` output dir and the multi config defaults. The test is
  `CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR`, since `PROJECT_IS_TOP_LEVEL` wants
  cmake 3.21 and the root asks for 3.15. Two of those really did reach a consumer: the
  config type list is a FORCEd cache entry so it overwrote the consumer's own, and the
  explorer and bridge defaults had it fetch SDL3, IXWebSocket and libdatachannel. `dist/`
  is committed, so a consumer never runs the packer and never reads `src/`.
- `install()` copies the four `dist/` headers into `include/` and writes an export set plus
  `dart-config.cmake` (from `tools/dart-config.cmake.in`) into `share/cmake/dart`, so
  `find_package(dart CONFIG)` hands over the same two targets. `CONFIG` is not optional:
  CMake ships a `FindDart` module for an unrelated old tool and module mode finds that
  first on a case insensitive filesystem. The config finds Threads before the targets file,
  which names `Threads::Threads`.
- `project()` carries a VERSION so the config package can answer a version request, with
  `SameMinorVersion` compatibility. Bump it with the release tag.
- Three builds, one macro. `src/common/api.h` defines `DART_API`, which every public
  declaration carries. The default build compiles DART into the caller's own binary and
  needs no decoration. `DART_BUILD_SHARED` marks each entry point exported, so the shared
  library's export table is exactly what the public headers declare and nothing else,
  which is why no binding keeps an export list. `DART_LINK_SHARED` marks them imported,
  which only Windows needs. GNU toolchains also take `-fvisibility=hidden` so the
  internals stay in. The declaration carries the attribute and the definition in the `.c`
  stays plain: the amalgamation puts both in one translation unit, declaration first.
- `dart_allocator_heap` and `dart_heap_realloc` are the process heap as an allocator and
  as a `DartAllocFn`. They exist so a binding over the shared library never has to reach
  for an internal symbol or route allocation back through its own runtime.
- The `DART_THREADS` and `DART_SHM` detection blocks live identically in both
  `platform/core.h` and `transport/core.h`, because every translation unit must agree
  whichever header it saw first. Edit both together. Code guards are `#ifdef DART_THREADS`,
  never `#ifndef DART_NO_THREADS`. A new platform layer declares support by implementing
  the thread, mutex, condvar and waker contract and defining `DART_THREADS` itself.
- The `DART_IMPLEMENTATION` auto define in `dart.hpp` is guarded by
  `!defined(__cplusplus)`, so a `.cpp` anchor spells the define itself.
- Bindings: `csharp/native/build.ps1` builds the native library with `DART_BUILD_SHARED`, `csharp/unity/pack.ps1`
  assembles the Unity package, `node bridge/client/build.mjs` regenerates the JS client
  dist. A tag push builds win-x64 and linux-x64 and publishes the NuGet and Unity packages.
  Nothing binary is committed.
- `.gitignore` binary patterns are anchored to the root. An unanchored `node` once matched
  `src/node/`. Check `git status --ignored` when a new source file will not stage.
