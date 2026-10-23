/* A live node's receive path: each chunk, a source port and a datagram, goes through the
 * runtime's own dispatch. The wire tap isolates the node, so nothing leaves or arrives. */
#include "support/test.h"
#include "fuzz.h"
#include "read.h"

/* the seed carries the domain, so every run uses this one */
#define FUZZ_RX_DOMAIN 65000

static void rx_on_message(const RantMsg *m){
    fuzz_touch(m->publisher_name.data, m->publisher_name.len);
    fuzz_touch(m->header.data, m->header.len);
    fuzz_touch(m->data.data, m->data.len);
    if (m->schema){
        FUZZ_REQUIRE(rant_schema_validate(m->schema, m->data));
        fuzz_read_message(m->schema, m->data);
    }
}

static void rx_on_request(RantRequest *req, void *user){
    (void)user;
    rant_request_reply(req, req->data);
}

/* the node under test: a typed reliable subscription, a raw topic, a function and a variable.
   Shared memory is off, so a same host peer's payloads ride the datagrams too */
static RantNode *rx_open(RantAllocator *a, uint16_t max_peers){
    RantNodeOpts o;
    RantNode *n;
    memset(&o, 0, sizeof o);
    o.domain = FUZZ_RX_DOMAIN; o.disable_shm = 1; o.discovery.max_peers = max_peers;
    o.net.multicast_interface = "127.0.0.1";
    n = rant_node_open(a, "fuzz-rx", rx_on_message, NULL, &o);
    if (!n) return NULL;
    if (!rant_node_create_topic(n, "fuzz/image", RANT_SUB_ONLY, rant_node_schema(n, "Image"),
            &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE, .keep_last = 8 } })
        || !rant_node_create_topic(n, "fuzz/raw", RANT_PUBSUB, NULL, NULL)
        || !rant_node_create_function_definition(n, "fuzz/echo", rant_node_schema(n, "Transform"),
            rant_node_schema(n, "Transform"), rx_on_request, NULL, NULL)
        || !rant_node_create_variable_definition(n, "fuzz/names", rant_node_schema(n, "JointNames"),
            NULL)){
        rant_node_close(n, 0);
        return NULL;
    }
    return n;
}

static void reflect(RantNode *n){
    RantIter it, eit;
    RantPeerInfo p;
    RantEntityInfo e;
    memset(&it, 0, sizeof it);
    while (rant_node_peers_next(n, &it, &p)){
        fuzz_touch(p.name.data, p.name.len);
        fuzz_touch(p.address.data, p.address.len);
        memset(&eit, 0, sizeof eit);
        while (rant_node_entities_next(n, p.id, &eit, &e)) fuzz_touch(e.name.data, e.name.len);
    }
    memset(&it, 0, sizeof it);
    while (rant_node_mesh_next(n, &it, &e)){
        fuzz_touch(e.name.data, e.name.len);
        if (e.schema){
            RantBytes w = rant_schema_wire(e.schema);
            fuzz_touch(w.data, w.len);
        }
    }
}

/* Each chunk goes to a growing node and to one on a fixed pool with one slot and four
   records, whose directory the input's peers overflow: strangers and the loose queue */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size){
    static const uint8_t lo[4] = { 127, 0, 0, 1 };
    static uint8_t small_mem[1u << 20];
    RantAllocator a = rant_allocator_heap(0), sa = rant_allocator_static(small_mem, sizeof small_mem);
    FuzzIn in;
    const uint8_t *c;
    size_t n;
    RantNode *node[2];
    int k;
    wire_tap_write(&wire_tap.isolate, 1);
    node[0] = rx_open(&a, 8);
    node[1] = rx_open(&sa, 1);
    FUZZ_REQUIRE(node[0] != NULL && node[1] != NULL);
    in.p = data; in.left = size;
    while (fuzz_chunk(&in, &c, &n)){
        if (n < 2) continue;
        for (k = 0; k < 2; k++){
            rant_node_lock(node[k]);
            i_rant_node_rx_one(node[k], node[k]->fd, c + 2, n - 2, lo, (uint16_t)(c[0] | (c[1] << 8)));
            rant_node_unlock(node[k]);
        }
    }
    for (k = 0; k < 2; k++){
        rant_node_poll(node[k], 0);    /* timers, detail requests and repairs, all vanish */
        reflect(node[k]);
        rant_node_close(node[k], 0);
    }
    rant_allocator_reset(&a);
    wire_tap_write(&wire_tap.isolate, 0);
    return 0;
}

