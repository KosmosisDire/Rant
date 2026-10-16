# Building and testing

CMake drives everything. It runs the amalgamation (`src/` to `dist/` through
`tools/pack.cmake`, pure CMake with no compiler needed), builds the programs into `bin/` and
the tests into the build tree. Release and Debug share `bin/`, so switching overwrites in
place. `dist/` is not
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

Programs: `rant_bench`, `pubsub` and `if_probe_check`. `pubsub --help` and `rant_bench`
with no command list their modes and flags. `rant_dist` amalgamates only, and `rant_shared`
is the shared library the C#, Unity and Python packages bundle, copied to
`dist/native/<rid>/`.
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

`python_install` builds the wheel and reinstalls it into the Python found first on PATH,
forced since a local build keeps the released version number. It is not part of
`packages`.

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
              GIT_TAG v0.0.24)

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
find_package(rant 0.0.24 CONFIG REQUIRED)
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
cc  -std=c99 -Wall -Idist tools/pubsub.c -o pubsub -lrt -lpthread
gcc -std=c99 -Wall -Idist tools/pubsub.c -o pubsub.exe -lws2_32 -lbcrypt -lwinmm
```

## Version and detection

- `rant_version()` returns the compiled library's version, such as `"0.0.17"`.
  `RANT_VERSION` is the header's. They differ only when a header meets another build's
  shared library.
- Bindings: `rant::version()` in C++, `RantNode.Version` in C#, `rant.version()` in Python.
- Every binary that opens a node carries the bytes `\0RANT-MAGIC <format> v<version>`
  followed by its features and a NUL, for example `RANT-MAGIC 1 v0.0.17 threads shm
  proc_stats patterns stdtypes`. Read the format number first and stop at one you do not
  know. A byte search finds Rant in any executable, library or firmware image:

```sh
grep -a -o "RANT-MAGIC [ -~]*" app.exe
```

## Testing

Every test program is one ctest entry, and ctest runs them in parallel. Build first.

```sh
ctest --preset windows                  # everything (or linux)
ctest --preset windows -L unit          # the sans IO cores, well under a second
ctest --preset windows -R schema        # the entries whose name matches
build/tests/bin/unit_transport rtt      # one case of one program, --list lists them
```

Labels: `unit` (sans IO, no sockets), `e2e` (real nodes on loopback), `noshm` (the twin of a
program built with `RANT_NO_SHM`), `bindings` (the C++, Python and C# suites, the last two
when Python and dotnet are found), `interop` (each language publishing to every other one),
`build` (the implementation compiled with every warning an error), `bench`
(`rant_bench memscale`) and `fuzz` (each fuzz target's seeds and saved crashes). A program
exits 0 on pass, 1 on a failed check and 77 when it skips. Every test stays on loopback, so
the Windows firewall never asks.

### Sanitizers and coverage

Each of these presets builds every program instrumented in its own tree (`build-asan`,
`build-tsan`, `build-coverage`), so `build/` and `bin/` stay as they are. The Python and C#
suites load the staged library, so they run only in the normal build.

```sh
cmake --preset linux-asan && cmake --build --preset linux-asan && ctest --preset linux-asan
cmake --preset windows-asan && cmake --build --preset windows-asan && ctest --preset windows-asan
cmake --preset linux-tsan && cmake --build --preset linux-tsan
setarch $(uname -m) -R ctest --preset linux-tsan
cmake --preset linux-coverage && cmake --build --preset linux-coverage
ctest --preset linux-coverage
gcovr -r . --filter 'dist/rant\.h$' --merge-mode-functions=separate build-coverage
```

- `linux-asan` is AddressSanitizer with UBSan and leak checks, `windows-asan` is MSVC's
  AddressSanitizer.
- `setarch -R` turns off address randomization, which TSan rejects on recent kernels such
  as WSL's with "unexpected memory mapping".
- Every test compiles `dist/rant.h`, so coverage reports its line numbers. Add
  `--html-details build-coverage/coverage.html` for a page per source line.

### Fuzzing

The `linux-fuzz` (Clang) and `windows-fuzz` (MSVC) presets add a libFuzzer program per
target in `tests/fuzz/`: `receive` (datagrams into a live node), `schema` (DSL text and
schema wire) and `message` (typed messages and maps). Start a corpus from the seeds, then
fuzz for as long as you like.

```sh
cmake --preset linux-fuzz && cmake --build --preset linux-fuzz
mkdir -p build-fuzz/corpus/receive
build-fuzz/tests/bin/fuzz_receive_replay --seeds build-fuzz/corpus/receive
build-fuzz/tests/bin/fuzz_receive build-fuzz/corpus/receive -max_total_time=600
```

- A versioned Clang takes `-DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19`
  on the configure line.
- A crash lands as `crash-<hash>` in the working folder. `fuzz_receive_replay <file>`
  reruns it, and once fixed it goes to `tests/fuzz/crashes/receive/` so ctest replays it.
- `-fork=4 -ignore_crashes=1` keeps fuzzing past a crash already known.

## Measuring

`rant_bench` holds the measurements.

```sh
rant_bench sweep                     # full mesh RTT against throughput (10 nodes)
rant_bench sweep --nodes 10 --duration 8 --rates 0,5k,50k,200k
rant_bench sweep --rates 1k,10k --reliable --diag
rant_bench sweep --rates 20k --reliable --extra-ch 500            # 500 idle topics
rant_bench sweep --rates 20k --reliable --extra-ch 500 --spread   # load across all of them
rant_bench node n0 11 50000 8        # one node by hand (name domain hz duration)
rant_bench memscale                  # heap use per send, exit 1 if a steady send allocates
rant_bench threadbench               # the threaded send path
rant_bench queuebench                # callback queue dispatch against inline callbacks
rant_bench sendbench                 # UDP send cost (Windows only)
```

`sweep` spawns N `rant_bench node` children per rate, waits for them to exit, and
aggregates their `SUMMARY` lines into a table. A nonzero `Stall ms` column means nodes
lost the CPU mid run, so distrust that row.

## Two machines

```sh
rant_bench serve                             # on machine B
rant_bench sweep --remote --rates 5k,20k     # on machine A, B's nodes join every rate
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
