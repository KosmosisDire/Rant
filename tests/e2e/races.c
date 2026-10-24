/* Late and reordered datagrams: a node reads its group socket before its unicast one, and WiFi
 * and the bridge reorder. spec/testing.md says what each case pins. */
#include "support/test.h"
#ifndef _WIN32
  #include <arpa/inet.h>
  #include <netinet/in.h>
#endif

typedef struct { const char *who; RantEventKind kind; uint32_t peer; uint16_t rcv; char name[24]; } RcEvent;
static RcEvent rc_events[512];
static int rc_nevents;
static unsigned rc_got[64], rc_msgs;      /* deliveries by the payload's first byte, and all */

static void rc_on_event(const RantEvent *ev){
    RcEvent *e;
    if (ev->kind != RANT_PEER_UP && ev->kind != RANT_PEER_DOWN && ev->kind != RANT_PEER_INTEREST) return;
    if (rc_nevents >= (int)(sizeof rc_events / sizeof rc_events[0])) return;
    e = &rc_events[rc_nevents++];
    e->who = ev->user ? (const char *)ev->user : "?";
    e->kind = ev->kind; e->peer = ev->peer; e->rcv = ev->receive_topics;
    snprintf(e->name, sizeof e->name, "%s", ev->peer_name ? ev->peer_name : "");
}

static void rc_on_message(const RantMsg *m){
    if (m->data.len >= 1 && m->data.data[0] < 64) rc_got[m->data.data[0]]++;
    rc_msgs++;
}

static void rc_reset(void){
    rc_nevents = 0; rc_msgs = 0;
    memset(rc_got, 0, sizeof rc_got);
    wire_tap_hold_clear();
}

/* the events of node who from first on, printed when print is set */
static int rc_events_of(int first, const char *who, int print){
    int i, n = 0;
    for (i = first; i < rc_nevents; i++){
        const RcEvent *e = &rc_events[i];
        if (strcmp(e->who, who)) continue;
        if (print)
            printf("    %s: %s %s (id %u, receives %u)\n", e->who,
                   e->kind == RANT_PEER_UP ? "peer up" : e->kind == RANT_PEER_DOWN ? "peer down" : "interest",
                   e->name, e->peer, e->rcv);
        n++;
    }
    return n;
}

static RantNodeOpts rc_opts(uint16_t dom, const char *who){
    RantNodeOpts o;
    memset(&o, 0, sizeof o);
    o.domain = dom; o.disable_shm = 1; o.discovery.max_peers = 8;
    o.net.multicast_interface = "127.0.0.1";
    o.user_data = (void *)who;
    return o;
}

static RantNode *rc_open(RantNodeOpts o, const char *name){
    RantAllocator a = rant_allocator_heap(0);
    return rant_node_open(&a, name, rc_on_message, rc_on_event, &o);
}

static RantTopic *rc_topic(RantNode *n, const char *name, RantRole role, int reliable,
                           uint16_t keep_last, uint16_t catch_up){
    RantTopicOpts co;
    memset(&co, 0, sizeof co);
    co.qos.reliability = reliable ? RANT_RELIABLE : RANT_BEST_EFFORT;
    co.qos.keep_last = keep_last; co.qos.catch_up = catch_up;
    return n ? rant_node_create_topic(n, name, role, NULL, &co) : NULL;
}

/* n's record of the peer named name: 1 holding a slot, 0 without one, -1 unknown */
static int rc_known(RantNode *n, const char *name, i_RantDiscoveryPeerView *out){
    const i_RantDiscoveryState *st = i_rant_discovery_state(n->discovery);
    uint16_t i, s, cap = i_rant_discovery_record_cap(st), np = i_rant_discovery_max_peers(st);
    size_t len = strlen(name);
    i_RantDiscoveryPeerView v, w;
    for (i = 0; i < cap; i++){
        if (!i_rant_discovery_known_at(st, i, &v)) continue;
        if (v.name.len != len || memcmp(v.name.data, name, len)) continue;
        if (out) *out = v;
        for (s = 0; s < np; s++)
            if (i_rant_discovery_peer_at(st, s, &w) && w.id == v.id) return 1;
        return 0;
    }
    if (out) memset(out, 0, sizeof *out);
    return -1;
}

/* the publishers a subscriber topic is matched to now. rant_topic_match_count counts subscribers */
static int rc_pubs(RantTopic *t){
    i_RantTopic *tt = t ? i_rant_topic_at(t->n->transport, t->index, NULL) : NULL;
    return tt ? (int)tt->matched_readers : -1;
}

static const char *rc_slot_word(int k){ return k == 1 ? "slotted" : k == 0 ? "known, no slot" : "unknown"; }

/* the hold's target: datagrams to this node's data socket, from this source port unless 0 */
static RantNode *rc_hold_node;
static uint16_t rc_hold_port;

static int rc_hold_from(long long sock, const struct sockaddr *from){
    if (!rc_hold_node || sock != (long long)rc_hold_node->fd || !from || from->sa_family != AF_INET) return 0;
    return !rc_hold_port || ntohs(((const struct sockaddr_in *)from)->sin_port) == rc_hold_port;
}

/* an ACKNACK, delivered now and again later */
static int rc_hold_ack(long long sock, const unsigned char *b, long len, const struct sockaddr *from){
    if (!rc_hold_from(sock, from) || len < WIRE_TAP_HEADER_NACK) return 0;
    return (b[0] & WIRE_TAP_MSG_MASK) == WIRE_TAP_NACK ? 2 : 0;
}