/* The seed records what the node receives on both sockets while a live peer matches every
   entity, publishes a small and a fragmented image, calls, writes and leaves */
static FuzzSeed g_session;
static long long g_rx_data, g_rx_disc;
static uint8_t g_chunk[2 + 65536];

static void capture(long long sock, const unsigned char *buf, long len, const struct sockaddr *from){
    const uint8_t *port;
    if ((sock != g_rx_data && sock != g_rx_disc) || len <= 0 || len > 65536) return;
    if (!from || from->sa_family != AF_INET) return;
    port = (const uint8_t *)&((const struct sockaddr_in *)from)->sin_port;   /* network order */
    g_chunk[0] = port[1]; g_chunk[1] = port[0];
    memcpy(g_chunk + 2, buf, (size_t)len);
    fuzz_seed_chunk(&g_session, g_chunk, 2 + (size_t)len);
}

static uint32_t image(const RantSchema *s, uint8_t *buf, size_t cap, size_t bytes){
    static uint8_t pixels[6000];
    memset(pixels, 0x5a, sizeof pixels);
    if (!rant_schema_message_default(s, buf, cap)) return 0;
    rant_set_uint(buf, cap, s, "width", 4);
    rant_set_uint(buf, cap, s, "height", 2);
    rant_set_enum(buf, cap, s, "format", "Rgb8");
    rant_set_array(buf, cap, s, "data", rant_bytes(pixels, bytes));
    return rant_schema_msg_len(s, buf, cap);
}

static void on_response(const RantResponse *r){ (void)r; }

/* The second seed: seven peers that share nothing with the node, the last an observer,
   open around it and leave. They outnumber the small node's records, and their fetches and
   the observer's solicit reach it from sources it forgot */
#define FUZZ_CROWD 7
static void crowd_seed(FuzzEmit emit){
    RantAllocator ra = rant_allocator_heap(0), ca[FUZZ_CROWD];
    RantNode *rx, *crowd[FUZZ_CROWD];
    uint64_t end;
    int k;
    g_session.len = 0;
    rx = rx_open(&ra, 8);
    FUZZ_REQUIRE(rx != NULL);
    g_rx_data = (long long)rx->fd;
    g_rx_disc = (long long)rx->discovery->fd;
    wire_tap_on_recv = capture;
    for (k = 0; k < FUZZ_CROWD; k++){
        RantNodeOpts o; char nm[16], tp[16];
        memset(&o, 0, sizeof o);
        o.domain = FUZZ_RX_DOMAIN; o.disable_shm = 1; o.discovery.max_peers = 8;
        o.net.multicast_interface = "127.0.0.1";
        o.fetch_details = (uint8_t)(k == FUZZ_CROWD - 1);
        sprintf(nm, "fuzz-c%d", k); sprintf(tp, "fuzz/c%d", k);
        ca[k] = rant_allocator_heap(0);
        crowd[k] = rant_node_open(&ca[k], nm, NULL, NULL, &o);
        if (crowd[k]) rant_node_create_topic(crowd[k], tp, RANT_PUB_ONLY, NULL, NULL);
    }
    for (end = i_rant_plat_now_us() + 800000u; i_rant_plat_now_us() < end; ){
        rant_node_poll(rx, 1);
        for (k = 0; k < FUZZ_CROWD; k++) if (crowd[k]) rant_node_poll(crowd[k], 0);
    }
    for (k = 0; k < FUZZ_CROWD; k++) if (crowd[k]) rant_node_close(crowd[k], 1);
    test_pump(rx, NULL, 50);
    wire_tap_on_recv = NULL;
    rant_node_close(rx, 0);
    for (k = 0; k < FUZZ_CROWD; k++) rant_allocator_reset(&ca[k]);
    rant_allocator_reset(&ra);
    FUZZ_REQUIRE(g_session.len > 0);
    emit(g_session.buf, g_session.len);
}

