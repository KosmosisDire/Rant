# Build system

The root `CMakeLists.txt` amalgamates `src/` into `dist/` and builds the host tools, tests
and examples. The header only portable core is an INTERFACE target `dart`. Host programs
link `dart_host`, which carries the platform libraries (Windows: ws2_32, bcrypt, winmm.
Linux: rt) plus Threads.

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
  full `-static`, other GNU toolchains static libgcc and libstdc++ with glibc shared.
- `CMakePresets.json` has `windows` and `linux` configure presets, both with the explorer
  on and `binaryDir` `build`. The Windows preset pins no generator and no architecture so
  it works with any installed Visual Studio. Build presets `windows` and `linux` pin
  Release, `windows-debug` and `linux-debug` pin Debug.
- Multi config generators append a per config subdirectory to the output dir, so the root
  pins every config's output dir to `bin/`. The VS generator's `cmake --build` defaults to
  Debug whatever the config order, which is why the build preset pins Release.
  `CMAKE_DEFAULT_BUILD_TYPE` is rejected by the VS generator, so it is guarded to Ninja
  Multi-Config with an `unset(... CACHE)` else branch.
- `DART_BUILD_TOOLS=OFF` (auto when cross compiling) builds only the `dart` target.
- The `DART_THREADS` and `DART_SHM` detection blocks live identically in both
  `platform/core.h` and `transport/core.h`, because every translation unit must agree
  whichever header it saw first. Edit both together. Code guards are `#ifdef DART_THREADS`,
  never `#ifndef DART_NO_THREADS`. A new platform layer declares support by implementing
  the thread, mutex, condvar and waker contract and defining `DART_THREADS` itself.
- The `DART_IMPLEMENTATION` auto define in `dart.hpp` is guarded by
  `!defined(__cplusplus)`, so a `.cpp` anchor spells the define itself.
- Bindings: `csharp/native/build.ps1` builds the native library, `csharp/unity/pack.ps1`
  assembles the Unity package, `node bridge/client/build.mjs` regenerates the JS client
  dist. A tag push builds win-x64 and linux-x64 and publishes the NuGet and Unity packages.
  Nothing binary is committed.
- `.gitignore` binary patterns are anchored to the root. An unanchored `node` once matched
  `src/node/`. Check `git status --ignored` when a new source file will not stage.
