/* Interactive node: manage topics at runtime by typing commands, then chat.
 *   sub    <topic>   subscribe   (start seeing messages on that topic)
 *   pub    <topic>   publish     (your typed lines get pushed to that topic)
 *   pubsub <topic>   do both
 *   drop   <topic>   stop pub and sub on that topic
 * Any other line is encoded as a ChatMsg (the schema below) and published to every
 * topic you currently publish on; deliveries decode through DartMsg.schema.
 * Run two copies (one host, or two on a LAN) and type in each; pass a node name
 * (e.g. ./node alice) to label who a message came from, --verbose to print
 * discovery/transport events, --if <ip> to pin multicast to a given interface
 * (rarely needed: the interface is auto-detected, but pin it on a multihomed host
 * where the wrong NIC is chosen), and --peer <ip> to seed discovery with a known
 * peer's address over unicast (bootstraps a connection even where multicast is
 * blocked; the peer only needs the IP, discovery replies with its data port). All
 * defaults: best-effort, domain 0, unicast data, multicast discovery.
 *
 * DART runs its own background service thread (dart_node_start), so the main thread
 * just reads stdin with a normal blocking fgets and calls the node directly: every
 * dart_* call is thread-safe, and a send is flushed immediately (the service thread
 * is woken, no tick to wait for).
 *   POSIX  : cc  -std=c99 -Idist examples/example.c -o node -lrt -lpthread
 *   Windows: gcc -std=c99 -Idist examples/example.c -o example.exe -lws2_32 -lbcrypt -lwinmm */
#define DART_IMPLEMENTATION
#include "dart.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>   /* rand: the demo message's filler fields */

#define MAX_TOPICS 32

/* The ChatMsg schema, in the DSL every program using the topic pastes verbatim. It
 * deliberately exercises every serialization kind (all eleven scalars, a scalar array,
 * a string, a string array, a nested struct): the typed line is the text field,
 * everything else is random filler so the explorer has structure to show. Every line
 * typed is encoded through it on send and decoded from DartMsg.schema on delivery;
 * fields are accessed by name (nested members by dotted path: "vel.dx"). */
static const char CHAT_SCHEMA[] =
    "ChatMsg"
    "{"
    "    ts:      u64,"
    "    seq:     u32,"
    "    ttl:     u16,"
    "    hops:    u8,"
    "    mood:    i8,"
    "    delta:   i16,"
    "    score:   i32,"
    "    drift:   i64,"
    "    ratio:   f32,"
    "    weight:  f64,"
    "    urgent:  bool,"
    "    pos:     f64[3],"
    "    vel:     { dx: f32, dy: f32 },"
    "    tags:    string<8>[2],"
    "    text:    string<256>"
    "}";
#define CHAT_TEXT_CAP 256
static DartSchema *g_schema;

/* pack one ChatMsg: the typed line plus every other kind, filled with random values.
 * Start from the canonical default (all zero), then set what we care about, all BY
 * NAME (nested struct members by dotted path). */
static void chat_encode(uint8_t *buf, size_t cap, const char *line, size_t len){
    static uint32_t seq;
    uint8_t pos_wire[24]; uint64_t bits; double p; int k;
    dart_schema_message_default(g_schema, buf, cap);
    dart_set_uint(buf, cap, g_schema, "ts",     i_dart_plat_now_us());
    dart_set_uint(buf, cap, g_schema, "seq",    ++seq);
    dart_set_uint(buf, cap, g_schema, "ttl",    (uint64_t)(rand() & 0xFFFF));
    dart_set_uint(buf, cap, g_schema, "hops",   (uint64_t)(rand() & 0xFF));
    dart_set_int (buf, cap, g_schema, "mood",   (int64_t)(rand() % 201 - 100));
    dart_set_int (buf, cap, g_schema, "delta",  (int64_t)(rand() % 20001 - 10000));
    dart_set_int (buf, cap, g_schema, "score",  (int64_t)rand() - RAND_MAX / 2);
    dart_set_int (buf, cap, g_schema, "drift",  ((int64_t)rand() << 20) - ((int64_t)RAND_MAX << 19));
    dart_set_f32 (buf, cap, g_schema, "ratio",  (float)rand() / (float)RAND_MAX);
    dart_set_f64 (buf, cap, g_schema, "weight", 100.0 * (double)rand() / (double)RAND_MAX);
    dart_set_uint(buf, cap, g_schema, "urgent", (uint64_t)(rand() & 1));
    for (k = 0; k < 3; k++){                             /* f64 array: LE element bytes */
        p = (double)(rand() % 2001 - 1000) / 10.0;
        memcpy(&bits, &p, 8); i_dart_le_w64(pos_wire + 8 * k, bits);
    }
    dart_set_array(buf, cap, g_schema, "pos", dart_bytes(pos_wire, sizeof pos_wire));
    dart_set_f32 (buf, cap, g_schema, "vel.dx", (float)(rand() % 100) / 10.0f);
    dart_set_f32 (buf, cap, g_schema, "vel.dy", (float)(rand() % 100) / 10.0f);
    dart_set_string_at(buf, cap, g_schema, "tags", 0, dart_cstr((rand() & 1) ? "loud" : "quiet"));
    dart_set_string_at(buf, cap, g_schema, "tags", 1, dart_cstr((rand() & 1) ? "red" : "blue"));
    dart_set_string(buf, cap, g_schema, "text", dart_string(line, len));
}

