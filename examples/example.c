/* An interactive chat node. Type sub, pub, pubsub or drop plus a topic name to manage
 * topics, any other line is published as a ChatMsg. docs/building.md lists the flags. */
#define RAMBLE_IMPLEMENTATION
#include "ramble.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>   /* rand: the demo message's filler fields */

#define MAX_TOPICS 32

/* The ChatMsg schema, pasted verbatim by every program on the topic. It exercises every
 * serialization kind and several standard types, so the explorer has structure to show. */
static const char CHAT_SCHEMA[] =
    "Bearing = f32\n"   /* our alias. The newline ends it: a definition ends at its type */
    "ChatMsg"
    "{"
    "    ts:      Timestamp,"            /* Unix epoch microseconds, UTC */
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
    "    heading: Bearing,"              /* our alias: never reads as a bare f32 */
    "    pos:     Double3,"              /* meters */
    "    vel:     Float2,"
    "    at:      Transform,"            /* a translation plus a rotation quaternion */
    "    tint:    Color,"                /* sRGB RGBA bytes */
    "    tags:    string<8>[2],"
    "    path:    f32[],"                /* variable array: a live element count */
    "    text:    string,"               /* variable string: the typed line, unbounded */
    "    extras:  map"                   /* self describing tagged values */
    "}";
static RambleSchema *g_schema;

/* Pack one ChatMsg: the typed line plus every other kind filled with random values, all
 * set by name from the canonical default. Returns the live length to send. */
static uint32_t chat_encode(uint8_t *buf, size_t cap, const char *line, size_t len){
    static uint32_t seq;
    uint8_t path_wire[16], extras[64]; uint32_t fbits;
    int k, n; float pf; RambleMapWriter w;
    ramble_schema_message_default(g_schema, buf, cap);
    ramble_set_int (buf, cap, g_schema, "ts",     ramble_timestamp_now());
    ramble_set_uint(buf, cap, g_schema, "seq",    ++seq);
    ramble_set_uint(buf, cap, g_schema, "ttl",    (uint64_t)(rand() & 0xFFFF));
    ramble_set_uint(buf, cap, g_schema, "hops",   (uint64_t)(rand() & 0xFF));
    ramble_set_int (buf, cap, g_schema, "mood",   (int64_t)(rand() % 201 - 100));
    ramble_set_int (buf, cap, g_schema, "delta",  (int64_t)(rand() % 20001 - 10000));
    ramble_set_int (buf, cap, g_schema, "score",  (int64_t)rand() - RAND_MAX / 2);
    ramble_set_int (buf, cap, g_schema, "drift",  ((int64_t)rand() << 20) - ((int64_t)RAND_MAX << 19));
    ramble_set_f32 (buf, cap, g_schema, "ratio",  (float)rand() / (float)RAND_MAX);
    ramble_set_f64 (buf, cap, g_schema, "weight", 100.0 * (double)rand() / (double)RAND_MAX);
    ramble_set_uint(buf, cap, g_schema, "urgent", (uint64_t)(rand() & 1));
    ramble_set_f32 (buf, cap, g_schema, "heading", (float)(rand() % 3600) / 10.0f);
    ramble_set_f64 (buf, cap, g_schema, "pos.x",  (double)(rand() % 2001 - 1000) / 10.0);
    ramble_set_f64 (buf, cap, g_schema, "pos.y",  (double)(rand() % 2001 - 1000) / 10.0);
    ramble_set_f64 (buf, cap, g_schema, "pos.z",  (double)(rand() % 2001 - 1000) / 10.0);
    ramble_set_f32 (buf, cap, g_schema, "vel.x",  (float)(rand() % 100) / 10.0f);
    ramble_set_f32 (buf, cap, g_schema, "vel.y",  (float)(rand() % 100) / 10.0f);
    {   /* a Transform is a struct of standard types, so its members nest by dotted path.
           The C mirror RambleTransform has the identical layout if you would rather memcpy. */
        RambleTransform at = ramble_transform_identity();
        at.translation = ramble_double3((double)(rand() % 100) / 10.0, 0.0, 0.0);
        ramble_set_f64(buf, cap, g_schema, "at.translation.x", at.translation.x);
        ramble_set_f64(buf, cap, g_schema, "at.rotation.w", at.rotation.w);
    }
    {   RambleColor tint = ramble_color_from_hex(0x3080C0FFu);   /* 0xRRGGBBAA */
        ramble_set_uint(buf, cap, g_schema, "tint.r", tint.r);
        ramble_set_uint(buf, cap, g_schema, "tint.g", tint.g);
        ramble_set_uint(buf, cap, g_schema, "tint.b", tint.b);
        ramble_set_uint(buf, cap, g_schema, "tint.a", tint.a);
    }
    ramble_set_string_at(buf, cap, g_schema, "tags", 0, ramble_cstr((rand() & 1) ? "loud" : "quiet"));
    ramble_set_string_at(buf, cap, g_schema, "tags", 1, ramble_cstr((rand() & 1) ? "red" : "blue"));
    n = rand() % 4;                                      /* variable array: 0 to 3 live f32 */
    for (k = 0; k < n; k++){
        pf = (float)(rand() % 1000) / 10.0f;
        memcpy(&fbits, &pf, 4); i_ramble_le_w32(path_wire + 4 * k, fbits);
    }
    ramble_set_array(buf, cap, g_schema, "path", ramble_bytes(path_wire, (size_t)n * 4));
    ramble_set_string(buf, cap, g_schema, "text", ramble_string(line, len));
    w = ramble_map_begin(extras, sizeof extras);           /* the schema escape hatch */
    ramble_map_put_uint(&w, "battery", (uint64_t)(rand() % 101));
    ramble_map_put_string(&w, "state", ramble_cstr((rand() & 1) ? "docked" : "roaming"));
    ramble_set_map(buf, cap, g_schema, "extras", ramble_bytes(extras, ramble_map_finish(&w)));
    return ramble_schema_msg_len(g_schema, buf, cap);
}

