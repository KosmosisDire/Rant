/* The C side of an interop pair: `tick pub <seconds>` publishes the Tick schema every
 * language's tick program shares, `tick sub <seconds> <publisher>` exits 0 once twenty
 * ticks from that node decoded right and 1 on a wrong one or when the seconds run out.
 * Loopback, on RANT_DOMAIN. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rant.h"

#define TICK_SCHEMA "Tick { seq: u64, when: Timestamp, value: f64, at: Transform }"
#define TICK_WANT   20

static const RantSchema *g_schema;
static const char *g_from = "";
static int g_good, g_bad;

static int near(double a, double b){ return fabs(a - b) < 1e-9; }

static void on_tick(const RantMsg *m){
    uint64_t seq = rant_get_uint(m->data, g_schema, "seq");
    double angle = (double)seq * 0.01;
    int ok = m->schema != NULL
          && m->publisher_name.len == strlen(g_from)
          && memcmp(m->publisher_name.data, g_from, m->publisher_name.len) == 0
          && rant_get_int(m->data, g_schema, "when") > 0
          && near(rant_get_f64(m->data, g_schema, "value"), sin(angle))
          && near(rant_get_f64(m->data, g_schema, "at.pose.position.x"), cos(angle))
          && near(rant_get_f64(m->data, g_schema, "at.pose.position.y"), sin(angle));
    if (ok) g_good++;
    else if (g_bad++ == 0) printf("FAIL: tick %llu decoded wrong\n", (unsigned long long)seq);
}

int main(int argc, char **argv){
    int sub = argc > 1 && !strcmp(argv[1], "sub");
    double seconds = argc > 2 ? atof(argv[2]) : 10.0;
    if (argc > 3) g_from = argv[3];
    RantAllocator alloc = rant_allocator_heap(0);
    RantNodeOpts opts;
    RantTopicOpts topic_opts;
    RantTopic *topic;
    RantNode *node;
    RantTimestamp start;
    uint64_t seq = 0;
    uint8_t msg[256];
    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&opts, 0, sizeof opts);
    opts.net.multicast_interface = "127.0.0.1";     /* the domain comes from RANT_DOMAIN */
    node = rant_node_open(&alloc, sub ? "c-tick-sub" : "c-tick-pub", sub ? on_tick : NULL, NULL, &opts);
    if (!node){ printf("FAIL: node open\n"); return 1; }
    g_schema = rant_node_schema(node, TICK_SCHEMA);
    memset(&topic_opts, 0, sizeof topic_opts);
    topic_opts.qos.reliability = RANT_RELIABLE;
    topic_opts.qos.keep_last = 64;
    topic = g_schema ? rant_node_create_topic(node, "tick", sub ? RANT_SUB_ONLY : RANT_PUB_ONLY,
                                              g_schema, &topic_opts) : NULL;
    if (!topic){ printf("FAIL: tick topic\n"); rant_node_close(node, 0); return 1; }
    start = rant_timestamp_now();
    while ((double)(rant_timestamp_now() - start) < seconds * 1e6 && !g_bad && g_good < TICK_WANT){
        if (!sub && rant_topic_match_count(topic) > 0){
            double angle = (double)seq * 0.01;
            rant_schema_message_default(g_schema, msg, sizeof msg);
            rant_set_uint(msg, sizeof msg, g_schema, "seq", seq);
            rant_set_int(msg, sizeof msg, g_schema, "when", rant_timestamp_now());
            rant_set_f64(msg, sizeof msg, g_schema, "value", sin(angle));
            rant_set_f64(msg, sizeof msg, g_schema, "at.pose.position.x", cos(angle));
            rant_set_f64(msg, sizeof msg, g_schema, "at.pose.position.y", sin(angle));
            rant_topic_send(topic, rant_bytes(msg, rant_schema_size(g_schema)), NULL);
            seq++;
        }
        rant_node_poll(node, 2);
    }
    rant_node_close(node, 1);
    if (!sub) return 0;
    printf("%s: %d ticks decoded right, %d wrong\n", g_good >= TICK_WANT && !g_bad ? "PASS" : "FAIL",
           g_good, g_bad);
    return g_good >= TICK_WANT && !g_bad ? 0 : 1;
}
