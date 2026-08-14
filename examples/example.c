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
 * a capped string + string array, and the variable kinds: an unbounded string, a variable
 * array, a self-describing map) plus several STANDARD TYPES (docs/stdtypes.md): the typed
 * line is the text field, everything else is random filler so the explorer has structure
 * to show. Every line typed is encoded through it on send and decoded from DartMsg.schema
 * on delivery; fields are accessed by name (nested members by dotted path: "vel.x",
 * "at.position.x").
 *
 * `Timestamp`, `Double3`, `Float2`, `Pose` and `Color` are STANDARD names: always in
 * scope, no definition needed, and the name rides the schema (never a message byte) and
 * NARROWS matching, so `at` only ever binds to another Pose. `Bearing = f32` shows the
 * same mechanism for your OWN types: an alias defined right here, distinct from a plain
 * f32 and from anyone else's differently-named f32. (Its `\n` matters: a definition ends
 * where its type ends, so concatenated C literals would lex `f32ChatMsg` as one word.
 * The field lines below self-delimit on their commas.) */
static const char CHAT_SCHEMA[] =
    "Bearing = f32\n"                    /* our own alias: degrees, not radians */
    "ChatMsg"
    "{"
    "    ts:      Timestamp,"            /* Unix-epoch microseconds, UTC */
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
    "    at:      Pose,"                 /* position + orientation quaternion */
    "    tint:    Color,"                /* sRGB RGBA bytes */
    "    tags:    string<8>[2],"
    "    path:    f32[],"                /* variable array: a live element count */
    "    text:    string,"               /* variable string: the typed line, unbounded */
    "    extras:  map"                   /* self-describing tagged values */
    "}";
static DartSchema *g_schema;

/* pack one ChatMsg: the typed line plus every other kind, filled with random values.
 * Start from the canonical default (all zero, empty frames), then set what we care
 * about, all BY NAME (nested struct members by dotted path). Returns the live length
 * to send (variable fields make it per-message). */
static uint32_t chat_encode(uint8_t *buf, size_t cap, const char *line, size_t len){
    static uint32_t seq;
    uint8_t path_wire[16], extras[64]; uint32_t fbits;
    int k, n; float pf; DartMapWriter w;
    dart_schema_message_default(g_schema, buf, cap);
    dart_set_int (buf, cap, g_schema, "ts",     dart_timestamp_now());
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
    dart_set_f32 (buf, cap, g_schema, "heading", (float)(rand() % 3600) / 10.0f);
    dart_set_f64 (buf, cap, g_schema, "pos.x",  (double)(rand() % 2001 - 1000) / 10.0);
    dart_set_f64 (buf, cap, g_schema, "pos.y",  (double)(rand() % 2001 - 1000) / 10.0);
    dart_set_f64 (buf, cap, g_schema, "pos.z",  (double)(rand() % 2001 - 1000) / 10.0);
    dart_set_f32 (buf, cap, g_schema, "vel.x",  (float)(rand() % 100) / 10.0f);
    dart_set_f32 (buf, cap, g_schema, "vel.y",  (float)(rand() % 100) / 10.0f);
    {   /* a Pose is a struct of standard types, so its members nest by dotted path; the
           C mirror DartPose has the identical layout if you would rather memcpy one in */
        DartPose at = dart_pose_identity();
        at.position = dart_double3((double)(rand() % 100) / 10.0, 0.0, 0.0);
        dart_set_f64(buf, cap, g_schema, "at.position.x", at.position.x);
        dart_set_f64(buf, cap, g_schema, "at.orientation.w", at.orientation.w);
    }
    {   DartColor tint = dart_color_from_hex(0x3080C0FFu);   /* 0xRRGGBBAA */
        dart_set_uint(buf, cap, g_schema, "tint.r", tint.r);
        dart_set_uint(buf, cap, g_schema, "tint.g", tint.g);
        dart_set_uint(buf, cap, g_schema, "tint.b", tint.b);
        dart_set_uint(buf, cap, g_schema, "tint.a", tint.a);
    }
    dart_set_string_at(buf, cap, g_schema, "tags", 0, dart_cstr((rand() & 1) ? "loud" : "quiet"));
    dart_set_string_at(buf, cap, g_schema, "tags", 1, dart_cstr((rand() & 1) ? "red" : "blue"));
    n = rand() % 4;                                      /* variable array: 0..3 live f32 */
    for (k = 0; k < n; k++){
        pf = (float)(rand() % 1000) / 10.0f;
        memcpy(&fbits, &pf, 4); i_dart_le_w32(path_wire + 4 * k, fbits);
    }
    dart_set_array(buf, cap, g_schema, "path", dart_bytes(path_wire, (size_t)n * 4));
    dart_set_string(buf, cap, g_schema, "text", dart_string(line, len));
    w = dart_map_begin(extras, sizeof extras);           /* the schema escape hatch */
    dart_map_put_uint(&w, "battery", (uint64_t)(rand() % 101));
    dart_map_put_string(&w, "state", dart_cstr((rand() & 1) ? "docked" : "roaming"));
    dart_set_map(buf, cap, g_schema, "extras", dart_bytes(extras, dart_map_finish(&w)));
    return dart_schema_msg_len(g_schema, buf, cap);
}

