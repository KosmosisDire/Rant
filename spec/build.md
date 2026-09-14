# Build system

The root `CMakeLists.txt` amalgamates `src/` into `dist/` and builds the host tools and
tests. Four targets: `rant` is the header only core, `rant_platform` the OS
libraries (Windows: ws2_32, bcrypt, winmm. Linux: rt) plus Threads, `rant_host` the built
static library a consumer links, and `rant_shared` the shared library the bindings load.
Each carries a `rant::` alias, and that is what another project links, whether it adds
Rant as a subproject or finds the installed package.

- `tools/pack.cmake` runs through an `add_custom_command` whose OUTPUT is the three dist
  headers, so it re packs only when a `src/` file or pack.cmake changes. It also splices
  `rant.h` into `dist/rant.hpp` and writes the two anchors. `dist/` is not committed,
  so configure also runs the packer once when any output is missing: a header only
  consumer depends on no build step, and cmake before 3.19 takes no dependency on an
  INTERFACE target. The target is `rant_dist`, not `dist`, since a consumer may own that
  name.
- `pyproject.toml` builds the Python wheel through scikit-build-core, which runs this same
  CMake with `SKBUILD` set. The one rule keyed on it installs `rant_shared` next to the
  package. The wheel is tagged `py3-none`, since ctypes needs no Python ABI, so one wheel
  per platform serves every interpreter. Linux wheels are built inside manylinux, which
  is what gives the shipped Linux library its glibc 2.28 floor.
- CMake `file(WRITE)` turns `\n` into CRLF on Windows while `file(READ)` strips CR. The
  packer strips `\r` after read so its logic is deterministic, and `.gitattributes` pins
  every committed text file to LF.
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
  variable it sets is a plain one in Rant's own directory scope, so a project that adds
  Rant as a subproject keeps the runtime it picked and only Rant's targets go static.
- `CMakePresets.json` has `windows` and `linux` configure presets, both with `binaryDir`
  `build`. The Windows preset pins no generator and no architecture so
  it works with any installed Visual Studio. Build presets `windows` and `linux` pin
  Release, `windows-debug` and `linux-debug` pin Debug.
- Multi config generators append a per config subdirectory to the output dir, so the root
  pins every config's output dir to `bin/`. The VS generator's `cmake --build` defaults to
  Debug whatever the config order, which is why the build preset pins Release.
  `CMAKE_DEFAULT_BUILD_TYPE` is rejected by the VS generator, so it is guarded to Ninja
  Multi-Config with an `unset(... CACHE)` else branch.
- `RANT_BUILD_TOOLS=OFF` (auto when cross compiling or when Rant is a subproject) drops
  the programs. `rant_platform` and `rant_host` survive it, because a consumer wants the
  library without the tests. A cross build gets `rant` alone: the platform is its own.
- Four targets, one job each. `rant` is the `dist/` include directory and nothing else.
  `rant_platform` is the OS libraries, in `tools/rant_platform.cmake` so the root build and
  a standalone `explore/` or `bridge/` configure share one list. `rant_host` is a static library over
  `dist/rant.c`, so a consumer defines no `RANT_IMPLEMENTATION` and writes no anchor.
  The in tree programs link `rant` and `rant_platform` instead, never `rant_host`: each
  compiles its own flavour of the amalgamation, some with `RANT_NO_SHM` or transport only,
  and linking the built library too would define every symbol twice. `rant_shared` is the
  same anchor built with `RANT_BUILD_SHARED` and hidden visibility, `EXCLUDE_FROM_ALL` in a
  subproject, and it exports `RANT_LINK_SHARED` to whatever links it.
- `dist/rant.c` and `dist/rant.cpp` are the generated anchors, two lines each. They are
  what `rant_host`, the native plugin builds and the bridge compile, so the define lives
  in one generated place instead of a hand written file per consumer.
- Everything but the library and its packer is top level only: the programs, the `bin/`
  output dir and the multi config defaults. The test is
  `CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR`, since `PROJECT_IS_TOP_LEVEL` wants
  cmake 3.21 and the root asks for 3.15. The config type list really did reach a
  consumer once: it is a FORCEd cache entry, so it overwrote the consumer's own. `dist/` is
  not committed, so a consumer runs the packer and reads `src/`.
