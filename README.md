# DART: Discovery And Realtime Transport

A small, dependency-free C99 pub-sub middleware:
- automatic peer discovery over multicast
- realtime optionally-reliable UDP transport
- high-throughput shared-memory communication on localhost

Wrappers for C, C++, Python, C# / .NET, and Unity share one core.

## Install

Grab the file(s) for your language from the latest
[GitHub Release](https://github.com/KosmosisDire/DART/releases).

### C (`dart.h`)
Drop `dart.h` into your project and define the implementation in **one** `.c` file:
```c
#define DART_IMPLEMENTATION
#include "dart.h"
```
Link the platform libraries: `-lws2_32 -lbcrypt -lwinmm` (Windows) or `-lrt` (Linux).

### C++ (`dart.hpp`)
Drop `dart.hpp` into your project and define the implementation in **one** `.cpp` file:
```cpp
#define DART_IMPLEMENTATION
#include "dart.hpp"
```
Compile as C++17, and link the same platform libraries as C.

### CMake
Fetch DART with CPM (or plain `FetchContent`) and link the target that carries the
platform libraries:
```cmake
CPMAddPackage(NAME dart
              GIT_REPOSITORY https://github.com/KosmosisDire/DART.git
              GIT_TAG v0.0.12-beta)

target_link_libraries(app PRIVATE dart::dart_host)
```
`dart::dart_host` is built for you, so nothing in your project defines
`DART_IMPLEMENTATION`: include `dart.h` and link. Only the library is configured: no tools,
no explorer, no bridge, nothing fetched.
`dart::dart` is the header only core for a target that links its own platform libraries.
An installed DART is `find_package(dart CONFIG REQUIRED)`, same targets. See
[docs/building.md](docs/building.md).

### Python
```sh
pip install dart-middleware
```
The wheel carries the native library, so nothing compiles on your machine.

### C# / .NET
Put the `.nupkg` in a folder, register it as a local NuGet source, and add the package:
```sh
dotnet nuget add source /path/to/that/folder -n dart
dotnet add package Dart
```

### Unity
Add it in the Package Manager (`+`, then "Add package from git URL"):
```
https://github.com/KosmosisDire/DART.git#upm
```
Or download `dart-<version>.unitypackage` from the release and use `Assets > Import Package
> Custom Package` (it imports into `Assets/Dart/`, native plugins included).

## Building from source

Needs CMake 3.15+ and a C99 compiler. This packs `src/` into the `dist/` single-headers
and builds the tools, tests and examples into the repo root:

```sh
cmake -S . -B build
cmake --build build
```

Pass `-DDART_BUILD_TOOLS=OFF` to skip the host programs. To build the C# / Unity native
libraries and packages yourself, see [csharp/README.md](csharp/README.md).