void fuzz_seeds(FuzzEmit emit){
    RantAllocator aa = rant_allocator_heap(0), ba = rant_allocator_heap(0);
    RantNodeOpts o;
    RantNode *rx, *tx = NULL;
    RantTopic *img = NULL, *raw = NULL;
    RantFunction *fn = NULL;
    RantVariable *var = NULL;
    static uint8_t buf[8192];
    uint32_t len;
    g_session.len = 0;
    rx = rx_open(&aa, 8);
    memset(&o, 0, sizeof o);
    o.domain = FUZZ_RX_DOMAIN; o.disable_shm = 1; o.discovery.max_peers = 8;
    o.net.multicast_interface = "127.0.0.1";
    if (rx) tx = rant_node_open(&ba, "fuzz-tx", NULL, NULL, &o);
    if (tx){
        img = rant_node_create_topic(tx, "fuzz/image", RANT_PUB_ONLY, rant_node_schema(tx, "Image"),
                  &(RantTopicOpts){ .qos = { .reliability = RANT_RELIABLE, .keep_last = 8 } });
        raw = rant_node_create_topic(tx, "fuzz/raw", RANT_PUBSUB, NULL, NULL);
        fn = rant_node_create_remote_function(tx, "fuzz/echo", rant_node_schema(tx, "Transform"),
                  rant_node_schema(tx, "Transform"), NULL);
        var = rant_node_create_remote_variable(tx, "fuzz/names", rant_node_schema(tx, "JointNames"), NULL);
    }
    if (img && raw && fn && var){
        const RantSchema *ts = rant_node_schema(tx, "Transform");
        g_rx_data = (long long)rx->fd;
        g_rx_disc = (long long)rx->discovery->fd;
        wire_tap_on_recv = capture;
        PUMP_UNTIL(rx, tx, 8000, rant_topic_match_count(img) > 0 && rant_function_match_count(fn) > 0);
        len = image(rant_topic_schema(img), buf, sizeof buf, 16);
        if (len) rant_topic_send(img, rant_bytes(buf, len), NULL);
        len = image(rant_topic_schema(img), buf, sizeof buf, 6000);
        if (len) rant_topic_send(img, rant_bytes(buf, len), NULL);
        rant_topic_send(raw, rant_bytes("raw bytes", 9), NULL);
        if (rant_schema_message_default(ts, buf, sizeof buf))
            rant_function_call_async(fn, rant_bytes(buf, rant_schema_msg_min(ts)), on_response, NULL, NULL);
        len = rant_schema_msg_min(rant_variable_schema(var));
        if (rant_schema_message_default(rant_variable_schema(var), buf, sizeof buf))
            rant_variable_set(var, rant_bytes(buf, len));
        test_pump(rx, tx, 300);
        rant_node_close(tx, 1);
        tx = NULL;
        test_pump(rx, NULL, 50);
        wire_tap_on_recv = NULL;
    }
    if (tx) rant_node_close(tx, 0);
    if (rx) rant_node_close(rx, 0);
    rant_allocator_reset(&aa);
    rant_allocator_reset(&ba);
    FUZZ_REQUIRE(g_session.len > 0);
    emit(g_session.buf, g_session.len);
    crowd_seed(emit);
}
