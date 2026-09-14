# Platform layer

`src/platform/` is the one OS layer. `core.h` is the contract. `core.c` is the Windows and
POSIX implementation (Linux, macOS, the BSDs and ESP-IDF over lwIP) and the only file in
Ramble with an OS ifdef. A runtime speaks only `i_ramble_plat_*` and never touches a sockaddr.
A new platform is one new implementation of the header, and a platform with BSD sockets
needs none. The layer is pure IO, so the sans-IO builds strip it with the runtimes. On non
MSVC Windows link ws2_32, bcrypt and winmm.

## Address convention

An endpoint (send, recv, a peer, a seed) is a `uint8_t ip[4]` plus a host order `uint16_t`
port. A multicast group or an interface address is a `uint32_t` in network byte order,
called a naddr. The two are the same four bytes. `i_ramble_plat_ip4_to_naddr` and
`i_ramble_plat_naddr_to_ip4` move between them.

## Feature flags

Three features share one flag shape: `RAMBLE_SHM`, `RAMBLE_THREADS` and `RAMBLE_PROC_STATS`.
Each is auto detected where the bundled layer provides it, the matching `RAMBLE_NO_*` opt
out always wins, and a new platform layer that implements the contract declares support by
defining the flag itself. Code guards are `#ifdef RAMBLE_THREADS`, never
`#ifndef RAMBLE_NO_THREADS`.

| flag | on by default | the platform needs |
|---|---|---|
| `RAMBLE_SHM` | Windows, Linux, macOS, the BSDs | file mappings, or shm_open and mmap. Older glibc wants `-lrt` |
| `RAMBLE_THREADS` | the same plus ESP-IDF and any libc with pthread.h | thread, mutex, condvar and waker. Older toolchains want `-lpthread` |
| `RAMBLE_PROC_STATS` | the same plus ESP-IDF | GetProcessTimes, getrusage or the ESP heap API |

When `RAMBLE_PROC_STATS` is off the two stats functions are absent and every consumer
compiles out with them, so the meta snapshot omits its proc section and a layer that
cannot measure implements nothing. The SHM and THREADS detection blocks are mirrored in
`transport/core.h` (see spec/build.md).

## Contract notes

- Startup and cleanup. WSAStartup and timeBeginPeriod are refcounted by the OS per
  process, so matched calls pair safely from any thread and Ramble keeps no counter. The 1 ms
  timer matters: the default 15.6 ms tick throttles ACK and repair rates.
  `RAMBLE_NO_HIGHRES_TIMER` skips it.
- Clocks. The monotonic clock is QueryPerformanceCounter or CLOCK_MONOTONIC, microseconds
  from an arbitrary epoch. The wall clock is a separate call for timestamps that compare
  across hosts.
- Entropy. BCryptGenRandom, esp_fill_random, arc4random_buf, or getrandom then
  /dev/urandom. A failure returns 0 and the caller falls back to hostname, pid and time.
  On ESP the hostname is `esp-` plus the factory MAC, since lwIP has no gethostname.
- Heap. `i_ramble_plat_realloc` is the single heap dependency (ptr NULL allocates, size 0
  frees). A target with a custom heap overrides just this.
- Process stats are per process, not per node, so several nodes in one process report the
  same numbers and consumers dedup by pid. Any out pointer may be NULL. A platform may fill
  peak RSS but not current, which then reads 0. On ESP the firmware image is the process:
  RSS is heap in use, peak RSS is the free heap low water mark, and CPU is reported only
  when the ESP timer backed FreeRTOS run time stats are enabled. Heap stats exist only on
  ESP. The others return 0 so callers omit the fields rather than present RSS as a heap.
- Receive. Windows fails an oversized datagram with WSAEMSGSIZE after filling the buffer,
  POSIX delivers the prefix. The layer delivers the prefix on Windows too, so discovery
  reads the intact fixed header and grows and refetches instead of failing the recv.
  `i_ramble_plat_suppress_connreset` stops an ICMP port unreachable from failing the next
  recv on a shared socket (Windows only).