/* A adds a topic and then drops the one B wants, inside one of B's poll gaps. B reads the group
   copies of both first, then the unicast copy of the first, which it must not judge. */
static void older_blob_checks(void){
    RantNodeOpts o = rc_opts(TEST_DOMAIN + 1, "B");
    RantNode *A = NULL, *B; RantTopic *img = NULL, *sub = NULL;
    i_RantDiscoveryPeerView v; int first, events, k;
    rc_reset();
    B = rc_open(o, "ob-b");
    o.user_data = (void *)"A";
    A = rc_open(o, "ob-a");
    img = rc_topic(A, "ob/image", RANT_PUB_ONLY, 0, 0, 0);
    sub = rc_topic(B, "ob/image", RANT_SUB_ONLY, 0, 0, 0);
    CHECK(A && B && img && sub, "older blob: nodes up");
    if (!(A && B && img && sub)) goto done;
    PUMP_UNTIL(A, B, 5000, rc_pubs(sub) == 1 && rant_topic_match_count(img) == 1);
    test_pump(A, B, 800);                                   /* past the opening's repeats */
    k = rc_known(B, "ob-a", &v);
    CHECK(rc_pubs(sub) == 1 && k == 1, "older blob: B matched A (%d publishers, %s)", rc_pubs(sub), rc_slot_word(k));
    first = rc_nevents;
    (void)rc_topic(A, "ob/extra", RANT_PUB_ONLY, 0, 0, 0);  /* still meets B */
    test_pump(A, NULL, 20);
    rant_topic_retire(img);                                 /* meets B no more */
    test_pump(A, NULL, 20);
    rant_node_poll(B, 0);                                   /* one pass reads both changes */
    k = rc_known(B, "ob-a", &v);
    events = rc_events_of(first, "B", 0);
    CHECK(k == 0 && events == 2,
          "older blob: in one pass B judges the new blob once and never goes back (%s at version %u of %u, %d events)",
          rc_slot_word(k), v.meta_version, v.adv_meta_version, events);
    if (k != 0 || events != 2) (void)rc_events_of(first, "B", 1);
done:
    if (A) rant_node_close(A, 1);
    if (B) rant_node_close(B, 1);
}

/* B recreates its subscriber, so its reader has a new epoch, and a late copy of the old
   reader's ACKNACK lands at A. A replays from its join point, which the app never sees. */
static void old_epoch_checks(void){
    RantNodeOpts o = rc_opts(TEST_DOMAIN + 6, "A");
    RantNode *A, *B; RantTopic *pub, *sub; RantRepairStats a0, a1, b0, b1;
    uint8_t pb[8]; int i;
    rc_reset();
    A = rc_open(o, "oe-a");
    pub = rc_topic(A, "oe/beat", RANT_PUB_ONLY, 1, 8, 4);
    o.user_data = (void *)"B";
    B = rc_open(o, "oe-b");
    sub = rc_topic(B, "oe/beat", RANT_SUB_ONLY, 1, 8, 4);
    CHECK(A && B && pub && sub, "old epoch: nodes up");
    if (!(A && B && pub && sub)) goto done;
    PUMP_UNTIL(A, B, 5000, rc_pubs(sub) == 1 && rant_topic_match_count(pub) == 1);
    test_pump(A, B, 1200);                                  /* past the first match window */
    rc_hold_node = A; rc_hold_port = i_rant_plat_local_port(B->fd); wire_tap_hold_if = rc_hold_ack;
    for (i = 1; i <= 8; i++){ memset(pb, 0, sizeof pb); pb[0] = (uint8_t)i; rant_topic_send(pub, rant_bytes(pb, sizeof pb), NULL); }
    PUMP_UNTIL(A, B, 2000, rc_msgs >= 8 && wire_tap_held());
    wire_tap_hold_if = NULL;
    test_pump(A, B, 100);
    CHECK(rc_msgs == 8 && wire_tap_held(), "old epoch: B took 8 samples, a copy of one of its acks kept");
    rant_topic_retire(sub);
    sub = rc_topic(B, "oe/beat", RANT_SUB_ONLY, 1, 8, 4);
    PUMP_UNTIL(A, B, 2000, rc_msgs >= 12);
    test_pump(A, B, 300);
    CHECK(sub && rc_msgs == 12, "old epoch: the new subscriber caught up on the last 4 (%u deliveries)", rc_msgs);
    if (!sub) goto done;
    rant_topic_repair_stats(pub, &a0); rant_topic_repair_stats(sub, &b0);
    wire_tap_release();
    memset(pb, 0, sizeof pb); pb[0] = 9; rant_topic_send(pub, rant_bytes(pb, sizeof pb), NULL);
    test_pump(A, B, 500);
    rant_topic_repair_stats(pub, &a1); rant_topic_repair_stats(sub, &b1);
    CHECK(!wire_tap_held() && rc_msgs == 13 && rc_got[9] == 1,
          "old epoch: the app takes the next sample once (%u deliveries, A sent %llu fragments, B dropped %llu as old)",
          rc_msgs, (unsigned long long)(a1.frags_sent - a0.frags_sent), (unsigned long long)(b1.frags_old - b0.frags_old));
done:
    wire_tap_hold_clear();
    if (A) rant_node_close(A, 1);
    if (B) rant_node_close(B, 1);
}

static const TestCase cases[] = {
    { "older_blob", older_blob_checks },
    { "old_epoch",  old_epoch_checks },
};

TEST_MAIN(cases)