static int g_verbose = 0;     /* --verbose: print discovery/transport events */

/* One topic the user has touched. We track the pub/sub bits locally because the
 * transport role enum has no getter, and we keep the handle to send/re-role it.
 * Only the input thread touches this table, so it needs no lock of its own. */
typedef struct {
    char         name[DART_TOPIC_NAME_MAX + 1];
    DartChannel *ch;
    int          pub;   /* 1 = we publish on this topic */
    int          sub;   /* 1 = we subscribe to this topic */
} Topic;

static Topic g_topics[MAX_TOPICS];
static int   g_n_topics = 0;

static DartRole role_of(int pub, int sub){
    if (pub && sub) return DART_PUBSUB;
    if (pub)        return DART_PUB_ONLY;
    if (sub)        return DART_SUB_ONLY;
    return DART_INACTIVE;
}

static Topic *find_topic(const char *name){
    for (int i = 0; i < g_n_topics; i++)
        if (strcmp(g_topics[i].name, name) == 0) return &g_topics[i];
    return NULL;
}

/* Find the topic, or create it on the node the first time it's named. */
static Topic *get_topic(DartNode *n, const char *name){
    Topic *t = find_topic(name);
    if (t) return t;
    if (g_n_topics == MAX_TOPICS){ printf("  (topic table full, max %d)\n", MAX_TOPICS); return NULL; }
    if (strlen(name) > DART_TOPIC_NAME_MAX){ printf("  (topic name too long)\n"); return NULL; }

    DartChannel *ch = dart_node_create_channel(n, name, DART_INACTIVE, g_schema, NULL);
    if (!ch){ printf("  (create channel failed for '%s')\n", name); return NULL; }

    t = &g_topics[g_n_topics++];
    strcpy(t->name, name);
    t->ch = ch; t->pub = 0; t->sub = 0;
    return t;
}

/* Apply a pub/sub change to a topic and push the new role to the transport. */
static void set_role(DartNode *n, const char *name, int pub, int sub){
    Topic *t = get_topic(n, name);
    if (!t) return;
    t->pub = pub; t->sub = sub;
    dart_channel_set_role(t->ch, role_of(pub, sub));
    const char *s = pub && sub ? "pubsub" : pub ? "pub" : sub ? "sub" : "drop";
    printf("  [%s] %s\n", s, name);
}

/* decode a ChatMsg through the schema the message arrived with (its length is already
 * validated against it). The one-way age is meaningful on one host; across machines it
 * just reflects clock skew. A schema-less message (a raw publisher) prints as-is. */
static void on_message(const DartMsg *msg){
    if (msg->schema){
        uint64_t   ts   = dart_get_uint(msg->data, msg->schema, "ts");
        uint64_t   seq  = dart_get_uint(msg->data, msg->schema, "seq");
        DartString text = dart_get_string(msg->data, msg->schema, "text");
        printf("[%.*s] %.*s > %.*s  (#%llu, +%.2f ms)\n",
               (int)msg->sender_name.len, msg->sender_name.data,
               (int)msg->channel_name.len, msg->channel_name.data,
               (int)text.len, text.data ? text.data : "",
               (unsigned long long)seq,
               (double)(i_dart_plat_now_us() - ts) / 1000.0);
    } else {
        printf("[%.*s] %.*s > %.*s\n", (int)msg->sender_name.len, msg->sender_name.data,
               (int)msg->channel_name.len, msg->channel_name.data,
               (int)msg->data.len, (const char *)msg->data.data);
    }
}

static void on_event(const DartEvent *ev){
    char line[160];
    if (g_verbose) printf("  <event> %s\n", dart_event_str(ev, line, sizeof line));
}

