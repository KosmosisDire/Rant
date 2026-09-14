# Build system

The root `CMakeLists.txt` amalgamates `src/` into `dist/` and builds the host tools, tests
and examples. Four targets: `ramble` is the header only core, `ramble_platform` the OS
libraries (Windows: ws2_32, bcrypt, winmm. Linux: rt) plus Threads, `ramble_host` the built
static library a consumer links, and `ramble_shared` the shared library the bindings load.
Each carries a `ramble::` alias, and that is what another project links, whether it adds
Ramble as a subproject or finds the installed package.

- `tools/pack.cmake` runs through an `add_custom_command` whose OUTPUT is the three dist
  headers, so it re packs only when a `src/` file or pack.cmake changes. It also splices
  `ramble.h` into `dist/ramble.hpp` and writes the two anchors.
- `pyproject.toml` builds the Python wheel through scikit-build-core, which runs this same
  CMake with `SKBUILD` set. The one rule keyed on it installs `ramble_shared` next to the
  package. The wheel is tagged `py3-none`, since ctypes needs no Python ABI, so one wheel
  per platform serves every interpreter. Linux wheels are built inside manylinux, which
  is what gives the shipped Linux library its glibc 2.28 floor.
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
  variable it sets is a plain one in Ramble's own directory scope, so a project that adds
  Ramble as a subproject keeps the runtime it picked and only Ramble's targets go static.
- `CMakePresets.json` has `windows` and `linux` configure presets, both with the explorer
  on and `binaryDir` `build`. The Windows preset pins no generator and no architecture so
  it works with any installed Visual Studio. Build presets `windows` and `linux` pin
  Release, `windows-debug` and `linux-debug` pin Debug.
- Multi config generators append a per config subdirectory to the output dir, so the root
  pins every config's output dir to `bin/`. The VS generator's `cmake --build` defaults to
  Debug whatever the config order, which is why the build preset pins Release.
  `CMAKE_DEFAULT_BUILD_TYPE` is rejected by the VS generator, so it is guarded to Ninja
  Multi-Config with an `unset(... CACHE)` else branch.
- `RAMBLE_BUILD_TOOLS=OFF` (auto when cross compiling or when Ramble is a subproject) drops
  the programs. `ramble_platform` and `ramble_host` survive it, because a consumer wants the
  library without the tests. A cross build gets `ramble` alone: the platform is its own.
- Four targets, one job each. `ramble` is the `dist/` include directory and nothing else.
  `ramble_platform` is the OS libraries, in `tools/ramble_platform.cmake` so the root build and
  a standalone `explore/` or `bridge/` configure share one list. `ramble_host` is a static library over
  `dist/ramble.c`, so a consumer defines no `RAMBLE_IMPLEMENTATION` and writes no anchor.
  The in tree programs link `ramble` and `ramble_platform` instead, never `ramble_host`: each
  compiles its own flavour of the amalgamation, some with `RAMBLE_NO_SHM` or transport only,
  and linking the built library too would define every symbol twice. `ramble_shared` is the
  same anchor built with `RAMBLE_BUILD_SHARED` and hidden visibility, `EXCLUDE_FROM_ALL` in a
  subproject, and it exports `RAMBLE_LINK_SHARED` to whatever links it.
- `dist/ramble.c` and `dist/ramble.cpp` are the generated anchors, two lines each. They are
  what `ramble_host`, the native plugin builds and the bridge compile, so the define lives
  in one generated place instead of a hand written file per consumer.
- Everything but the library is top level only: the packer target, the programs, the
  explorer, the bridge, the `bin/` output dir and the multi config defaults. The test is
  `CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR`, since `PROJECT_IS_TOP_LEVEL` wants
  cmake 3.21 and the root asks for 3.15. Two of those really did reach a consumer: the
  config type list is a FORCEd cache entry so it overwrote the consumer's own, and the
  explorer and bridge defaults had it fetch SDL3, IXWebSocket and libdatachannel. `dist/`
  is committed, so a consumer never runs the packer and never reads `src/`.
- `install()` copies the four `dist/` headers into `include/` and writes an export set plus
  `ramble-config.cmake` (from `tools/ramble-config.cmake.in`) into `share/cmake/ramble`, so
  `find_package(ramble CONFIG)` hands over the same two targets. The config finds Threads
  before the targets file, which names `Threads::Threads`.
