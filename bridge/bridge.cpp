/* DART WebSocket bridge: one WebSocket connection = one full DART node on the mesh.
 * Text frames are the JSON control plane (open the node, create channels, flip roles,
 * query peers); binary frames are the data plane (a fixed 3/7-byte little-endian header
 * plus the raw payload). The protocol is specified in PROTOCOL.md.
 *
 * Threading: IXWebSocket runs each accepted connection on its own thread, which fits
 * the one-node-per-connection model directly. The node itself is thread-safe (a
 * node-level lock in the C core serializes every dart_* call) and runs its own
 * background service thread via dart_node_start, so the bridge holds no lock and no
 * poll thread of its own: the connection thread does control ops + publishes, the
 * node's service thread delivers messages/events straight out of its callbacks
 * (IXWebSocket's send is itself thread-safe). */
#include "dart.h"   /* declarations only; the C99 implementation is dart_impl.c */

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

static const int      kProtoVersion = 1;
static const uint8_t  kOpData       = 0x01;      /* the one binary op, both directions */
static size_t         g_max_buffered = 8u << 20; /* drop a client that buffers past this */
static int            g_verbose      = 0;

/* One WebSocket connection = one node. node/ws/name are written only by the
 * connection's own thread (IXWebSocket delivers one connection's messages on one
 * thread); the node's service thread reads them from inside the dart callbacks,
 * which dart_node_close fences (it joins the service thread before returning). */
struct Conn {
    DartNode      *node = nullptr;
    ix::WebSocket *ws   = nullptr;
    std::string    name;   /* the node name (bridge-generated when the client omits it) */
};

static std::mutex g_conns_lock;
static std::unordered_map<std::string, std::shared_ptr<Conn>> g_conns;

/* ---- small formatting helpers ---------------------------------------------------- */

static std::string addr_str(const uint8_t *ip, uint8_t ip_len, uint16_t port){
    char buf[64];
    if (ip_len == 4) snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);
    else             snprintf(buf, sizeof buf, "[?]:%u", port);   /* IPv6 locators: not yet on the wire */
    return buf;
}

static std::string hex64(uint64_t v){
    char buf[17];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    return buf;
}

static const char *kind_str(uint8_t kind){
    switch (kind){
    case DART_U8: return "u8";   case DART_U16: return "u16";
    case DART_U32: return "u32"; case DART_U64: return "u64";
    case DART_I8: return "i8";   case DART_I16: return "i16";
    case DART_I32: return "i32"; case DART_I64: return "i64";
    case DART_F32: return "f32"; case DART_F64: return "f64";
    case DART_BOOL: return "bool";
    case DART_ARR: return "arr"; case DART_STRUCT: return "struct";
    case DART_STR: return "string";
    case DART_VSTR: return "vstring"; case DART_VARR: return "varr";
    case DART_MAP: return "map";
    default: return "?";
    }
}

static const char *result_str(int rc){
    switch (rc){
    case DART_ERR_NO_CHANNEL: return "no such channel";
    case DART_ERR_TOO_BIG:    return "message too big";
    case DART_ERR_ROLE:       return "channel role cannot publish";
    case DART_ERR_OOM:        return "out of memory";
    default:                  return "send failed";
    }
}

/* The compiled schema as the protocol's field table: every field at every depth with a
 * dotted path and its absolute offset, so a client encodes/decodes with no codegen. */
static json fields_json(const DartSchema *s){
    json fields = json::array();
    std::vector<std::string> parents;   /* enclosing struct names, one per depth level */
    uint16_t n = dart_schema_field_count(s);
    for (uint16_t i = 0; i < n; i++){
        DartSchemaFieldInfo f;
        if (!dart_schema_field_at(s, i, &f)) break;
        parents.resize(f.depth);        /* leaving a struct shrinks the path */
        std::string path;
        for (const std::string &p : parents){ path += p; path += '.'; }
        path.append(f.name.data, f.name.len);
        json row = { {"path", path}, {"kind", kind_str(f.kind)},
                     {"offset", f.offset}, {"size", f.size} };
        if (f.kind == DART_ARR){ row["elem"] = kind_str(f.elem); row["count"] = f.count; }
        if (f.kind == DART_VARR) row["elem"] = kind_str(f.elem);   /* live count, no bound */
        if (f.str_cap) row["cap"] = f.str_cap;                     /* STR + STR-element arrays */
        fields.push_back(row);
        if (f.kind == DART_STRUCT) parents.push_back(std::string(f.name.data, f.name.len));
    }
    return fields;
}