static int g_verbose = 0;     /* --verbose: print discovery and transport events */

/* One topic the user has touched. The pub and sub bits live here because the role enum
 * has no getter. Only the input thread touches this table, so it needs no lock. */
typedef struct {
    char         name[RAMBLE_TOPIC_NAME_MAX + 1];
    RambleTopic *ch;
    int          pub;   /* 1 = we publish on this topic */
    int          sub;   /* 1 = we subscribe to this topic */
} Topic;

static Topic g_topics[MAX_TOPICS];
static int   g_n_topics = 0;

static RambleRole role_of(int pub, int sub){
    if (pub && sub) return RAMBLE_PUBSUB;
    if (pub)        return RAMBLE_PUB_ONLY;
    if (sub)        return RAMBLE_SUB_ONLY;
    return RAMBLE_INACTIVE;
}

static Topic *find_topic(const char *name){
    for (int i = 0; i < g_n_topics; i++)
        if (strcmp(g_topics[i].name, name) == 0) return &g_topics[i];
    return NULL;
}

/* Find the topic, or create it on the node the first time it is named. */
static Topic *get_topic(RambleNode *n, const char *name){
    Topic *t = find_topic(name);
    if (t) return t;
    if (g_n_topics == MAX_TOPICS){ printf("  (topic table full, max %d)\n", MAX_TOPICS); return NULL; }
    if (strlen(name) > RAMBLE_TOPIC_NAME_MAX){ printf("  (topic name too long)\n"); return NULL; }

    RambleTopic *ch = ramble_node_create_topic(n, name, RAMBLE_INACTIVE, g_schema, NULL);
    if (!ch){ printf("  (create topic failed for '%s')\n", name); return NULL; }

    t = &g_topics[g_n_topics++];
    strcpy(t->name, name);
    t->ch = ch; t->pub = 0; t->sub = 0;
    return t;
}

/* Apply a pub or sub change to a topic and push the new role to the transport. */
static void set_role(RambleNode *n, const char *name, int pub, int sub){
    Topic *t = get_topic(n, name);
    if (!t) return;
    t->pub = pub; t->sub = sub;
    ramble_topic_set_role(t->ch, role_of(pub, sub));
    const char *s = pub && sub ? "pubsub" : pub ? "pub" : sub ? "sub" : "drop";
    printf("  [%s] %s\n", s, name);
}

/* Decode a ChatMsg through the schema it arrived with, its length already validated.
 * A schema less message from a raw publisher prints as is. */
static void on_message(const RambleMsg *msg){
    if (msg->schema){
        int64_t      ts   = ramble_get_int(msg->data, msg->schema, "ts");   /* a Timestamp */
        uint64_t     seq  = ramble_get_uint(msg->data, msg->schema, "seq");
        double       hdg  = ramble_get_f32(msg->data, msg->schema, "heading");
        RambleString text = ramble_get_string(msg->data, msg->schema, "text");
        printf("[%.*s] %.*s > %.*s  (#%llu, %.1f deg, +%.2f ms)\n",
               (int)msg->publisher_name.len, msg->publisher_name.data,
               (int)msg->topic_name.len, msg->topic_name.data,
               (int)text.len, text.data ? text.data : "",
               (unsigned long long)seq, hdg,
               /* both clocks are Unix epoch microseconds, so this is one way latency plus
                  clock skew: meaningful on one host, skew bound across machines */
               (double)(ramble_timestamp_now() - ts) / 1000.0);
    } else {
        printf("[%.*s] %.*s > %.*s\n", (int)msg->publisher_name.len, msg->publisher_name.data,
               (int)msg->topic_name.len, msg->topic_name.data,
               (int)msg->data.len, (const char *)msg->data.data);
    }
}

static void on_event(const RambleEvent *ev){
    char line[160];
    if (g_verbose) printf("  <event> %s\n", ramble_event_str(ev, line, sizeof line));
}

