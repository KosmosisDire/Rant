# Building and testing

CMake drives everything. It runs the amalgamation (`src/` to `dist/` through
`tools/pack.cmake`, pure CMake with no compiler needed) and builds the tools and tests into
`bin/`. Release and Debug share `bin/`, so switching overwrites in place. `dist/` is not
committed: a build generates it, and each GitHub release attaches the packed headers.

```sh
cmake -S . -B build          # configure (add -G Ninja for Ninja)
cmake --build build          # re amalgamates dist/ if src/ changed, then builds
```

The OS presets in `CMakePresets.json` do the same. Release is the default.

```sh
cmake --preset windows                 # or linux
cmake --build --preset windows         # Release
cmake --build --preset windows-debug   # Debug, overwrites Release in bin/
```

Targets: `rant_test`, `rant_test_noshm`, `pubsub`, `if_probe_check`, `rant_dist`
(amalgamate only) and `rant_shared`, the shared library the C#, Unity and Python packages
bundle, copied to `dist/native/<rid>/`. `pubsub --help` lists that tool's modes and flags.
To regenerate `dist/` without CMake:

```sh
cmake -DSRC=src -DOUT=dist -P tools/pack.cmake
```

## Packages

Every package this machine can build lands in `dist/`:

```sh
cmake --build build --config Release --target packages
```

`unity_package` assembles `dist/com.rant.rant/` and `dist/rant-<version>.unitypackage`,
`nuget_package` runs `dotnet pack` for `dist/Rant.<version>.nupkg`, and `python_wheel`
runs `pip wheel` for this platform's wheel. Each bundles the native libraries in
`dist/native/`, so a local package covers this machine and the release workflow covers
every platform. The version is the `VERSION` file at the root.

Embedded targets (Arduino, ESP32, VxWorks) do not build the host programs. They drop a
`dist/` header (generated here, or from a release) into their own project and link the header only `rant` target, or just
point at `dist/`. Set `-DRANT_BUILD_TOOLS=OFF`. Cross builds default it off.

## From another CMake project

Add Rant as a subproject and link one of two targets: `rant::rant_host` is the core plus
the platform libraries, `rant::rant` is the header only core alone, for an embedded target
that links its own platform. Only the library and the packer that generates its headers are
configured, so nothing is fetched and no tools or tests are built.

```cmake
CPMAddPackage(NAME rant
              GIT_REPOSITORY https://github.com/KosmosisDire/Rant.git
              GIT_TAG v0.0.13)

target_link_libraries(app PRIVATE rant::rant_host)
```

`rant::rant_host` is a built library, so your project defines nothing and compiles no
anchor. Include `rant.h` anywhere and link. `rant::rant` is the header only alternative,
for an embedded target that compiles the amalgamation itself with its own flags.

Plain `FetchContent_Declare` plus `FetchContent_MakeAvailable(rant)` gives the same two
targets. A project that exports its own targets against `rant::` also sets
`RANT_INSTALL=ON`, or its export refuses them.

Or install Rant once and find it.

```sh
cmake -S . -B build -DRANT_BUILD_TOOLS=OFF
cmake --install build --prefix /usr/local
```

```cmake
find_package(rant 0.0.13 CONFIG REQUIRED)
target_link_libraries(app PRIVATE rant::rant_host)
```

The install is the `dist/` headers in `include/`, the `rant_host` static library and the
`rant` shared library in `lib/` (the DLL in `bin/` on Windows), and the config package in
`share/cmake/rant`.

## Without CMake

Compile a consumer straight against `dist/`, from a release or generated with the command
above. Linux needs `-lrt` for shared memory and
`-lpthread` for threads (folded into libc on modern glibc and musl). macOS and BSD need
neither. Windows threads are in kernel32.

```sh
cc  -std=c99 -Wall -Idist tests/rant_test.c -o rant_test -lrt -lpthread
gcc -std=c99 -Wall -Idist tests/rant_test.c -o rant_test.exe -lws2_32 -lbcrypt -lwinmm
```

`rant_test.c` uses `rant.h` and needs the implementation in its own translation unit
so its diagnostic `sendto` and `recvfrom` wrappers can intercept the transport's calls.

## Testing

All test tooling is one C program, `rant_test`.

```sh
rant_test selftest                  # every selftest phase, exit 0 = pass
rant_test sweep                     # full mesh RTT vs throughput sweep (10 nodes)
rant_test sweep --nodes 10 --duration 8 --rates 0,5k,50k,200k
rant_test sweep --rates 1k,10k --reliable --diag
rant_test sweep --rates 20k --reliable --extra-ch 500            # 500 idle topics
rant_test sweep --rates 20k --reliable --extra-ch 500 --spread # load across all of them
rant_test node n0 11 50000 8        # a single node by hand (name domain hz duration)
rant_test sendbench                 # UDP send cost microbench (Windows only)
rant_test queuebench                # consumer queue take vs inline callback
```

Run both `rant_test selftest` and `rant_test_noshm selftest`. The phases cover loss
events, backpressure, dynamic interest, relaying, NAT, the patterns, retire, churn, the
lapped and ahead rules and the round trip estimator.

`sweep` spawns N `rant_test node` children per rate, waits for them to exit, and
aggregates their `SUMMARY` lines into a table. A nonzero `Stall ms` column means nodes
lost the CPU mid run, so distrust that row.

## Two machines

```sh
rant_test serve                             # on machine B
rant_test sweep --remote --rates 5k,20k     # on machine A, B's nodes join every rate
```

The control plane is Rant itself on its own domain (default 9, keep `--domain` away from
it). Workers HELLO the coordinator, receive one command per rate so starts are
synchronized, spawn the same N children, and publish their SUMMARY lines back over a
reliable topic. Machines must share a subnet (announce TTL 1). `--mcast 2` leaves
discovery on every real NIC, `--mcast 1` pins it to loopback to isolate a single host
run. `--if <IP>` pins one interface. `--peer <IP>` seeds discovery over unicast where
multicast is broken.

When testing two processes by hand, launch both in one shell command a few hundred
milliseconds apart. The gap between two separate commands looks like a peer drop.
