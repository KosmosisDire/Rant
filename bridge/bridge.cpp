/* DART WebSocket bridge: one WebSocket connection = one full DART node on the mesh.
 * Text frames are the JSON control plane (open the node, create topics, flip roles,
 * drain); binary frames are the data plane (a fixed 3/7-byte little-endian header plus
 * the raw payload). The protocol is specified in PROTOCOL.md.
 *
 * This is a LEAN pub/sub proxy, not a mesh debugger: it does not expose the peer table
 * or other nodes' schemas. A typed topic carries its own declared schema (both ends
 * paste the same DSL), so the client encodes/decodes with the field table returned by
 * `topic` and never needs to see a peer's layout.
 *
 * Built entirely on the C++ wrapper (dart.hpp): the Conn owns a dart::Node whose
 * handlers (captured lambdas) format deliveries/events straight onto the WebSocket.
 *
 * Threading: IXWebSocket runs each accepted connection on its own thread, which fits
 * the one-node-per-connection model directly. The node is thread-safe (a node-level
 * lock in the C core serializes every call) and runs its own background service thread
 * via Node::start(), so the bridge holds no lock and no poll thread of its own: the
 * connection thread does control ops + publishes, the node's service thread delivers
 * messages/events out of its handlers (IXWebSocket's send is itself thread-safe). */
#include "dart.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

static const int      kProtoVersion = 2;
static const uint8_t  kOpData       = 0x01;      /* the one binary op, both directions */
static size_t         g_max_buffered = 8u << 20; /* drop a client that buffers past this */
static int            g_verbose      = 0;

/* One WebSocket connection = one node. node/ws/name are written only by the
 * connection's own thread (IXWebSocket delivers one connection's messages on one
 * thread); the node's service thread reads them from inside the dart handlers, which
 * the node's teardown fences (Node::~Node joins the service thread before returning). */
struct Conn {
    std::optional<dart::Node> node;
    ix::WebSocket            *ws = nullptr;
    std::string               name;   /* node name (bridge-generated when the client omits it) */
};

static std::mutex g_conns_lock;
static std::unordered_map<std::string, std::shared_ptr<Conn>> g_conns;

/* ---- small formatting helpers ---------------------------------------------------- */

static std::string hex64(uint64_t v){
    char buf[17];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    return buf;
}

static const char *kind_str(dart::FieldType kind){
    switch (kind){
    case dart::FieldType::U8:   return "u8";   case dart::FieldType::U16: return "u16";
    case dart::FieldType::U32:  return "u32";  case dart::FieldType::U64: return "u64";
    case dart::FieldType::I8:   return "i8";   case dart::FieldType::I16: return "i16";
    case dart::FieldType::I32:  return "i32";  case dart::FieldType::I64: return "i64";
    case dart::FieldType::F32:  return "f32";  case dart::FieldType::F64: return "f64";
    case dart::FieldType::Bool: return "bool";
    case dart::FieldType::Array:   return "arr";  case dart::FieldType::Struct: return "struct";
    case dart::FieldType::String:  return "string";
    case dart::FieldType::VString: return "vstring"; case dart::FieldType::VArray: return "varr";
    case dart::FieldType::Map:     return "map";
    default: return "?";
    }
}

static const char *send_status_str(dart::SendStatus rc){
    switch (rc){
    case dart::SendStatus::NoTopic:   return "no such topic";
    case dart::SendStatus::TooBig:      return "message too big";
    case dart::SendStatus::BadRole:     return "topic role cannot publish";
    case dart::SendStatus::OutOfMemory: return "out of memory";
    case dart::SendStatus::State:       return "wrong state";
    case dart::SendStatus::NoSys:       return "not supported";
    default:                            return "send failed";
    }
}

/* The compiled schema as the protocol's field table: every field at every depth with a
 * dotted path and its absolute offset, so a client encodes/decodes with no codegen.
 * Fixed fields carry offset/size; the variable kinds (vstring/varr/map) report 0 and
 * live in the message tail after the fixed section (whose length is the reply's `size`). */
static json fields_json(const dart::Schema &s){
    json fields = json::array();
    std::vector<std::string> parents;   /* enclosing struct names, one per depth level */
    uint16_t n = s.field_count();
    for (uint16_t i = 0; i < n; i++){
        dart::Schema::Field f;
        if (!s.field_at(i, f)) break;
        parents.resize(f.depth);        /* leaving a struct shrinks the path */
        std::string path;
        for (const std::string &p : parents){ path += p; path += '.'; }
        path.append(f.name.data(), f.name.size());
        json row = { {"path", path}, {"kind", kind_str(f.kind)},
                     {"offset", f.offset}, {"size", f.size} };
        if (f.kind == dart::FieldType::Array){ row["elem"] = kind_str(f.elem); row["count"] = f.count; }
        if (f.kind == dart::FieldType::VArray) row["elem"] = kind_str(f.elem);  /* live count, no bound */
        if (f.str_cap) row["cap"] = f.str_cap;                                  /* string + string arrays */
        fields.push_back(row);
        if (f.kind == dart::FieldType::Struct) parents.push_back(std::string(f.name.data(), f.name.size()));
    }
    return fields;
}