/* Handle a command line. Returns 1 if it was a command, 0 if it is plain chat. */
static int handle_command(RambleNode *n, char *line){
    char verb[16], topic[RAMBLE_TOPIC_NAME_MAX + 1];
    int got = sscanf(line, "%15s %64s", verb, topic);
    if (got < 1) return 0;

    if      (strcmp(verb, "sub")    == 0 && got == 2){ Topic *t = find_topic(topic); set_role(n, topic, t ? t->pub : 0, 1); }
    else if (strcmp(verb, "pub")    == 0 && got == 2){ Topic *t = find_topic(topic); set_role(n, topic, 1, t ? t->sub : 0); }
    else if (strcmp(verb, "pubsub") == 0 && got == 2){ set_role(n, topic, 1, 1); }
    else if (strcmp(verb, "drop")   == 0 && got == 2){ set_role(n, topic, 0, 0); }
    else return 0;
    return 1;
}

/* Parse a dotted quad IPv4 address into 4 bytes without the platform socket headers.
 * Returns 1 on success, 0 if malformed. */
static int parse_ipv4(const char *s, uint8_t out[4]){
    int a, b, c, d, n;
    if (sscanf(s, "%d.%d.%d.%d%n", &a, &b, &c, &d, &n) != 4 || s[n] != '\0') return 0;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return 0;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return 1;
}

int main(int argc, char **argv){
    const char *name = NULL;   /* optional node name, NULL = auto "node-XXXXXXXX" */
    const char *ifc  = NULL;   /* --if <ip>: pin multicast to this interface on a multihomed host */
    const char *peer = NULL;   /* --peer <ip>: seed discovery with this address over unicast */
    for (int i = 1; i < argc; i++){
        if      (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) g_verbose = 1;
        else if ((strcmp(argv[i], "--if")   == 0 || strcmp(argv[i], "-i") == 0) && i + 1 < argc) ifc  = argv[++i];
        else if ((strcmp(argv[i], "--peer") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) peer = argv[++i];
        else if (!name) name = argv[i];
    }
    RambleDiscoveryAddr seed;
    if (peer){
        memset(&seed, 0, sizeof seed);
        seed.ip_len = 4;   /* port 0 = discovery_port */
        if (!parse_ipv4(peer, seed.ip)){ fprintf(stderr, "--peer: bad address '%s'\n", peer); return 1; }
    }
    /* compile the shared ChatMsg schema from the allocator the node is about to own. The
     * node copies mem by value at open, so the pool reset in close frees the schema too. */
    RambleAllocator mem = ramble_allocator_heap(0);
    g_schema = ramble_schema_compile(ramble_allocator_alloc, &mem, CHAT_SCHEMA, NULL);
    if (!g_schema || ramble_schema_msg_min(g_schema) > 256){   /* the send buffer below is 512 */
        const char *err = NULL;
        if (!g_schema) ramble_schema_compile(ramble_allocator_alloc, &mem, CHAT_SCHEMA, &err);
        fprintf(stderr, "schema compile failed%s%.40s\n", err ? " near: " : "", err ? err : "");
        return 1;
    }
    srand((unsigned)i_ramble_plat_now_us());   /* the filler fields are random per message */

    RambleNode *n = ramble_node_open(&mem, name, on_message, on_event,
                                 &(RambleNodeOpts){ .max_topics = MAX_TOPICS,
                                                  .net = { .multicast_interface = ifc,
                                                           .seed_peers   = peer ? &seed : NULL,
                                                           .n_seed_peers = peer ? 1 : 0 } });
    if (!n){ fprintf(stderr, "ramble_node_open failed\n"); return 1; }

    printf("commands: sub <topic> | pub <topic> | pubsub <topic> | drop <topic>\n"
           "any other line is published to every topic you pub on. ctrl-d / ctrl-z to quit.\n"
           "%s", g_verbose ? "" : "(run with --verbose to print discovery/transport events)\n");

    ramble_node_start(n);   /* the service thread drives discovery, receive, timers and send */

    char line[256];
    while (fgets(line, sizeof line, stdin)){          /* normal blocking input */
        size_t len = strcspn(line, "\n");             /* drop the trailing newline */
        line[len] = '\0';
        if (!len) continue;
        if (handle_command(n, line)) continue;

        /* plain chat: encode a ChatMsg and publish it to every topic we publish on. Each
           send wakes the service thread, so it flushes now. */
        uint8_t buf[512];   /* msg_min plus this line's variable content, checked at startup */
        int sent = 0;
        uint32_t msg_len = chat_encode(buf, sizeof buf, line, len);
        for (int i = 0; i < g_n_topics; i++)
            if (g_topics[i].pub){ ramble_topic_send(g_topics[i].ch, ramble_bytes(buf, msg_len), NULL); sent++; }
        if (!sent) printf("  (no pub topic yet: try 'pub <topic>' or 'pubsub <topic>')\n");
    }

    ramble_node_close(n, 1);   /* EOF: stops the service thread, pool reset frees g_schema too */
    return 0;
}