/* A peer's advertised interest list (its topics): one row per direction (a pub and a
 * sub row for a pubsub topic). The announce carries only 32-bit hashes; names and
 * schemas come from the node's greedy detail cache (opts.fetch_details), which fills
 * within an RTT of a peer appearing, so a just-seen topic may briefly show as its hash
 * with no field table. */
static json peer_topics_json(DartNode *node, const DartDiscoveryPeer &p){
    json topics = json::array();
    DartInterestIter it = {}; DartTopic t;
    while (dart_node_peer_interest_next(&p, &it, &t)){
        DartString nm = dart_node_peer_topic_name(node, p.id, t.alias);
        json row;
        if (nm.data) row["name"] = std::string(nm.data, nm.len);
        else {
            char hx[16];
            snprintf(hx, sizeof hx, "0x%08x", (unsigned)t.hash);
            row["name"] = hx;
        }
        row["role"]     = t.is_pub ? "pub" : "sub";
        row["reliable"] = t.reliable != 0;
        if (t.is_pub){
            uint64_t hash = 0;
            const DartSchema *s = dart_node_peer_topic_schema(node, p.id, t.alias, &hash);
            if (hash) row["hash"] = hex64(hash);
            if (s){ row["size"] = dart_schema_size(s); row["fields"] = fields_json(s); }
        }
        topics.push_back(row);
    }
    return topics;
}

/* adopt: parse the schema a live peer advertises for publishing `name`, so a client
 * can join a typed topic it never declared (the explorer's adopt pattern). Reads the
 * greedy detail cache; the returned copy is the caller's (parsed into scratch). */
static DartSchema *adopt_schema(Conn *c, const std::string &name, DartAllocator *scratch){
    uint16_t count = 0;
    DartSchema *found = nullptr;
    /* the peer view is zero-copy: hold the node lock across the walk so the node's
       service thread cannot mutate it mid-read */
    dart_node_lock(c->node);
    const DartDiscoveryPeer *peers = dart_node_peers(c->node, &count);
    for (uint16_t i = 0; !found && peers && i < count; i++){
        DartInterestIter it = {}; DartTopic t;
        while (dart_node_peer_interest_next(&peers[i], &it, &t)){
            DartString nm; const DartSchema *s;
            if (!t.is_pub) continue;
            nm = dart_node_peer_topic_name(c->node, peers[i].id, t.alias);
            if (nm.len != name.size() || memcmp(nm.data, name.data(), name.size()) != 0) continue;
            s = dart_node_peer_topic_schema(c->node, peers[i].id, t.alias, nullptr);
            if (!s) continue;
            {   DartBytes wire = dart_schema_wire(s);
                found = dart_schema_parse(wire.data, wire.len, dart_allocator_alloc, scratch);
                if (found) break;
            }
        }
    }
    dart_node_unlock(c->node);
    return found;
}

static int role_from(const std::string &s, DartRole *out){
    if (s == "pubsub")   { *out = DART_PUBSUB;   return 1; }
    if (s == "pub")      { *out = DART_PUB_ONLY; return 1; }
    if (s == "sub")      { *out = DART_SUB_ONLY; return 1; }
    if (s == "inactive") { *out = DART_INACTIVE; return 1; }
    return 0;
}

/* ---- server -> client sends ------------------------------------------------------- */

static void send_json(Conn *c, const json &j){
    if (c->ws) c->ws->send(j.dump(), false);
}

static void reply_ok(Conn *c, const json &seq, json extra){
    extra["op"] = "reply"; extra["seq"] = seq; extra["ok"] = true;
    send_json(c, extra);
}

static void reply_err(Conn *c, const json &seq, const std::string &error){
    send_json(c, { {"op", "reply"}, {"seq", seq}, {"ok", false}, {"error", error} });
}

static void send_error_event(Conn *c, uint16_t channel, int code){
    send_json(c, { {"op", "event"}, {"event", "send_error"},
                   {"channel", channel}, {"code", code}, {"error", result_str(code)} });
}

