# DART: Discovery And Realtime Transport

A small, dependency-free C99 pub-pub middleware:
- automatic peer discovery over multicast
- realtime optionally-reliable UDP transport
- High throughput shared memory communication on localhost

# Usage

To use the library:
1. drop the `dist/dart.h` header into your own project.
2. Include it in a single location with:
```c
#define DART_IMPLEMENTATION
#include "dart.h"
```

## Building

Needs CMake 3.15+ and a C99 compiler.

```sh
cmake -S . -B build
cmake --build build
```

This packs `src/` into the `dist/` single-headers and builds the tools,
tests and examples into the repo root. Pass `-DDART_BUILD_TOOLS=OFF` to skip the host programs.