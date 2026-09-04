# Testing and measuring

`tests/dart_test.c` holds the selftest phases, the sweep harness and the benches. The
commands are in docs/building.md. This file holds what makes a test or a measurement
trustworthy.

## Harness rules

- `ST_CHECK` evaluates its condition once now, but never put a side effecting or draining
  call inside a check macro.
- Launch paired processes in one shell command. The gap between two tool calls fakes a
  peer drop.
- Both `dart_test selftest` and `dart_test_noshm selftest` must pass, plus a sweep for
  transport changes. A comment only change must leave code byte identical.
- The selftests use `"127.0.0.1"` as the discovery interface to stay off the LAN, and a
  per process domain base so concurrent runs and the SHM module test (segment names carry
  it) do not collide.
- A raw transport `on_message` sees the 8 byte source stamp first. A payload tag sits at
  `data[DART_TIMESTAMP_BYTES]`.
- A wall clock threshold check (waker under 400 ms, varwait under 250 ms) can flake once
  under load. Suspect those before the code.
- A nested wait only pumps the calling node, so a peer in the same process must answer
  from a service thread.
- Default QoS (shallow ring, no backpressure wait) loses bursts legally. Lossless
  multithreaded tests set `keep_last` and `backpressure_wait_us`. A burst that fits one
  datagram parks in `tx_hold` and counts as sent, so a hostile eviction test needs bursts
  bigger than one datagram plus the ring.

## Measurement traps

- Never redirect a child's stdout through a PowerShell pipe. The pipe fills and an
  unbuffered printf in the child blocks for seconds. `dart_test sweep` spawns children
  itself and redirects to files.
- A load generator that repays its backlog invents drops: a stall becomes one burst from
  every node, receive buffers overflow, and the drop percentage reads as stall over
  duration. Check the stall and max gap columns before trusting a row.
- Silent kernel drops are the usual real cause. The default 64 KB receive buffer holds
  about 900 datagrams and Windows drops with no counter moving. Bigger buffers give
  bufferbloat, not a fix. Drain the receive socket to empty every poll: one datagram per
  poll let a churn flood back up and delayed peer detection by tens of seconds.
- Deep replay is by design, so replay receivers must be idempotent. The sweep control
  plane keys on a per run nonce.
- Windows charges a multicast sender about 65 us per local joiner and picks different
  egress interfaces per socket. That measurement is why data is unicast only.

## Measured envelope

The shape of the limits on one host (10 node full mesh, 32 core Windows 11), not current
figures. Per datagram cost dominates, so a 1024 B send costs the same as 70 B. Best effort
sustains 50 kHz per node at zero drops and 0.26 ms round trip. Reliable is clean through
45 kHz per node, and 50 kHz hits the repair ceiling (the 32 seqno NACK window). Mesh
datagrams grow as N squared times the rate. Idle topics cost nothing.

Threaded (`dart_test threadbench`): flat out 688k msg/s UDP and 452k SHM on loopback with
zero evictions. Queued delivery (`queuebench`) costs 14% at 1 KB up to 35% at 1 MB.
Reliable same host SHM at keep_last 4 went from 92 to 2300 msg/s with the immediate in
order ack. A zero subscriber send costs 52 ns. `dart_topic_match_count` costs 11 ns, so an
app that builds an expensive payload should gate on it.

## Live debugging

A `probe.c` that defines `DART_IMPLEMENTATION` against `dist/dart.h` can read every
internal struct (discovery peer flags, observed sources) and wrap `sendto` and `recvfrom`
with macros to log all discovery and detail traffic. Interest lines stuck at a fixed
publish count while liveness continues mean the peer's detail egress is black holed. Do
not trust a peer down time at seconds granularity to tell a BYE from a timeout.

Repair diagnostics (`dart_repair_stats`): `frags_dup` should stay near 0. Rising
`nacks_sent` with a flat reader position is a wedge. `frags_ahead` tracking the live
fragment rate with `msgs_skipped` tracking the message rate was the lapped treadmill.
Reproduce with a pub sub pair on loopback, `disable_shm`, 200 KB at 30 Hz and a queued
subscriber whose consumer sleeps 3 s.