/* Peer name lookup off the live table. Called from inside a dart callback, where
 * read-only queries are legal and the table is stable for the callback's duration. */
static std::string peer_name(Conn *c, uint32_t id){
    uint16_t count = 0;
    const DartDiscoveryPeer *peers = dart_node_peers(c->node, &count);
    for (uint16_t i = 0; peers && i < count; i++)
        if (peers[i].id == id && peers[i].name.data)
            return std::string(peers[i].name.data, peers[i].name.len);
    return "";
}

/* Format one event as JSON and send it. Runs inside the dart callback (on the node's
 * service thread, or on the connection thread for events a control op triggered). */
static void send_event(Conn *c, const DartEvent &ev){
    char text[192];
    json e = { {"op", "event"}, {"text", dart_event_str(&ev, text, sizeof text)} };
    switch (ev.kind){
    case DART_PEER_UP:
        e["event"] = "peer_up"; e["peer"] = ev.peer;
        e["addr"] = addr_str(ev.ip, ev.ip_len, ev.port);
        e["name"] = peer_name(c, ev.peer);
        break;
    case DART_PEER_DOWN:
        e["event"] = "peer_down"; e["peer"] = ev.peer;
        e["name"] = peer_name(c, ev.peer);
        break;
    case DART_PEER_INTEREST:
        e["event"] = "peer_interest"; e["peer"] = ev.peer;
        e["publishes"] = ev.publish_topics; e["receives"] = ev.receive_topics;
        break;
    case DART_MSG_LOST:
        e["event"] = "msg_lost"; e["channel"] = ev.channel; e["peer"] = ev.peer;
        e["first"] = ev.lost_first; e["count"] = ev.lost_count;
        break;
    case DART_ERROR:   /* one catch-all: "text" carries the message, "error" the code */
        e["event"] = "error"; e["error"] = (int)ev.error;
        if (ev.channel_name)  e["channel_name"] = ev.channel_name;
        if (ev.channel)       e["channel"]      = ev.channel;
        if (ev.peer)          e["peer"]         = ev.peer;
        if (ev.os_error)      e["os_error"]     = ev.os_error;
        if (ev.ip_len)        e["addr"]         = addr_str(ev.ip, ev.ip_len, ev.port);
        if (ev.too_big_bytes) e["bytes"]        = ev.too_big_bytes;
        if (ev.lost_count){   e["first"] = ev.lost_first; e["count"] = ev.lost_count; }
        if (ev.identity)      e["identity"]     = hex64(ev.identity);
        break;
    default:
        e["event"] = "unknown";
        break;
    }
    send_json(c, e);
}

/* ---- node callbacks (send and read-only dart_* queries are legal here) ------------- */

static void on_dart_msg(const DartMsg *m){
    Conn *c = (Conn *)m->user;
    if (!c->ws) return;
    if (c->ws->bufferedAmount() > g_max_buffered){
        c->ws->close(1008, "slow consumer");    /* drop rather than buffer forever */
        return;
    }
    std::string frame;
    frame.resize(7 + m->data.len);
    uint8_t *p = (uint8_t *)&frame[0];
    p[0] = kOpData;
    p[1] = (uint8_t)(m->channel_id & 0xff); p[2] = (uint8_t)(m->channel_id >> 8);
    p[3] = (uint8_t)(m->sender_id);         p[4] = (uint8_t)(m->sender_id >> 8);
    p[5] = (uint8_t)(m->sender_id >> 16);   p[6] = (uint8_t)(m->sender_id >> 24);
    if (m->data.len) memcpy(p + 7, m->data.data, m->data.len);
    c->ws->send(frame, true);
}

static void on_dart_event(const DartEvent *ev){
    Conn *c = (Conn *)ev->user;
    if (c->ws) send_event(c, *ev);
}

/* ---- control plane ---------------------------------------------------------------- */

