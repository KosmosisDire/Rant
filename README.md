# DART: Discovery And Realtime Transport

A small, dependency-free C99 pub-sub middleware:
- automatic peer discovery over multicast
- realtime optionally-reliable UDP transport
- high-throughput shared-memory communication on localhost

Wrappers for C, C++, Python, C# / .NET, and Unity share one core.

## Install

Every package is on the latest
[GitHub Release](https://github.com/KosmosisDire/DART/releases), and on the registries
once a release has been published there.

### C and C++ with CMake
Fetch DART with CPM (or plain `FetchContent`) and link the target that carries the
platform libraries:
```cmake
CPMAddPackage(NAME dart
              GIT_REPOSITORY https://github.com/KosmosisDire/DART.git
              GIT_TAG v0.0.13)

target_link_libraries(app PRIVATE dart::dart_host)
```
`dart::dart_host` is built for you, so nothing in your project defines
`DART_IMPLEMENTATION`: include `dart.h` and link. Only the library is configured: no tools,
no explorer, no bridge, nothing fetched.
`dart::dart` is the header only core for a target that links its own platform libraries.
An installed DART is `find_package(dart CONFIG REQUIRED)`, same targets. See
[docs/building.md](docs/building.md).

### C and C++ without CMake
Take `dart.h` and `dart.c` (or `dart.hpp` and `dart.cpp`) from the release. Compile that
`.c` or `.cpp` file into your program, it is the one translation unit that emits the
implementation, and link the platform libraries: `-lws2_32 -lbcrypt -lwinmm` on Windows,
`-lrt` on Linux. C++ is C++17.

### Python
```sh
pip install dart-middleware
```
The wheel carries the native library, so nothing compiles on your machine. Before the
package is on PyPI, install the wheel for your platform from the release the same way.

### C# / .NET
```sh
dotnet add package Dart
```
Before the package is on nuget.org, download the `.nupkg` from the release and register
its folder first: `dotnet nuget add source <folder> -n dart`.

### Unity
Add it in the Package Manager (`+`, then "Add package from git URL"):
```
https://github.com/KosmosisDire/DART.git#upm
```
Or download `dart-<version>.unitypackage` from the release and use `Assets > Import Package
> Custom Package` (it imports into `Assets/Dart/`, native plugins included).

### Explorer, bridge and tools
`dart-<version>-<platform>.zip` on the release holds the explorer, the WebSocket bridge and
the command line tools for Windows, Linux and macOS. Unzip and run, nothing installs. The
JS client for the bridge is `dart.mjs`, `dart.js` and `dart.d.ts` on the same page.

## Building from source

Needs CMake 3.15+ and a C99 compiler. This packs `src/` into the `dist/` single-headers
and builds the tools, tests and examples into the repo root:

```sh
cmake -S . -B build
cmake --build build
```

Pass `-DDART_BUILD_TOOLS=OFF` to skip the host programs. `cmake --build build --target
packages` builds every package this machine can into `dist/`, see
[docs/building.md](docs/building.md).