- Poll translates at most 8 descriptors on the stack. Callers poll a few sockets.
- The route probe connects an unbound UDP socket and reads getsockname. No packet leaves.
  It is a diagnostic (`tools/if_probe_check.c`) and discovery routes nothing through it.
- Interfaces. `i_ramble_plat_local_ifaces` returns the up, non loopback IPv4 interfaces with
  netmasks: SIO_GET_INTERFACE_LIST on Windows, getifaddrs on POSIX, and the named netifs
  WIFI_STA_DEF, ETH_DEF and WIFI_AP_DEF on ESP (WiFi must be up before the node opens, and
  STA plus SoftAP reports both). A netmask the platform cannot report is 0 and never
  matches.
- Multicast leave is best effort. An interface that went away may have dropped the
  membership already.

## Threads

Opaque aligned unions keep OS headers out of the header. `core.c` checks the real types fit
with a negative array size typedef, since C99 has no `_Static_assert`. Windows uses SRWLOCK
and CONDITION_VARIABLE: pointer sized, they pair with condvars, and they are not recursive,
which the condvar contract requires. Node reentrancy is an owner id check above this layer.

The condvar wait may wake early or spuriously, so callers loop on a predicate plus a
monotonic deadline. Linux condvars wait on CLOCK_MONOTONIC so a wall clock step cannot
stretch a timeout. macOS uses the relative wait. Other POSIX waits on the realtime clock,
and a jump cuts the wait short, which the caller's loop absorbs. The Windows timeout is
rounded up in 64 bits, since 0xFFFFFFFF plus 999 would wrap and turn the longest waits
into 1 ms spins. On ESP-IDF `pthread_self` aborts from a task not created by
`pthread_create` (the Arduino loopTask that drives poll), so the thread id is the FreeRTOS
task handle.

The waker is a nonblocking UDP socket bound to 127.0.0.1 on an ephemeral port and
connected to itself. connect filters foreign datagrams, repeated signals coalesce in the
socket buffer, and it is the one self pipe that polls on both Windows and POSIX with no
new poll API. How the node uses it is in spec/node.md.

## Shared memory primitives

create maps a fresh named segment, zero filled. attach maps an existing one whole: the
reader learns the size from the OS (VirtualQuery or fstat) and gets it back for detach.
detach unmaps, and the creator passes unlink to remove the OS object. On Windows the object
dies with the last handle. On POSIX the handle carries the name for shm_unlink and a reader
never unlinks. A name is the POSIX `/name` form or a plain Windows object name, derived by
the shm module from the node uuid.

The host id is the Linux machine-id, else a 128 bit hash of the hostname (two FNV-1a
passes with distinct seeds). It is a pre check only. A successful attach is the real gate.
The 64 bit atomics carry acquire and release order for the chunk generation stamp.

## Common helpers

`src/common/` holds the helpers every layer shares: `alloc.h` (spec/allocation.md),
`arena.h` (the measure then build bump packer), `bytes.h` (little endian packing),
`hash.h` (FNV-1a) and `string.h` (`RambleBytes` and `RambleString`). All are static inline, so
a layer compiles standalone and pays nothing for a helper it does not use, and the
amalgamator emits each once per implementation unit.

The FNV basis in `hash.h` is not the textbook one. On wire identities depend on it, so it
never changes. `i_ramble_plat_hash16` in the platform layer uses the textbook basis and is
unrelated.

`common/stdint.h` is a stdint.h for toolchains that ship none: the Wind River and Yaskawa
MotoPlus GCC 4.3 for i586 VxWorks predates the freestanding header. It assumes ILP32 with
a 64 bit long long. It sits on that build's include path only and is never amalgamated,
since the packer inlines only quoted includes.