- `install()` copies the four `dist/` headers into `include/` and writes an export set plus
  `rant-config.cmake` (from `tools/rant-config.cmake.in`) into `share/cmake/rant`, so
  `find_package(rant CONFIG)` hands over the same two targets. The config finds Threads
  before the targets file, which names `Threads::Threads`.
- The version lives once, in `VERSION` at the root: `project()` reads it so the config
  package answers a version request with `SameMinorVersion` compatibility, `Rant.csproj`
  reads it through an MSBuild property function, `pyproject.toml` through the
  scikit-build-core regex provider, and the Unity packer stamps it into `package.json`.
  A release is a tag `v` plus that number, and the workflow refuses any other tag.
- Three builds, one macro. `src/common/api.h` defines `RANT_API`, which every public
  declaration carries. The default build compiles Rant into the caller's own binary and
  needs no decoration. `RANT_BUILD_SHARED` marks each entry point exported, so the shared
  library's export table is exactly what the public headers declare and nothing else,
  which is why no binding keeps an export list. `RANT_LINK_SHARED` marks them imported,
  which only Windows needs. GNU toolchains also take `-fvisibility=hidden` so the
  internals stay in. The declaration carries the attribute and the definition in the `.c`
  stays plain: the amalgamation puts both in one translation unit, declaration first.
- `rant_allocator_heap` and `rant_heap_realloc` are the process heap as an allocator and
  as a `RantAllocFn`. They exist so a binding over the shared library never has to reach
  for an internal symbol or route allocation back through its own runtime.
- The `RANT_THREADS` and `RANT_SHM` detection blocks live identically in both
  `platform/core.h` and `transport/core.h`, because every translation unit must agree
  whichever header it saw first. Edit both together. Code guards are `#ifdef RANT_THREADS`,
  never `#ifndef RANT_NO_THREADS`. A new platform layer declares support by implementing
  the thread, mutex, condvar and waker contract and defining `RANT_THREADS` itself.
- The `RANT_IMPLEMENTATION` auto define in `rant.hpp` is guarded by
  `!defined(__cplusplus)`, so a `.cpp` anchor spells the define itself.
- Bindings: `rant_shared` is the one native library every binding loads. The top level
  build copies it to `dist/native/<rid>/`, named by .NET runtime identifier, so the NuGet,
  Unity and Python packages read one place. macOS builds it universal under the portable
  `osx` identifier. `tools/unity.cmake` assembles the Unity package. Nothing under `dist/`
  is committed.
- The `packages` target builds whichever of `unity_package`, `nuget_package` (needs
  dotnet) and `python_wheel` (needs Python) this machine can, into `dist/`, each over the
  libraries in `dist/native/`. The release workflow runs the same three after merging the
  libraries of every platform.
- CI is two workflows. `build.yml` runs on every push to main and every pull request, on
  linux, windows and macos: configure and build the library and its programs, build
  `tools/consumer` from the source tree and from an install, pack the NuGet and build the
  wheel of that platform. No test suite runs yet: that waits on the test rework, so a red build means
  a build broke.
- `release.yml` runs on a tag `v*` and refuses one that differs from `VERSION`. Two jobs
  per platform, then one release job. `native` runs cibuildwheel, which builds the wheel
  and, inside it, the library the NuGet and Unity packages bundle, so the Linux libraries
  come out of the manylinux container, aarch64 under QEMU since no arm runner is free on
  a private repo. `programs` builds the tools into one zip per platform, Linux on
  ubuntu-22.04 for a glibc 2.35 floor. `release` builds the sdist from the clean checkout,
  packs the `dist/` headers, merges the libraries, packs the NuGet, assembles the Unity
  package, attaches
  everything to a GitHub Release, pushes the upm branch, and publishes to nuget.org and
  PyPI when the `NUGET_API_KEY` and `PYPI_API_TOKEN` secrets exist.
- `tools/consumer` is a throwaway project that consumes Rant the way a user does, from the
  source tree by default and from an install with `RANT_CONSUMER_FIND_PACKAGE=ON`. It is
  the only thing that notices when external consumption breaks, which the normal build
  cannot see.
- `.gitignore` binary patterns are anchored to the root. An unanchored `node` once matched
  `src/node/`. Check `git status --ignored` when a new source file will not stage.
