# Building and testing

CMake drives everything. It runs the amalgamation (`src/` to `dist/` through
`tools/pack.cmake`, pure CMake with no compiler needed) and builds the tools, tests and
examples into `bin/`. Release and Debug share `bin/`, so switching overwrites in place.

```sh
cmake -S . -B build          # configure (add -G Ninja for Ninja)
cmake --build build          # re amalgamates dist/ if src/ changed, then builds
```

The OS presets in `CMakePresets.json` also build the explorer. Release is the default.

```sh
cmake --preset windows                 # or linux
cmake --build --preset windows         # Release
cmake --build --preset windows-debug   # Debug, overwrites Release in bin/
```

`build.bat` and `build.sh` do the same with the JS client enabled. Add `debug` for a
Debug build.

Targets: `ramble_test`, `ramble_test_noshm`, `example`, `pubsub`, `if_probe_check`, `dist`
(amalgamate only) and `ramble_shared`, the shared library the C#, Unity and Python packages
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

`unity_package` assembles `dist/com.rant.ramble/` and `dist/ramble-<version>.unitypackage`,
`nuget_package` runs `dotnet pack` for `dist/Ramble.<version>.nupkg`, and `python_wheel`
runs `pip wheel` for this platform's wheel. Each bundles the native libraries in
`dist/native/`, so a local package covers this machine and the release workflow covers
every platform. The version is the `VERSION` file at the root.

Embedded targets (Arduino, ESP32, VxWorks) do not build the host programs. They drop a
`dist/` header into their own project and link the header only `ramble` target, or just
point at `dist/`. Set `-DRAMBLE_BUILD_TOOLS=OFF`. Cross builds default it off.

## From another CMake project

Add Ramble as a subproject and link one of two targets: `ramble::ramble_host` is the core plus
the platform libraries, `ramble::ramble` is the header only core alone, for an embedded target
that links its own platform. Only the library is configured, so nothing is fetched and no
tools, explorer or bridge are built.

```cmake
CPMAddPackage(NAME ramble
              GIT_REPOSITORY https://github.com/KosmosisDire/DART.git
              GIT_TAG v0.0.13)

target_link_libraries(app PRIVATE ramble::ramble_host)
```

`ramble::ramble_host` is a built library, so your project defines nothing and compiles no
anchor. Include `ramble.h` anywhere and link. `ramble::ramble` is the header only alternative,
for an embedded target that compiles the amalgamation itself with its own flags.

Plain `FetchContent_Declare` plus `FetchContent_MakeAvailable(ramble)` gives the same two
targets. A project that exports its own targets against `ramble::` also sets
`RAMBLE_INSTALL=ON`, or its export refuses them.

Or install Ramble once and find it.

```sh
cmake -S . -B build -DRAMBLE_BUILD_TOOLS=OFF -DRAMBLE_BUILD_EXPLORER=OFF -DRAMBLE_BUILD_BRIDGE=OFF
cmake --install build --prefix /usr/local
```

```cmake
find_package(ramble 0.0.13 CONFIG REQUIRED)
target_link_libraries(app PRIVATE ramble::ramble_host)
```

The install is the `dist/` headers in `include/`, the `ramble_host` static library and the
`ramble` shared library in `lib/` (the DLL in `bin/` on Windows), and the config package in
`share/cmake/ramble`.

## Without CMake

Compile a consumer straight against `dist/`. Linux needs `-lrt` for shared memory and
`-lpthread` for threads (folded into libc on modern glibc and musl). macOS and BSD need
neither. Windows threads are in kernel32.

```sh
cc  -std=c99 -Wall -Idist examples/example.c  -o node        -lrt -lpthread
cc  -std=c99 -Wall -Idist tests/ramble_test.c -o ramble_test -lrt -lpthread
gcc -std=c99 -Wall -Idist examples/example.c  -o example.exe -lws2_32 -lbcrypt
gcc -std=c99 -Wall -Idist tests/ramble_test.c -o ramble_test.exe -lws2_32 -lbcrypt -lwinmm
```

`example.c` uses the two header packaging. `ramble_test.c` uses `ramble.h` and needs the
implementation in its own translation unit so its diagnostic `sendto` and `recvfrom`
wrappers can intercept the transport's calls.

The example is an interactive chat node. Type `sub`, `pub`, `pubsub` or `drop` plus a
topic name to manage topics, and any other line is published as a ChatMsg to every topic
you publish on. A first argument names the node. `--verbose` prints every discovery and
transport event, `--if <ip>` pins multicast to an interface on a multihomed host, and
`--peer <ip>` seeds discovery with one address over unicast where multicast is blocked.
`pattern_demo` runs a server node owning a function, a task and a variable next to a
client node that calls them, so the explorer shows every pattern kind live.

## Testing

All test tooling is one C program, `ramble_test`.

```sh
ramble_test selftest                # every selftest phase, exit 0 = pass
ramble_test sweep                   # full mesh RTT vs throughput sweep (10 nodes)
ramble_test sweep --nodes 10 --duration 8 --rates 0,5k,50k,200k
ramble_test sweep --rates 1k,10k --reliable --diag
ramble_test sweep --rates 20k --reliable --extra-ch 500          # 500 idle topics
ramble_test sweep --rates 20k --reliable --extra-ch 500 --spread # load across all of them
ramble_test node n0 11 50000 8      # a single node by hand (name domain hz duration)
ramble_test sendbench               # UDP send cost microbench (Windows only)
ramble_test queuebench              # consumer queue take vs inline callback
```

Run both `ramble_test selftest` and `ramble_test_noshm selftest`. The phases cover loss
events, backpressure, dynamic interest, relaying, NAT, the patterns, retire, churn, the
lapped and ahead rules and the round trip estimator.

`sweep` spawns N `ramble_test node` children per rate, waits for them to exit, and
aggregates their `SUMMARY` lines into a table. A nonzero `Stall ms` column means nodes
lost the CPU mid run, so distrust that row.

## Two machines

```sh
ramble_test serve                           # on machine B
ramble_test sweep --remote --rates 5k,20k   # on machine A, B's nodes join every rate
```

The control plane is Ramble itself on its own domain (default 9, keep `--domain` away from
it). Workers HELLO the coordinator, receive one command per rate so starts are
synchronized, spawn the same N children, and publish their SUMMARY lines back over a
reliable topic. Machines must share a subnet (announce TTL 1). `--mcast 2` leaves
discovery on every real NIC, `--mcast 1` pins it to loopback to isolate a single host
run. `--if <IP>` pins one interface. `--peer <IP>` seeds discovery over unicast where
multicast is broken.

When testing two processes by hand, launch both in one shell command a few hundred
milliseconds apart. The gap between two separate commands looks like a peer drop.
