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

Targets: `dart_test`, `dart_test_noshm`, `example`, `pubsub`, `if_probe_check`, and
`dist` (amalgamate only). `pubsub --help` lists that tool's modes and flags. To regenerate
`dist/` without CMake:

```sh
cmake -DSRC=src -DOUT=dist -P tools/pack.cmake
```

Embedded targets (Arduino, ESP32, VxWorks) do not build the host programs. They drop a
`dist/` header into their own project and link the header only `dart` target, or just
point at `dist/`. Set `-DDART_BUILD_TOOLS=OFF`. Cross builds default it off.

## Without CMake

Compile a consumer straight against `dist/`. Linux needs `-lrt` for shared memory and
`-lpthread` for threads (folded into libc on modern glibc and musl). macOS and BSD need
neither. Windows threads are in kernel32.

```sh
cc  -std=c99 -Wall -Idist examples/example.c -o node      -lrt -lpthread
cc  -std=c99 -Wall -Idist tests/dart_test.c  -o dart_test -lrt -lpthread
gcc -std=c99 -Wall -Idist examples/example.c -o example.exe -lws2_32 -lbcrypt
gcc -std=c99 -Wall -Idist tests/dart_test.c  -o dart_test.exe -lws2_32 -lbcrypt -lwinmm
```

`example.c` uses the two header packaging. `dart_test.c` uses `dart.h` and needs the
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

All test tooling is one C program, `dart_test`.

```sh
dart_test selftest                # every selftest phase, exit 0 = pass
dart_test sweep                   # full mesh RTT vs throughput sweep (10 nodes)
dart_test sweep --nodes 10 --duration 8 --rates 0,5k,50k,200k
dart_test sweep --rates 1k,10k --reliable --diag
dart_test sweep --rates 20k --reliable --extra-ch 500          # 500 idle topics
dart_test sweep --rates 20k --reliable --extra-ch 500 --spread # load across all of them
dart_test node n0 11 50000 8      # a single node by hand (name domain hz duration)
dart_test sendbench               # UDP send cost microbench (Windows only)
dart_test queuebench              # consumer queue take vs inline callback
```

Run both `dart_test selftest` and `dart_test_noshm selftest`. The phases cover loss
events, backpressure, dynamic interest, relaying, NAT, the patterns, retire, churn, the
lapped and ahead rules and the round trip estimator.

`sweep` spawns N `dart_test node` children per rate, waits for them to exit, and
aggregates their `SUMMARY` lines into a table. A nonzero `Stall ms` column means nodes
lost the CPU mid run, so distrust that row.

## Two machines

```sh
dart_test serve                           # on machine B
dart_test sweep --remote --rates 5k,20k   # on machine A, B's nodes join every rate
```

The control plane is DART itself on its own domain (default 9, keep `--domain` away from
it). Workers HELLO the coordinator, receive one command per rate so starts are
synchronized, spawn the same N children, and publish their SUMMARY lines back over a
reliable topic. Machines must share a subnet (announce TTL 1). `--mcast 2` leaves
discovery on every real NIC, `--mcast 1` pins it to loopback to isolate a single host
run. `--if <IP>` pins one interface. `--peer <IP>` seeds discovery over unicast where
multicast is broken.

When testing two processes by hand, launch both in one shell command a few hundred
milliseconds apart. The gap between two separate commands looks like a peer drop.