/* Handle a command line. Returns 1 if it was a command, 0 if it's plain chat. */
static int handle_command(DartNode *n, char *line){
    char verb[16], topic[DART_TOPIC_NAME_MAX + 1];
    int got = sscanf(line, "%15s %64s", verb, topic);
    if (got < 1) return 0;

    if      (strcmp(verb, "sub")    == 0 && got == 2){ Topic *t = find_topic(topic); set_role(n, topic, t ? t->pub : 0, 1); }
    else if (strcmp(verb, "pub")    == 0 && got == 2){ Topic *t = find_topic(topic); set_role(n, topic, 1, t ? t->sub : 0); }
    else if (strcmp(verb, "pubsub") == 0 && got == 2){ set_role(n, topic, 1, 1); }
    else if (strcmp(verb, "drop")   == 0 && got == 2){ set_role(n, topic, 0, 0); }
    else return 0;
    return 1;
}

/* parse a dotted-quad IPv4 address into 4 bytes; no dependency on platform socket
 * headers just for this. Returns 1 on success, 0 if malformed. */
static int parse_ipv4(const char *s, uint8_t out[4]){
    int a, b, c, d, n;
    if (sscanf(s, "%d.%d.%d.%d%n", &a, &b, &c, &d, &n) != 4 || s[n] != '\0') return 0;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return 0;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return 1;
}

int main(int argc, char **argv){
    const char *name = NULL;   /* optional node name; NULL => auto "node-XXXXXXXX" */
    const char *ifc  = NULL;   /* --if <ip>: pin multicast to this interface (multihomed hosts) */
    const char *peer = NULL;   /* --peer <ip>: seed discovery with this address over unicast */
    for (int i = 1; i < argc; i++){
        if      (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) g_verbose = 1;
        else if ((strcmp(argv[i], "--if")   == 0 || strcmp(argv[i], "-i") == 0) && i + 1 < argc) ifc  = argv[++i];
        else if ((strcmp(argv[i], "--peer") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) peer = argv[++i];
        else if (!name) name = argv[i];
    }
    DartDiscoveryAddr seed;
    if (peer){
        memset(&seed, 0, sizeof seed);
        seed.ip_len = 4;   /* port 0 = discovery_port */
        if (!parse_ipv4(peer, seed.ip)){ fprintf(stderr, "--peer: bad address '%s'\n", peer); return 1; }
    }
    /* compile the shared ChatMsg schema from the allocator the node is about to own
     * (dart_allocator_alloc is a DartAllocFn; pass &mem as its user). The node copies mem
     * by value at open, so the schema's page rides along and the pool reset in
     * dart_node_close frees it: no explicit dart_schema_free. */
    DartAllocator mem = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    g_schema = dart_schema_compile(dart_allocator_alloc, &mem, CHAT_SCHEMA, NULL);
    if (!g_schema || dart_schema_size(g_schema) > 512){ fprintf(stderr, "schema compile failed\n"); return 1; }
    srand((unsigned)i_dart_plat_now_us());   /* the filler fields are random per message */

    DartNode *n = dart_node_open(&mem, name, on_message, on_event,
                                 &(DartNodeOpts){ .max_channels = MAX_TOPICS,
                                                  .net = { .multicast_interface = ifc,
                                                           .seed_peers   = peer ? &seed : NULL,
                                                           .n_seed_peers = peer ? 1 : 0 } });
    if (!n){ fprintf(stderr, "dart_node_open failed\n"); return 1; }

    printf("commands: sub <topic> | pub <topic> | pubsub <topic> | drop <topic>\n"
           "any other line is published to every topic you pub on. ctrl-d / ctrl-z to quit.\n"
           "%s", g_verbose ? "" : "(run with --verbose to print discovery/transport events)\n");

    dart_node_start(n);   /* the node's service thread drives discovery/RX/timers/TX */

    char line[256];
    while (fgets(line, sizeof line, stdin)){          /* normal blocking input */
        size_t len = strcspn(line, "\n");             /* drop the trailing newline */
        line[len] = '\0';
        if (!len) continue;
        if (handle_command(n, line)) continue;

        /* plain chat: encode a ChatMsg and publish it to every topic we publish on
           (thread-safe; each send wakes the service thread, so TX flushes now) */
        uint8_t buf[512];   /* >= dart_schema_size(g_schema), checked at startup */
        int sent = 0;
        if (len > CHAT_TEXT_CAP) len = CHAT_TEXT_CAP;
        chat_encode(buf, sizeof buf, line, len);
        for (int i = 0; i < g_n_topics; i++)
            if (g_topics[i].pub){ dart_channel_send(g_topics[i].ch, dart_bytes(buf, dart_schema_size(g_schema))); sent++; }
        if (!sent) printf("  (no pub topic yet: try 'pub <topic>' or 'pubsub <topic>')\n");
    }

    dart_node_close(n, 1);   /* EOF: stops the service thread, pool reset frees g_schema too */
    return 0;
}