static int role_from(const std::string &s, dart::Role *out){
    if (s == "pubsub")   { *out = dart::Role::PubSub;   return 1; }
    if (s == "pub")      { *out = dart::Role::PubOnly;  return 1; }
    if (s == "sub")      { *out = dart::Role::SubOnly;  return 1; }
    if (s == "inactive") { *out = dart::Role::Inactive; return 1; }
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

static void send_error_event(Conn *c, uint16_t topic, dart::SendStatus rc){
    send_json(c, { {"op", "event"}, {"event", "send_error"},
                   {"topic", topic}, {"code", (int)rc}, {"error", send_status_str(rc)} });
}

/* Format one event as JSON and send it. Runs inside the dart event handler (on the
 * node's service thread, or on the connection thread for events a control op triggered).
 * Lean: no address formatting or peer-name lookups (those were mesh-debugger fields). */
static void send_event(Conn *c, const dart::Event &ev){
    if (!c->ws) return;
    json e = { {"op", "event"}, {"text", ev.to_string()} };
    switch (ev.kind()){
    case dart::EventKind::PeerUp:
        e["event"] = "peer_up"; e["peer"] = ev.peer();
        break;
    case dart::EventKind::PeerDown:
        e["event"] = "peer_down"; e["peer"] = ev.peer();
        break;
    case dart::EventKind::PeerInterest:
        e["event"] = "peer_interest"; e["peer"] = ev.peer();
        e["publishes"] = ev.publish_topics(); e["receives"] = ev.receive_topics();
        break;
    case dart::EventKind::MessageLost:
        e["event"] = "msg_lost"; e["topic"] = ev.topic(); e["peer"] = ev.peer();
        e["first"] = ev.lost_first(); e["count"] = ev.lost_count();
        break;
    case dart::EventKind::Error:   /* one catch-all: "text" carries the message, "error" the code */
        e["event"] = "error"; e["error"] = (int)ev.error();
        if (!ev.topic_name().empty()) e["topic_name"] = std::string(ev.topic_name());
        if (ev.topic())               e["topic"]      = ev.topic();
        if (ev.peer())                  e["peer"]         = ev.peer();
        if (ev.os_error())              e["os_error"]     = ev.os_error();
        break;
    default:
        e["event"] = "unknown";
        break;
    }
    send_json(c, e);
}

/* Delivery: one DART message -> one binary WebSocket frame [op][u16 topic][u32 publisher]
 * [payload]. Runs on the node's service thread; drops a client that cannot keep up. */
static void deliver_message(Conn *c, const dart::MessageIn &m){
    if (!c->ws) return;
    if (c->ws->bufferedAmount() > g_max_buffered){
        c->ws->close(1008, "slow consumer");    /* drop rather than buffer forever */
        return;
    }
    dart::Bytes data = m.data();
    uint16_t    ch   = m.topic_index();
    uint32_t    from = m.publisher_id();
    std::string frame;
    frame.resize(7 + data.size());
    uint8_t *p = (uint8_t *)&frame[0];
    p[0] = kOpData;
    p[1] = (uint8_t)(ch & 0xff);   p[2] = (uint8_t)(ch >> 8);
    p[3] = (uint8_t)(from);        p[4] = (uint8_t)(from >> 8);
    p[5] = (uint8_t)(from >> 16);  p[6] = (uint8_t)(from >> 24);
    if (data.size()) memcpy(p + 7, data.data(), data.size());
    c->ws->send(frame, true);
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

    dart::NodeOptions o;
    o.domain              = (uint16_t)req.value("domain", 0);
    o.max_topics        = (uint16_t)req.value("max_topics", 0);
    o.disable_shm         = req.value("disable_shm", false);
    o.multicast_interface = req.value("interface", std::string());
    o.fragment_size       = (uint16_t)req.value("fragment_size", 0);
    o.announce_interval_us = (uint32_t)req.value("announce_interval_ms", 0) * 1000u;
    o.peer_timeout_us      = (uint32_t)req.value("peer_timeout_ms", 0) * 1000u;
    o.max_peers            = (uint16_t)req.value("max_peers", 0);
    for (const auto &s : req.value("seed_peers", std::vector<std::string>{}))
        o.seed_peers.push_back(s);   /* "ip" or "ip:port"; the wrapper parses + rejects bad ones */

    /* handlers capture the stable Conn*; the node's service thread owns them and the
     * Conn always outlives its node (conn_close resets the node first). */
    auto on_msg = [c](const dart::MessageIn &m){ deliver_message(c, m); };
    auto on_evt = [c](const dart::Event    &e){ send_event(c, e); };

    auto node = dart::Node::open(name, on_msg, on_evt, o);
    if (!node){ reply_err(c, seq, "dart_node_open failed: " + dart::Node::last_open_error()); return; }
    c->node = std::move(node);
    c->name = name;
    c->node->start();   /* the node's own service thread drives everything */
    reply_ok(c, seq, { {"proto", kProtoVersion}, {"name", name} });
    if (g_verbose) printf("[bridge] node '%s' opened\n", name.c_str());
}

static void op_topic(Conn *c, const json &req, const json &seq){
    std::string name = req.value("name", "");
    if (name.empty()){ reply_err(c, seq, "missing topic name"); return; }
    dart::Role role;
    if (!role_from(req.value("role", "pubsub"), &role)){ reply_err(c, seq, "bad role"); return; }

    dart::Qos qos;
    qos.reliability          = req.value("reliable", false) ? dart::Reliability::Reliable
                                                            : dart::Reliability::BestEffort;
    qos.keep_last            = (uint16_t)req.value("keep_last", 0);
    qos.catch_up             = (uint16_t)req.value("catch_up", 0);
    qos.max_message_bytes    = (uint32_t)req.value("max_message_bytes", 0);
    qos.heartbeat_us         = (uint32_t)req.value("heartbeat_ms", 0) * 1000u;
    qos.repair_delay_us      = (uint32_t)req.value("repair_delay_ms", 0) * 1000u;
    qos.backpressure_wait_us = (uint32_t)req.value("backpressure_wait_ms", 0) * 1000u;
    qos.shm_max_bytes        = (uint32_t)req.value("shm_max_bytes", 0);

    /* Optional typed schema. create_topic copies the schema into node memory, so the
     * local one need not outlive the topic; we keep it just long enough to reply. */
    std::optional<dart::Schema> schema;
    std::string text = req.value("schema", "");
    if (!text.empty()){
        std::string err;
        schema = dart::Schema::compile(text, &err);
        if (!schema){ reply_err(c, seq, "schema error: " + err); return; }
    }

    dart::Topic ch = c->node->create_topic(name, role, schema ? &*schema : nullptr, qos);
    if (!ch){ reply_err(c, seq, "create failed (topic reserve full, bad name, or OOM)"); return; }

    json r = { {"id", ch.index()} };
    if (schema){
        r["size"]   = schema->size();          /* fixed-section length = where the tail starts */
        r["hash"]   = hex64(schema->hash());
        r["fields"] = fields_json(*schema);
    }
    reply_ok(c, seq, r);
}

static void op_role(Conn *c, const json &req, const json &seq){
    dart::Role role;
    if (!role_from(req.value("role", ""), &role)){ reply_err(c, seq, "bad role"); return; }
    dart::Topic ch = c->node->topic((uint16_t)req.value("topic", 0xffff));
    if (!ch){ reply_err(c, seq, "no such topic"); return; }
    if (ch.set_role(role) != dart::SendStatus::Ok){ reply_err(c, seq, "set_role failed"); return; }
    reply_ok(c, seq, {});
}

static void op_drain(Conn *c, const json &req, const json &seq){
    dart::Topic ch = c->node->topic((uint16_t)req.value("topic", 0xffff));
    if (!ch){ reply_err(c, seq, "no such topic"); return; }
    bool drained = ch.drain(req.value("timeout_ms", 1000));
    reply_ok(c, seq, { {"drained", drained} });
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
    if      (op == "topic") op_topic(c, req, seq);
    else if (op == "role")    op_role(c, req, seq);
    else if (op == "drain")   op_drain(c, req, seq);
    else reply_err(c, seq, "unknown op: " + op);
}

/* ---- data plane: [u8 op][u16 topic][payload] ------------------------------------ */

static void on_binary(Conn *c, const std::string &frame){
    if (frame.size() < 3 || (uint8_t)frame[0] != kOpData) return;   /* reserved ops: drop */
    uint16_t id = (uint16_t)((uint8_t)frame[1] | ((uint8_t)frame[2] << 8));

    if (!c->node){ send_error_event(c, id, dart::SendStatus::NoTopic); return; }
    /* topic() yields an invalid handle for a bad id; send() then returns NoTopic */
    dart::SendStatus rc = c->node->topic(id).send(dart::Bytes(frame.data() + 3, frame.size() - 3));
    if (rc != dart::SendStatus::Ok) send_error_event(c, id, rc);
    /* no explicit flush: the send kicks the node's service thread awake */
}

/* ---- connection lifecycle --------------------------------------------------------- */

static void conn_close(const std::shared_ptr<Conn> &c){
    if (c->node){
        /* ~Node stops + joins the service thread first, so no handler can be mid-flight
           (touching c->ws) once it returns; it closes with a BYE and frees everything */
        c->node.reset();
        if (g_verbose) printf("[bridge] node '%s' closed\n", c->name.c_str());
    }
    c->ws = nullptr;
}

int main(int argc, char **argv){
    int         port = 7480;
    std::string bind = "0.0.0.0";
    for (int i = 1; i < argc; i++){
        if      (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind = argv[++i];
        else if (!strcmp(argv[i], "--max-buffered") && i + 1 < argc) g_max_buffered = (size_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) g_verbose = 1;
        else {
            printf("usage: dart_bridge [--port 7480] [--bind 0.0.0.0] [--max-buffered bytes] [--verbose]\n"
                   "One WebSocket connection = one DART node (pub/sub); see bridge/PROTOCOL.md.\n"
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