static void op_open(Conn *c, const json &req, const json &seq){
    if (c->node){ reply_err(c, seq, "node already open"); return; }

    std::string name = req.value("name", "");
    if (name.empty()){                         /* generate here so the reply can carry it */
        char buf[16];
        snprintf(buf, sizeof buf, "ws-%08x", (unsigned)std::random_device{}());
        name = buf;
    }

    std::vector<DartDiscoveryAddr> seeds;      /* "ip" or "ip:port" strings, IPv4 */
    for (const auto &s : req.value("seed_peers", std::vector<std::string>{})){
        unsigned a, b, d, e, port = 0;
        if (sscanf(s.c_str(), "%u.%u.%u.%u:%u", &a, &b, &d, &e, &port) >= 4 &&
            a < 256 && b < 256 && d < 256 && e < 256 && port < 65536){
            DartDiscoveryAddr sp = {};
            sp.ip[0] = (uint8_t)a; sp.ip[1] = (uint8_t)b; sp.ip[2] = (uint8_t)d; sp.ip[3] = (uint8_t)e;
            sp.ip_len = 4; sp.port = (uint16_t)port;
            seeds.push_back(sp);
        } else { reply_err(c, seq, "bad seed_peers entry: " + s); return; }
    }
    std::string ifc = req.value("interface", "");

    DartNodeOpts o = {};
    o.domain       = (uint16_t)req.value("domain", 0);
    o.max_channels = (uint16_t)req.value("max_channels", 0);
    o.disable_shm  = req.value("disable_shm", false) ? 1 : 0;
    o.user_data    = c;
    o.net.multicast_interface = ifc.empty() ? nullptr : ifc.c_str();
    o.net.fragment_size       = (uint16_t)req.value("fragment_size", 0);
    o.net.seed_peers          = seeds.empty() ? nullptr : seeds.data();
    o.net.n_seed_peers        = (uint16_t)seeds.size();
    o.discovery.announce_interval_us = (uint32_t)req.value("announce_interval_ms", 0) * 1000u;
    o.discovery.peer_timeout_us      = (uint32_t)req.value("peer_timeout_ms", 0) * 1000u;
    o.discovery.max_peers            = (uint16_t)req.value("max_peers", 0);
    o.fetch_details = 1;   /* the protocol exposes peer topics + schemas: fetch them all */

    /* The node copies the allocator by value at open and resets it on close, so a local
     * is enough; "memory" is the dynamic heap cap (0 = the default runaway guard). */
    DartAllocator mem = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    mem.max_bytes = (size_t)req.value("memory", 0);

    c->node = dart_node_open(&mem, name.c_str(), on_dart_msg, on_dart_event, &o);
    if (!c->node){ reply_err(c, seq, "dart_node_open failed"); return; }
    c->name = name;
    dart_node_start(c->node);   /* the node's own service thread drives everything */
    reply_ok(c, seq, { {"proto", kProtoVersion}, {"name", name} });
    if (g_verbose) printf("[bridge] node '%s' opened\n", name.c_str());
}

static void op_channel(Conn *c, const json &req, const json &seq){
    std::string name = req.value("name", "");
    if (name.empty()){ reply_err(c, seq, "missing channel name"); return; }
    DartRole role;
    if (!role_from(req.value("role", "pubsub"), &role)){ reply_err(c, seq, "bad role"); return; }

    DartChannelOpts co = {};
    co.qos.reliability          = req.value("reliable", false) ? DART_RELIABLE : DART_BEST_EFFORT;
    co.qos.keep_last            = (uint16_t)req.value("keep_last", 0);
    co.qos.catch_up             = (uint16_t)req.value("catch_up", 0);
    co.qos.max_message_bytes    = (uint32_t)req.value("max_message_bytes", 0);
    co.qos.heartbeat_us         = (uint32_t)req.value("heartbeat_ms", 0) * 1000u;
    co.qos.repair_delay_us      = (uint32_t)req.value("repair_delay_ms", 0) * 1000u;
    co.qos.backpressure_wait_us = (uint32_t)req.value("backpressure_wait_ms", 0) * 1000u;
    co.qos.shm_max_bytes        = (uint32_t)req.value("shm_max_bytes", 0);

    /* Compile the schema from a scratch allocator: create_channel parses its own copy
     * into node memory, so the scratch is freed right after. */
    DartSchema   *schema = nullptr;
    DartAllocator scratch = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    std::string   text   = req.value("schema", "");
    bool          adopt  = req.value("adopt", false);
    if (!text.empty()){
        const char *err = nullptr;
        schema = dart_schema_compile(dart_allocator_alloc, &scratch, text.c_str(), &err);
        if (!schema){
            size_t at = err ? (size_t)(err - text.c_str()) : 0;
            dart_allocator_reset(&scratch);
            reply_err(c, seq, "schema error at offset " + std::to_string(at));
            return;
        }
    } else if (adopt){
        schema = adopt_schema(c, name, &scratch);   /* none advertised => raw channel */
    }
    DartChannel *ch = dart_node_create_channel(c->node, name.c_str(), role, schema, &co);
    dart_allocator_reset(&scratch);
    if (!ch){ reply_err(c, seq, "create failed (channel reserve full, bad name, or OOM)"); return; }

    json r = { {"id", dart_channel_index(ch)} };
    if (adopt) r["adopted"] = schema != nullptr;
    const DartSchema *s = dart_channel_schema(ch);
    if (s){
        r["size"]   = dart_schema_size(s);
        r["hash"]   = hex64(dart_schema_hash(s));
        r["fields"] = fields_json(s);
    }
    reply_ok(c, seq, r);
}

