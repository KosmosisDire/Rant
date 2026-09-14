# Rant

A small, dependency-free C99 pub-sub middleware:
- automatic peer discovery over multicast
- realtime optionally-reliable UDP transport
- high-throughput shared-memory communication on localhost

Wrappers for C, C++, Python, C# / .NET, and Unity share one core.

## Install

Every package is on the latest
[GitHub Release](https://github.com/KosmosisDire/Rant/releases), and on the registries
once a release has been published there.

### C and C++ with CMake
Fetch Rant with CPM (or plain `FetchContent`) and link the target that carries the
platform libraries:
```cmake
CPMAddPackage(NAME rant
              GIT_REPOSITORY https://github.com/KosmosisDire/Rant.git
              GIT_TAG v0.0.13)

target_link_libraries(app PRIVATE rant::rant_host)
```
`rant::rant_host` is built for you, so nothing in your project defines
`RANT_IMPLEMENTATION`: include `rant.h` and link. Only the library and its generated
headers are configured: no tools, no tests, nothing fetched.
`rant::rant` is the header only core for a target that links its own platform libraries.
An installed Rant is `find_package(rant CONFIG REQUIRED)`, same targets. See
[docs/building.md](docs/building.md).

### C and C++ without CMake
Take `rant.h` and `rant.c` (or `rant.hpp` and `rant.cpp`) from the release. Compile that
`.c` or `.cpp` file into your program, it is the one translation unit that emits the
implementation, and link the platform libraries: `-lws2_32 -lbcrypt -lwinmm` on Windows,
`-lrt` on Linux. C++ is C++17.

### Python
```sh
pip install rant-middleware
```
The wheel carries the native library, so nothing compiles on your machine. Before the
package is on PyPI, install the wheel for your platform from the release the same way.

### C# / .NET
```sh
dotnet add package Rant
```
Before the package is on nuget.org, download the `.nupkg` from the release and register
its folder first: `dotnet nuget add source <folder> -n rant`.

### Unity
Add it in the Package Manager (`+`, then "Add package from git URL"):
```
https://github.com/KosmosisDire/Rant.git#upm
```
Or download `rant-<version>.unitypackage` from the release and use `Assets > Import Package
> Custom Package` (it imports into `Assets/Rant/`, native plugins included).

### Tools
`rant-<version>-<platform>.zip` on the release holds the command line tools for Windows,
Linux and macOS. Unzip and run, nothing installs.

## Building from source

Needs CMake 3.15+ and a C99 compiler. This packs `src/` into the `dist/` single-headers
(not committed, so a checkout between releases generates them) and builds the tools and
tests into `bin/`:

```sh
cmake -S . -B build
cmake --build build
```

Pass `-DRANT_BUILD_TOOLS=OFF` to skip the host programs. `cmake --build build --target
packages` builds every package this machine can into `dist/`, see
[docs/building.md](docs/building.md).