static int g_verbose = 0;     /* --verbose: print discovery/transport events */

/* One topic the user has touched. We track the pub/sub bits locally because the
 * transport role enum has no getter, and we keep the handle to send/re-role it.
 * Only the input thread touches this table, so it needs no lock of its own. */
typedef struct {
    char         name[DART_TOPIC_NAME_MAX + 1];
    DartTopic *ch;
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

    DartTopic *ch = dart_node_create_topic(n, name, DART_INACTIVE, g_schema, NULL);
    if (!ch){ printf("  (create topic failed for '%s')\n", name); return NULL; }

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
    dart_topic_set_role(t->ch, role_of(pub, sub));
    const char *s = pub && sub ? "pubsub" : pub ? "pub" : sub ? "sub" : "drop";
    printf("  [%s] %s\n", s, name);
}

/* decode a ChatMsg through the schema the message arrived with (its length is already
 * validated against it). The one-way age is meaningful on one host; across machines it
 * just reflects clock skew. A schema-less message (a raw publisher) prints as-is. */
static void on_message(const DartMsg *msg){
    if (msg->schema){
        int64_t    ts   = dart_get_int(msg->data, msg->schema, "ts");   /* a Timestamp */
        uint64_t   seq  = dart_get_uint(msg->data, msg->schema, "seq");
        double     hdg  = dart_get_f32(msg->data, msg->schema, "heading");
        DartString text = dart_get_string(msg->data, msg->schema, "text");
        printf("[%.*s] %.*s > %.*s  (#%llu, %.1f deg, +%.2f ms)\n",
               (int)msg->publisher_name.len, msg->publisher_name.data,
               (int)msg->topic_name.len, msg->topic_name.data,
               (int)text.len, text.data ? text.data : "",
               (unsigned long long)seq, hdg,
               /* both clocks are Unix-epoch microseconds, so this is one-way latency plus
                  clock skew (meaningful on one host, skew-bound across machines) */
               (double)(dart_timestamp_now() - ts) / 1000.0);
    } else {
        printf("[%.*s] %.*s > %.*s\n", (int)msg->publisher_name.len, msg->publisher_name.data,
               (int)msg->topic_name.len, msg->topic_name.data,
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
    if (!g_schema || dart_schema_msg_min(g_schema) > 256){   /* the send buffer below is 512 */
        const char *err = NULL;
        if (!g_schema) dart_schema_compile(dart_allocator_alloc, &mem, CHAT_SCHEMA, &err);
        fprintf(stderr, "schema compile failed%s%.40s\n", err ? " near: " : "", err ? err : "");
        return 1;
    }
    srand((unsigned)i_dart_plat_now_us());   /* the filler fields are random per message */

    DartNode *n = dart_node_open(&mem, name, on_message, on_event,
                                 &(DartNodeOpts){ .max_topics = MAX_TOPICS,
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
        uint8_t buf[512];   /* >= msg_min + this line's variable content, checked at startup */
        int sent = 0;
        uint32_t msg_len = chat_encode(buf, sizeof buf, line, len);
        for (int i = 0; i < g_n_topics; i++)
            if (g_topics[i].pub){ dart_topic_send(g_topics[i].ch, dart_bytes(buf, msg_len)); sent++; }
        if (!sent) printf("  (no pub topic yet: try 'pub <topic>' or 'pubsub <topic>')\n");
    }

    dart_node_close(n, 1);   /* EOF: stops the service thread, pool reset frees g_schema too */
    return 0;
}