static void op_role(Conn *c, const json &req, const json &seq){
    DartRole role;
    if (!role_from(req.value("role", ""), &role)){ reply_err(c, seq, "bad role"); return; }
    DartChannel *ch = dart_node_channel(c->node, (uint16_t)req.value("channel", 0xffff));
    if (!ch){ reply_err(c, seq, "no such channel"); return; }
    if (dart_channel_set_role(ch, role) < 0){ reply_err(c, seq, "set_role failed"); return; }
    reply_ok(c, seq, {});
}

static void op_peers(Conn *c, const json &, const json &seq){
    json list = json::array();
    uint16_t count = 0;
    dart_node_lock(c->node);   /* zero-copy view: fence out the node's service thread */
    const DartDiscoveryPeer *peers = dart_node_peers(c->node, &count);
    for (uint16_t i = 0; peers && i < count; i++){
        const DartDiscoveryPeer &p = peers[i];
        list.push_back({ {"id", p.id},
                         {"name", p.name.data ? std::string(p.name.data, p.name.len) : ""},
                         {"addr", addr_str(p.addr.ip, p.addr.ip_len, p.addr.port)},
                         {"active", p.liveness == DART_PEER_ACTIVE},
                         {"frag", dart_node_peer_frag(&p)},
                         {"topics", peer_topics_json(c->node, p)} });
    }
    dart_node_unlock(c->node);
    reply_ok(c, seq, { {"peers", list} });
}

static void op_drain(Conn *c, const json &req, const json &seq){
    DartChannel *ch = dart_node_channel(c->node, (uint16_t)req.value("channel", 0xffff));
    if (!ch){ reply_err(c, seq, "no such channel"); return; }
    int drained = dart_channel_drain(ch, req.value("timeout_ms", 1000));
    reply_ok(c, seq, { {"drained", drained == 1} });
}

static void op_stats(Conn *c, const json &, const json &seq){
    size_t in_use = 0, peak = 0; uint64_t allocs = 0, waited_us = 0; uint32_t waited = 0;
    dart_node_mem_stats(c->node, &in_use, &peak, &allocs);
    dart_node_backpressure_stats(c->node, &waited_us, &waited);
    json r = { {"mem_in_use", in_use}, {"mem_peak", peak}, {"alloc_calls", allocs},
               {"backpressure_waited_us", waited_us}, {"backpressure_waited_sends", waited} };
#ifdef DART_SHM
    uint32_t shm_sent = 0, shm_recv = 0;
    dart_node_shm_stats(c->node, &shm_sent, &shm_recv);
    r["shm_sent"] = shm_sent; r["shm_recv"] = shm_recv;
#endif
    reply_ok(c, seq, r);
}

static void on_text(Conn *c, const std::string &raw){
    json req = json::parse(raw, nullptr, false);
    if (req.is_discarded() || !req.is_object()){
        reply_err(c, 0, "malformed JSON");
        return;
    }
    json seq = req.value("seq", json(0));
    std::string op = req.value("op", "");

    if (op == "open"){ op_open(c, req, seq); return; }
    if (!c->node){ reply_err(c, seq, "send open first"); return; }
    if      (op == "channel") op_channel(c, req, seq);
    else if (op == "role")    op_role(c, req, seq);
    else if (op == "peers")   op_peers(c, req, seq);
    else if (op == "drain")   op_drain(c, req, seq);
    else if (op == "stats")   op_stats(c, req, seq);
    else reply_err(c, seq, "unknown op: " + op);
}