- The version lives once, in `VERSION` at the root: `project()` reads it so the config
  package answers a version request with `SameMinorVersion` compatibility, `Ramble.csproj`
  reads it through an MSBuild property function, `pyproject.toml` through the
  scikit-build-core regex provider, and the Unity packer stamps it into `package.json`.
  A release is a tag `v` plus that number, and the workflow refuses any other tag.
- Three builds, one macro. `src/common/api.h` defines `RAMBLE_API`, which every public
  declaration carries. The default build compiles Ramble into the caller's own binary and
  needs no decoration. `RAMBLE_BUILD_SHARED` marks each entry point exported, so the shared
  library's export table is exactly what the public headers declare and nothing else,
  which is why no binding keeps an export list. `RAMBLE_LINK_SHARED` marks them imported,
  which only Windows needs. GNU toolchains also take `-fvisibility=hidden` so the
  internals stay in. The declaration carries the attribute and the definition in the `.c`
  stays plain: the amalgamation puts both in one translation unit, declaration first.
- `ramble_allocator_heap` and `ramble_heap_realloc` are the process heap as an allocator and
  as a `RambleAllocFn`. They exist so a binding over the shared library never has to reach
  for an internal symbol or route allocation back through its own runtime.
- The `RAMBLE_THREADS` and `RAMBLE_SHM` detection blocks live identically in both
  `platform/core.h` and `transport/core.h`, because every translation unit must agree
  whichever header it saw first. Edit both together. Code guards are `#ifdef RAMBLE_THREADS`,
  never `#ifndef RAMBLE_NO_THREADS`. A new platform layer declares support by implementing
  the thread, mutex, condvar and waker contract and defining `RAMBLE_THREADS` itself.
- The `RAMBLE_IMPLEMENTATION` auto define in `ramble.hpp` is guarded by
  `!defined(__cplusplus)`, so a `.cpp` anchor spells the define itself.
- Bindings: `ramble_shared` is the one native library every binding loads. The top level
  build copies it to `dist/native/<rid>/`, named by .NET runtime identifier, so the NuGet,
  Unity and Python packages read one place. macOS builds it universal under the portable
  `osx` identifier. `tools/unity.cmake` assembles the Unity package and
  `node bridge/client/build.mjs` regenerates the JS client dist. Nothing binary is
  committed.
- The `packages` target builds whichever of `unity_package`, `nuget_package` (needs
  dotnet) and `python_wheel` (needs Python) this machine can, into `dist/`, each over the
  libraries in `dist/native/`. The release workflow runs the same three after merging the
  libraries of every platform.
- CI is two workflows. `build.yml` runs on every push to main and every pull request, on
  linux, windows and macos: configure and build the library and its programs, regenerate
  `dist/` and refuse a diff, build `tools/consumer` from the source tree and from an
  install, pack the NuGet and build the wheel of that platform. The explorer and the
  bridge are off there, since they fetch SDL3 and libdatachannel and are moving out of
  this repo. No test suite runs yet: that waits on the test rework, so a red build means
  a build broke.
- `release.yml` runs on a tag `v*` and refuses one that differs from `VERSION`. Two jobs
  per platform, then one release job. `native` runs cibuildwheel, which builds the wheel
  and, inside it, the library the NuGet and Unity packages bundle, so the Linux libraries
  come out of the manylinux container, aarch64 under QEMU since no arm runner is free on
  a private repo. `programs` builds the explorer, the bridge and the tools into one zip
  per platform, Linux on ubuntu-22.04 for a glibc 2.35 floor. `release` merges the
  libraries, packs the NuGet, assembles the Unity package and the sdist, attaches
  everything to a GitHub Release, pushes the upm branch, and publishes to nuget.org and
  PyPI when the `NUGET_API_KEY` and `PYPI_API_TOKEN` secrets exist.
- `tools/consumer` is a throwaway project that consumes Ramble the way a user does, from the
  source tree by default and from an install with `RAMBLE_CONSUMER_FIND_PACKAGE=ON`. It is
  the only thing that notices when external consumption breaks, which the normal build
  cannot see.
- `.gitignore` binary patterns are anchored to the root. An unanchored `node` once matched
  `src/node/`. Check `git status --ignored` when a new source file will not stage.
