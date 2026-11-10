/* rant_bench: the latency node, the sweep and its remote worker, and the microbenches. One
 * command per file. main.c compiles the implementation, the rest use the public API. */
#ifndef RANT_BENCH_H
#define RANT_BENCH_H

#include <stddef.h>
#include <stdint.h>
#include "rant.h"
#include "../support/wire_tap.h"

uint64_t bench_now_ns(void);
void     bench_sleep_ms(int ms);
/* Pins a node's discovery to one interface or seeds it with a peer address, either may be
 * NULL. seed must outlive the open. */
void     bench_net(RantNodeOpts *opts, const char *if_ip, const char *peer_ip, RantAddr *seed);

int node_main(int argc, char **argv);
int sweep_main(int argc, char **argv);
int serve_main(int argc, char **argv);
int sendbench_main(void);
int memscale_main(void);
int threadbench_main(void);
int queuebench_main(void);

#endif /* RANT_BENCH_H */