/* ---- data plane: [u8 op][u16 channel][payload] ------------------------------------ */

static void on_binary(Conn *c, const std::string &frame){
    if (frame.size() < 3 || (uint8_t)frame[0] != kOpData) return;   /* reserved ops: drop */
    uint16_t id = (uint16_t)((uint8_t)frame[1] | ((uint8_t)frame[2] << 8));

    if (!c->node){ send_error_event(c, id, DART_ERR_NO_CHANNEL); return; }
    DartChannel *ch = dart_node_channel(c->node, id);
    if (!ch){ send_error_event(c, id, DART_ERR_NO_CHANNEL); return; }
    int rc = dart_channel_send(ch, dart_bytes(frame.data() + 3, frame.size() - 3));
    if (rc < 0) send_error_event(c, id, rc);
    /* no explicit flush: the send kicks the node's service thread awake */
}

/* ---- connection lifecycle --------------------------------------------------------- */

static void conn_close(const std::shared_ptr<Conn> &c){
    if (c->node){
        /* close stops + joins the node's service thread first, so no callback can be
           mid-flight (touching c->ws) once it returns */
        dart_node_close(c->node, 1);   /* BYE + the allocator reset frees everything */
        c->node = nullptr;
        if (g_verbose) printf("[bridge] node '%s' closed\n", c->name.c_str());
    }
    c->ws = nullptr;
}

int main(int argc, char **argv){
    int         port = 7480;
    std::string bind = "127.0.0.1";
    for (int i = 1; i < argc; i++){
        if      (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind = argv[++i];
        else if (!strcmp(argv[i], "--max-buffered") && i + 1 < argc) g_max_buffered = (size_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) g_verbose = 1;
        else {
            printf("usage: dart_bridge [--port 7480] [--bind 127.0.0.1] [--max-buffered bytes] [--verbose]\n"
                   "One WebSocket connection = one DART node; see bridge/PROTOCOL.md.\n"
                   "--bind 0.0.0.0 exposes the bridge (and full mesh access) beyond this host.\n");
            return strcmp(argv[i], "--help") && strcmp(argv[i], "-h") ? 1 : 0;
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);   /* rare lifecycle prints; keep them visible when redirected */
    ix::initNetSystem();
    ix::WebSocketServer server(port, bind);
    server.setOnClientMessageCallback(
        [](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket &ws,
           const ix::WebSocketMessagePtr &msg){
            switch (msg->type){
            case ix::WebSocketMessageType::Open: {
                auto c = std::make_shared<Conn>();
                c->ws = &ws;
                std::lock_guard<std::mutex> g(g_conns_lock);
                g_conns[state->getId()] = c;
                break;
            }
            case ix::WebSocketMessageType::Message: {
                std::shared_ptr<Conn> c;
                {
                    std::lock_guard<std::mutex> g(g_conns_lock);
                    auto it = g_conns.find(state->getId());
                    if (it != g_conns.end()) c = it->second;
                }
                if (!c) break;
                if (msg->binary) on_binary(c.get(), msg->str);
                else             on_text(c.get(), msg->str);
                break;
            }
            case ix::WebSocketMessageType::Close:
            case ix::WebSocketMessageType::Error: {
                std::shared_ptr<Conn> c;
                {
                    std::lock_guard<std::mutex> g(g_conns_lock);
                    auto it = g_conns.find(state->getId());
                    if (it != g_conns.end()){ c = it->second; g_conns.erase(it); }
                }
                if (c) conn_close(c);
                break;
            }
            default:
                break;
            }
        });

    auto res = server.listen();
    if (!res.first){
        fprintf(stderr, "dart_bridge: listen on %s:%d failed: %s\n", bind.c_str(), port, res.second.c_str());
        return 1;
    }
    server.start();
    printf("dart_bridge listening on ws://%s:%d (one connection = one node)\n", bind.c_str(), port);
    server.wait();
    ix::uninitNetSystem();
    return 0;
}
