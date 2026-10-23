/* The test harness: cases, checks, domains, pumps and node helpers. A program lists its
 * cases and ends with TEST_MAIN. spec/testing.md has the rules a test follows. */
#ifndef RANT_HARNESS_H
#define RANT_HARNESS_H

#include <stddef.h>
#include <stdint.h>
#include "rant.h"

/* cases: `prog` runs every case, `prog name...` the named ones, `prog --list` lists them.
   Exit 0 = pass, 1 = a check failed, 77 = every case skipped, 2 = bad arguments */
typedef struct { const char *name; void (*run)(void); } TestCase;
int  test_main(int argc, char **argv, const TestCase *cases, int n);
#define TEST_MAIN(cases) \
    int main(int argc, char **argv){ \
        return test_main(argc, argv, cases, (int)(sizeof cases / sizeof cases[0])); }
/* Marks the running case skipped. The case returns right after. */
void test_skip(const char *why);

/* cond is evaluated exactly once. The message arguments are evaluated on a pass too */
#define CHECK(cond, ...) test_check(!!(cond), __FILE__, __LINE__, __VA_ARGS__)
void test_check(int ok, const char *file, int line, const char *fmt, ...);

/* TEST_DOMAIN to TEST_DOMAIN + 63 belong to this process: ctest hands each program its own
   block through RANT_TEST_DOMAIN, a run by hand picks one from the clock */
extern uint16_t test_domain_base;
#define TEST_DOMAIN test_domain_base
#define TEST_DOMAINS 64

void test_sleep_ms(int ms);
/* Polls a, and b and c when given, for ms and at least one pass. Only a waits, so a pass
   costs about 1 ms */
void test_pump3(RantNode *a, RantNode *b, RantNode *c, int ms);
#define test_pump(a, b, ms) test_pump3((a), (b), NULL, (ms))
/* Pumps until cond holds or ms run out. cond is evaluated again after every pass */
#define PUMP3_UNTIL(a, b, c, ms, cond) do { \
        uint64_t pump_end_ = i_rant_plat_now_us() + (uint64_t)(ms) * 1000u; \
        while (!(cond) && i_rant_plat_now_us() < pump_end_) test_pump3((a), (b), (c), 0); \
    } while (0)
#define PUMP_UNTIL(a, b, ms, cond) PUMP3_UNTIL((a), (b), NULL, (ms), cond)
/* Sleeps until cond holds or ms run out, for nodes on their service threads */
#define WAIT_UNTIL(ms, cond) do { \
        uint64_t wait_end_ = i_rant_plat_now_us() + (uint64_t)(ms) * 1000u; \
        while (!(cond) && i_rant_plat_now_us() < wait_end_) test_sleep_ms(1); \
    } while (0)

/* Opens a node and creates its topics from defs in index order, so a topic index is its
   position in defs. NULL if the open or a create fails */
RantNode *test_node_open(const char *name, RantMsgFn on_msg, RantEventFn on_event,
                         RantNodeOpts opts, const i_RantTopicDef *defs, uint16_t n);
/* the index based call forms over test_node_open's topics */
#define test_send(n, idx, d, l)       rant_topic_send(rant_node_topic((n),(idx)), rant_bytes((d),(l)), NULL)
#define test_set_role(n, idx, r)      rant_topic_set_role(rant_node_topic((n),(idx)), (r))
#define test_drain(n, idx, ms)        rant_topic_drain(rant_node_topic((n),(idx)), (ms))
#define test_match_count(n, idx)      rant_topic_match_count(rant_node_topic((n),(idx)))
#define test_repair_stats(n, idx, o)  rant_topic_repair_stats(rant_node_topic((n),(idx)), (o))

/* A shared sink: per topic index samples, MSG_LOST events and lost counts, every message,
   the last publisher name and the name collisions seen */
extern unsigned long test_samples[8], test_gap_calls[8], test_gap_lost[8], test_messages;
extern unsigned long test_collisions;
extern char test_last_sender[64];
void test_on_message(const RantMsg *msg);
void test_on_event(const RantEvent *ev);

/* 1 when e is a builtin under @rant/. Unfetched, it is known only by its primary hash */
int test_builtin(const RantEntityInfo *e);
/* The first UDP port from port up that binds now. Another program may hold the derived one */
uint16_t test_free_port(uint16_t port);
/* Heap page sources that tag their pages, for nodes on two pools each. A page another source
   is asked to free is counted in test_page_foreign and kept, so a block crossing pools shows */
#define TEST_PAGE_SOURCES 4
extern long long test_page_live[TEST_PAGE_SOURCES], test_page_foreign;
RantPageFn test_page_source(int k);
void       test_page_clear(void);
/* Sets an environment variable, an empty value removes it */
void test_setenv(const char *name, const char *value);
/* Two raw transport cores on one topic for the sans IO cases. The writer knows the reader as
   peer 2, the reader the writer as peer 1. callbacks holds the reader's, NULL for none */
typedef struct {
    i_RantTransportState *w, *r;
    i_RantTopicDef w_def, r_def;
    i_RantTransportConfig w_cfg, r_cfg;
    RantAllocator w_alloc, r_alloc;
    void *w_mem, *r_mem;
} TestPair;
int  test_pair_open(TestPair *p, const char *topic, RantQos wq, RantQos rq,
                    const i_RantTransportConfig *callbacks);
void test_pair_close(TestPair *p);
/* One line of tests/golden/schemas.txt, the schema wire every binding pins */
typedef struct { char text[64]; uint64_t hash; uint8_t wire[256]; size_t wire_len; } TestGolden;
/* Line index of the file, counting only schema lines. 0 past the last one */
int      test_golden(int index, TestGolden *out);
/* The golden hash of a schema text, 0 when no line has it */
uint64_t test_golden_hash(const char *text);
/* A one way link between raw transports: hands src's interest to dst and runs the pairwise
   detail exchange by hand, as the runtimes would */
void test_apply_verified(i_RantTransportState *dst, uint32_t src_id, i_RantTransportState *src);

#endif /* RANT_HARNESS_H */
