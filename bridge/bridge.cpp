/* DART WebSocket bridge: one WebSocket connection = one full DART node on the mesh.
 * Text frames are the JSON control plane (open the node, create topics and pattern
 * entities, settle); binary frames are the data plane (a fixed little-endian header plus
 * the raw payload). The protocol is specified in PROTOCOL.md (kProtoVersion below).
 *
 * This is a LEAN proxy, not a mesh debugger: it does not expose the peer table or other
 * nodes' schemas. A typed topic/entity carries its own declared schema (both ends paste
 * the same DSL), so the client encodes/decodes with the field tables the create replies
 * return and never needs to see a peer's layout.
 *
 * Built entirely on the C++ wrapper (dart.hpp): the Conn owns a dart::Node whose
 * handlers (captured lambdas) format deliveries/events straight onto the WebSocket.
 * Patterns ride the wrapper's UNTYPED handles (FunctionDefinition<> etc): the payloads
 * stay raw bytes end to end, the client owns encode/decode.
 *
 * Threading: IXWebSocket runs each accepted connection on its own thread, which fits
 * the one-node-per-connection model directly. The node is thread-safe and runs its own
 * background service thread via Node::start(). Variable values push event-driven off
 * the C on-change hook (on the thread that applied the write); one extra bridge thread
 * per connection (the ticker) recomputes per-entity match state. Discipline: never call
 * into dart while holding Conn::mu (the dart handlers take mu from the service thread,
 * so holding mu across a dart call that wants the node lock would deadlock). */
#include "dart.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

static const int kProtoVersion = 10;

/* Binary frame ops (byte 0). One value space, meaning per direction. EVERY server-to-client
 * data frame ends its header with [u64 written_us], the writer's wall clock at the moment it
 * wrote the message (0 = the publisher opted out, or a synthesized outcome): one rule, always
 * the LAST header field, immediately before the payload, so every other field keeps its
 * offset. Client-to-server frames carry no stamp.
 *   0x01  publish (c->s: [u16 topic][payload])        delivery (s->c: [u16 topic][u32 publisher][u64 written][payload])
 *   0x02  var op  (c->s: [u16 ent][u8 mode][payload]) var update (s->c: [u16 ent][u8 flags][u64 written][payload]; flags bit0=forced bit1=write-event)
 *   0x03  task progress (c->s: [u32 req][payload])    progress (s->c: [u32 call][u32 provider][u64 written][payload]; empty payload = the RUNNING ack)
 *   0x04  call (c->s: [u16 ent][u32 call][payload])   call response (s->c: [u32 call][u8 status][u32 provider][u64 written][payload])
 *   0x05  request reply (c->s: [u32 req][u8 status][payload])   request (s->c: [u16 ent][u32 req][u64 written][payload]) */
static const uint8_t kOpData     = 0x01;
static const uint8_t kOpVar      = 0x02;
static const uint8_t kOpProgress = 0x03;
static const uint8_t kOpCall     = 0x04;
static const uint8_t kOpRequest  = 0x05;

static const int kTickMs = 30;   /* match-state poll cadence */

static size_t g_max_buffered = 8u << 20; /* drop a client that buffers past this */
static int    g_verbose      = 0;

/* One created pattern entity. Handles are thin and non-owning (the entity lives in the
 * node until close); Ent pointers are stable (unique_ptr, append-only until close). */
struct Ent {
    enum Kind { FnDef, FnRemote, VarDef, VarRemote, TaskDef, TaskRemote } kind = FnDef;
    uint16_t                   id = 0;
    dart::FunctionDefinition<> fndef;
    dart::RemoteFunction<>     fnrem;
    dart::VariableDefinition<> vardef;
    dart::RemoteVariable<>     varrem;
    dart::TaskDefinition<>     taskdef;
    dart::RemoteTask<>         taskrem;
    /* ticker state, guarded by Conn::mu */
    int                  last_match = -1;              /* match-state change detector */
};

/* One task request parked at the client (the bridge defers every task request; the
 * PendingTask streams progress and answers when the client does). Shared: the WS thread
 * completes it, the on_cancel scan (service thread) polls pt.cancelled(). The token verbs
 * are thread-safe in the C and a stale token reads false/State, so the unsynchronized
 * handle members are as safe as the file's other cross-thread pragmatism. */
struct ParkedTask {
    uint16_t            ent = 0;                /* the owning task definition's id */
    dart::PendingTask<> pt;
    std::atomic<bool>   cancel_pushed{ false }; /* one {op:"cancel"} push per request */
};

/* One WebSocket connection = one node. node/ws/name are written only by the
 * connection's own thread; the node's service thread and the ticker read them, fenced
 * by conn_close (ticker joined, then ~Node joins the service thread). */
struct Conn {
    std::optional<dart::Node> node;
    ix::WebSocket            *ws = nullptr;
    std::string               name;   /* node name (bridge-generated when the client omits it) */

    std::mutex mu;                    /* leaf lock: never call into dart while holding it */
    std::vector<std::unique_ptr<Ent>>              ents;
    std::unordered_map<uint32_t, dart::Deferred<>> parked;   /* req id -> parked function reply */
    std::unordered_map<uint32_t, std::shared_ptr<ParkedTask>> parked_tasks; /* req id -> parked task */
    /* client call id -> (remote-task ent, C call id): the handle for the cancel op.
     * Inserted before call_async (so the terminal response, which erases, can never race
     * an insert), erased at the one terminal response. */
    std::unordered_map<uint32_t, std::pair<uint16_t, uint32_t>> task_calls;
    uint32_t                                       next_req = 0;
    std::vector<uint16_t>                          topic_ids;
    std::unordered_map<uint16_t, int>              topic_match;  /* topic id -> packed matches/ready */
    uint8_t                                        log_sub = 0;  /* bit (1<<level) set once subscribed */

    std::thread       ticker;
    std::atomic<bool> stop{ false };
};

static std::mutex g_conns_lock;
static std::unordered_map<std::string, std::shared_ptr<Conn>> g_conns;

/* ---- small formatting helpers ---------------------------------------------------- */

static std::string hex64(uint64_t v){
    char buf[17];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    return buf;
}

static void w16(uint8_t *p, uint16_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v){
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void w64(uint8_t *p, uint64_t v){ for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t r16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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
    case dart::FieldType::Enum:    return "enum";
    case dart::FieldType::Named:   return "named";
    default: return "?";
    }
}

static const char *send_status_str(dart::SendStatus rc){
    switch (rc){
    case dart::SendStatus::NoTopic:     return "no such topic";
    case dart::SendStatus::TooBig:      return "message too big";
    case dart::SendStatus::BadRole:     return "role cannot do that";
    case dart::SendStatus::OutOfMemory: return "out of memory";
    case dart::SendStatus::State:       return "wrong state";
    case dart::SendStatus::NoSys:       return "not supported";
    default:                            return "send failed";
    }
}

/* One field of a schema, whatever we reflected it from: the compiled handle
 * (dart::Schema::Field) or the owned peer snapshot (dart::SchemaField). Both build their
 * rows through field_row_json, so the two tables can never drift apart again. They did:
 * peer reflection used to omit `named`, so a client rebuilding a peer's type got an
 * anonymous struct where the owner said Color, and every send was schema-refused. */
struct FieldSrc {
    std::string name, type_name, elem_name;
    dart::FieldType kind = dart::FieldType::U8, elem = dart::FieldType::U8;
    uint16_t count = 0, depth = 0, str_cap = 0, arr_parent = 0xFFFFu;
    uint32_t offset = 0, size = 0, elem_size = 0;
    json variants = json::array();      /* enum only: the option table, already built */
};

/* one row of the protocol's field table: a dotted path, the type, and its absolute offset,
 * so a client encodes, decodes and rebuilds the type with no codegen. `parents` carries the
 * enclosing struct names and is advanced here. Fixed fields carry offset/size; the variable
 * kinds (vstring/varr/map) report 0 and live in the message tail after the fixed section
 * (whose length is the schema's `size`). */
static json field_row_json(const FieldSrc &f, std::vector<std::string> &parents){
    parents.resize(f.depth);            /* leaving a struct shrinks the path */
    std::string path;
    for (const std::string &p : parents){ path += p; path += '.'; }
    path += f.name;
    json row = { {"path", path}, {"kind", kind_str(f.kind)},
                 {"offset", f.offset}, {"size", f.size} };
    /* a NAMED type is unwrapped: `kind` is what it wraps, `named` is the name it
       carries (a Pose reads as a struct of Double3 + Quaternion, and says so) */
    if (!f.type_name.empty()) row["named"] = f.type_name;
    if (f.kind == dart::FieldType::Array){ row["elem"] = kind_str(f.elem); row["count"] = f.count; }
    if (f.kind == dart::FieldType::VArray) row["elem"] = kind_str(f.elem);  /* live count, no bound */
    if (f.kind == dart::FieldType::Array || f.kind == dart::FieldType::VArray){
        if (!f.elem_name.empty()) row["elem_named"] = f.elem_name;
        if (f.elem_size) row["elem_size"] = f.elem_size;
        if (f.elem == dart::FieldType::Struct) row["elem_struct"] = true;   /* element-0 template follows */
    }
    if (f.arr_parent != 0xFFFFu) row["in_array"] = f.arr_parent;            /* index it to read */
    if (f.kind == dart::FieldType::Enum){                                   /* backing + option table */
        row["backing"] = kind_str(f.elem);
        row["variants"] = f.variants;
    }
    if (f.str_cap) row["cap"] = f.str_cap;                                  /* string + string arrays */
    /* a struct field, and a struct ARRAY (whose element-0 template follows it), both
       open a path level */
    if (f.kind == dart::FieldType::Struct ||
        ((f.kind == dart::FieldType::Array || f.kind == dart::FieldType::VArray)
         && f.elem == dart::FieldType::Struct))
        parents.push_back(f.name);
    return row;
}

/* the compiled schema as the protocol's field table: every field at every depth */
static json fields_json(const dart::Schema &s){
    json fields = json::array();
    std::vector<std::string> parents;
    uint16_t n = s.field_count();
    for (uint16_t i = 0; i < n; i++){
        dart::Schema::Field f;
        FieldSrc r;
        if (!s.field_at(i, f)) break;
        r.name.assign(f.name.data(), f.name.size());
        r.type_name.assign(f.type_name.data(), f.type_name.size());
        r.elem_name.assign(f.elem_name.data(), f.elem_name.size());
        r.kind = f.kind; r.elem = f.elem;
        r.count = f.count; r.depth = f.depth; r.str_cap = f.str_cap; r.arr_parent = f.arr_parent;
        r.offset = f.offset; r.size = f.size; r.elem_size = f.elem_size;
        if (f.kind == dart::FieldType::Enum){
            dart::Schema::EnumVariant ev;
            for (uint16_t k = 0; k < s.enum_count(i); k++)
                if (s.enum_variant(i, k, ev))
                    r.variants.push_back({ {"name", std::string(ev.name.data(), ev.name.size())},
                                           {"value", ev.value} });
        }
        fields.push_back(field_row_json(r, parents));
    }
    return fields;
}

/* one schema's layout block ({size, hash, fields}), as `topic` replies it */
static json schema_json(const dart::Schema &s){
    return { {"size", s.size()}, {"hash", hex64(s.hash())}, {"fields", fields_json(s)} };
}

/* one decoded @dart/meta MapItem -> JSON (the whole self-describing snapshot body). u64
 * rides as a JSON number: the meta counters stay within JS safe range in practice, like
 * @dart/log's wall_us already does. */
static json mapitem_to_json(const dart::MapItem &m){
    if (m.is_bool())   return m.as_bool();
    if (m.is_uint())   return m.as_uint();
    if (m.is_int())    return m.as_int();
    if (m.is_double()) return m.as_f64();
    if (m.is_string()) return m.as_string();
    if (m.is_array()){
        json a = json::array();
        for (const dart::MapItem &e : m.as_array()) a.push_back(mapitem_to_json(e));
        return a;
    }
    if (m.is_map()){
        json o = json::object();
        for (const auto &kv : m.as_map()) o[kv.first] = mapitem_to_json(kv.second);
        return o;
    }
    return nullptr;   /* monostate */
}

static json mapdict_to_json(const dart::MapDict &d){
    json o = json::object();
    for (const auto &kv : d) o[kv.first] = mapitem_to_json(kv.second);
    return o;
}

static const char *entity_kind_str(dart::EntityKind k){
    switch (k){
    case dart::EntityKind::Topic:    return "topic";
    case dart::EntityKind::Function: return "function";
    case dart::EntityKind::Variable: return "variable";
    case dart::EntityKind::Task:     return "task";
    default:                         return "?";
    }
}

/* a reflected PEER schema's field table ({name, size, hash, fields}): the same rows a topic
 * create reply carries, from the owned SchemaInfo the wrapper hands back. */
static json schemainfo_json(const dart::SchemaInfo &si){
    json fields = json::array();
    std::vector<std::string> parents;
    for (const dart::SchemaField &f : si.fields){
        FieldSrc r;
        r.name = f.name; r.type_name = f.type_name; r.elem_name = f.elem_name;
        r.kind = f.kind; r.elem = f.elem;
        r.count = f.count; r.depth = f.depth; r.str_cap = f.str_cap; r.arr_parent = f.arr_parent;
        r.offset = f.offset; r.size = f.size; r.elem_size = f.elem_size;
        for (const dart::SchemaField::Option &v : f.variants)
            r.variants.push_back({ {"name", v.name}, {"value", v.value} });
        fields.push_back(field_row_json(r, parents));
    }
    /* `name` is the schema's ROOT TYPE name: a client that reflects a peer's schema needs it,
       with the field table and its `named` rows, to reconstruct the exact DSL and create a
       matching typed handle, so writing to a typed variable's set channel is accepted
       instead of schema-refused. */
    return { {"name", si.name}, {"size", si.size}, {"hash", hex64(si.hash)}, {"fields", fields} };
}

/* one reflected entity (from Node::entities / peer_entities) as JSON. `name` is the base
 * name (or a "0x????????" placeholder until details arrive); hash is the low-32 name
 * hash; schema hashes ride as hex (64-bit, past JS safe integers). When the peer's schema
 * is known, `schema` (and `rsp` for functions) carry the full field table so a client can
 * render types and decode live messages of a topic it merely discovered. */
static json entity_json(const dart::Entity &e){
    json j = { {"kind", entity_kind_str(e.kind)}, {"name", e.name},
               {"provides", e.provides}, {"consumes", e.consumes}, {"reliable", e.reliable},
               {"index", e.index}, {"hash", e.hash} };
    if (e.kind == dart::EntityKind::Variable){ j["writable"] = e.writable; j["forceable"] = e.forceable; }
    if (e.kind == dart::EntityKind::Task){
        j["cancellable"] = e.cancellable; j["exclusive"] = e.exclusive; j["multi"] = e.multi;
    }
    if (e.incomplete)      j["incomplete"]     = true;
    if (e.schema_hash)     j["schema_hash"]    = hex64(e.schema_hash);
    if (e.rsp_schema_hash) j["rsp_schema_hash"] = hex64(e.rsp_schema_hash);
    if (e.progress_schema_hash) j["progress_schema_hash"] = hex64(e.progress_schema_hash);
    if (!e.schema.empty())     j["schema"] = schemainfo_json(e.schema);
    if (!e.rsp_schema.empty()) j["rsp"]    = schemainfo_json(e.rsp_schema);
    if (!e.progress_schema.empty()) j["progress_schema"] = schemainfo_json(e.progress_schema);
    return j;
}

static int role_from(const std::string &s, dart::Role *out){
    if (s == "pubsub")   { *out = dart::Role::PubSub;   return 1; }
    if (s == "pub")      { *out = dart::Role::PubOnly;  return 1; }
    if (s == "sub")      { *out = dart::Role::SubOnly;  return 1; }
    if (s == "inactive") { *out = dart::Role::Inactive; return 1; }
    return 0;
}

static int log_level_from(const std::string &s, dart::LogLevel *out){
    if (s == "error") { *out = dart::LogLevel::Error; return 1; }
    if (s == "warn")  { *out = dart::LogLevel::Warn;  return 1; }
    if (s == "info")  { *out = dart::LogLevel::Info;  return 1; }
    return 0;
}
static const char *log_level_str(dart::LogLevel l){
    return l == dart::LogLevel::Error ? "error" : l == dart::LogLevel::Warn ? "warn" : "info";
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

/* a failed fire-and-forget data op; `entity` scopes pattern ops, `topic` publishes */
static void send_error_event(Conn *c, const char *scope, uint16_t id, dart::SendStatus rc){
    send_json(c, { {"op", "event"}, {"event", "send_error"},
                   {scope, id}, {"code", (int)rc}, {"error", send_status_str(rc)} });
}

/* binary push helpers */
static void push_frame(Conn *c, const uint8_t *hdr, size_t hdr_len, dart::Bytes payload){
    if (!c->ws) return;
    std::string frame;
    frame.resize(hdr_len + payload.size());
    memcpy(&frame[0], hdr, hdr_len);
    if (payload.size()) memcpy(&frame[hdr_len], payload.data(), payload.size());
    c->ws->send(frame, true);
}

/* ---- match-state push -------------------------------------------------------------
 * Recompute each created entity's match summary (all read-only dart queries, legal from
 * a callback) and push {op:"match", ...} for the ones that changed. Runs on the node's
 * service thread (peer events) and on the ticker; the cache under mu dedupes. */
static void push_match_states(Conn *c){
    if (!c->node || !c->ws) return;

    std::vector<uint16_t> topics;
    std::vector<Ent *>    ents;
    {
        std::lock_guard<std::mutex> g(c->mu);
        topics = c->topic_ids;
        for (auto &e : c->ents) ents.push_back(e.get());
    }

    for (uint16_t id : topics){
        dart::Topic t = c->node->topic(id);
        if (!t) continue;
        int matches = t.match_count(), ready = t.ready() ? 1 : 0;
        int packed = matches * 2 + ready;
        {
            std::lock_guard<std::mutex> g(c->mu);
            auto it = c->topic_match.find(id);
            if (it != c->topic_match.end() && it->second == packed) continue;
            c->topic_match[id] = packed;
            send_json(c, { {"op", "match"}, {"type", "topic"}, {"id", id},
                           {"matches", matches}, {"ready", ready != 0} });
        }
    }
    for (Ent *e : ents){
        int  m = 0;
        json j = { {"op", "match"}, {"id", e->id} };
        switch (e->kind){
        case Ent::FnDef:
            m = e->fndef.caller_count();
            j["type"] = "function_definition"; j["callers"] = m;
            break;
        case Ent::FnRemote:
            m = e->fnrem.has_definition() ? 1 : 0;
            j["type"] = "remote_function"; j["has_definition"] = m != 0;
            break;
        case Ent::VarDef:
            m = e->vardef.remote_count();
            j["type"] = "variable_definition"; j["remotes"] = m;
            break;
        case Ent::VarRemote:
            m = e->varrem.has_definition() ? 1 : 0;
            j["type"] = "remote_variable"; j["has_definition"] = m != 0;
            break;
        case Ent::TaskDef:
            m = e->taskdef.caller_count();
            j["type"] = "task_definition"; j["callers"] = m;
            break;
        case Ent::TaskRemote:
            m = e->taskrem.has_definition() ? 1 : 0;
            j["type"] = "remote_task"; j["has_definition"] = m != 0;
            break;
        }
        {
            std::lock_guard<std::mutex> g(c->mu);
            if (e->last_match == m) continue;
            e->last_match = m;
            send_json(c, j);
        }
    }
}

/* Format one event as JSON and send it. Runs inside the dart event handler (on the
 * node's service thread, or on the connection thread for events a control op triggered). */
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
        if (ev.peer())                e["peer"]       = ev.peer();
        if (ev.os_error())            e["os_error"]   = ev.os_error();
        break;
    default:
        e["event"] = "unknown";
        break;
    }
    send_json(c, e);
    /* peer/interest churn is when match summaries move; recompute + push changes */
    if (ev.kind() == dart::EventKind::PeerUp || ev.kind() == dart::EventKind::PeerDown ||
        ev.kind() == dart::EventKind::PeerInterest)
        push_match_states(c);
}

/* Delivery: one DART message -> one binary WebSocket frame [op][u16 topic][u32 publisher]
 * [u64 written_us][payload]. Runs on the node's service thread; drops a client that cannot keep up.
 * Pattern channels never reach this handler (the patterns layer routes them). */
static void deliver_message(Conn *c, const dart::MessageView &m){
    if (!c->ws) return;
    if (c->ws->bufferedAmount() > g_max_buffered){
        c->ws->close(1008, "slow consumer");    /* drop rather than buffer forever */
        return;
    }
    uint8_t hdr[15];
    hdr[0] = kOpData;
    w16(hdr + 1, m.topic_index());
    w32(hdr + 3, m.publisher_id());
    w64(hdr + 7, m.written_us());        /* the publisher's source stamp (0 = opted out) */
    push_frame(c, hdr, 15, m.data());
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
    o.max_topics          = (uint16_t)req.value("max_topics", 0);
    o.disable_shm         = req.value("disable_shm", false);
    o.multicast_interface = req.value("interface", std::string());
    o.fragment_size       = (uint16_t)req.value("fragment_size", 0);
    o.announce_interval_us = (uint32_t)req.value("announce_interval_ms", 0) * 1000u;
    o.peer_timeout_us      = (uint32_t)req.value("peer_timeout_ms", 0) * 1000u;
    o.max_peers            = (uint16_t)req.value("max_peers", 0);
    o.match_wait_ms        = req.value("match_wait_ms", 0);
    o.disable_logs         = req.value("disable_logs", false);
    o.disable_meta         = req.value("disable_meta", false);
    o.disable_error_logs   = req.value("disable_error_logs", false);
    o.fetch_details        = req.value("fetch_details", false);   /* resolve reflected names for unshared topics */
    for (const auto &s : req.value("seed_peers", std::vector<std::string>{}))
        o.seed_peers.push_back(s);   /* "ip" or "ip:port"; the wrapper parses + rejects bad ones */
    o.unicast_only         = req.value("unicast_only", false);   /* no multicast: seeds + relaying */
    o.self_ip              = req.value("self_ip", std::string()); /* state our locator outright */
    o.advertise_port       = (uint16_t)req.value("advertise_port", 0);
    if (o.max_topics == 0) o.max_topics = 8;

    /* handlers capture the stable Conn*; the node's service thread owns them and the
     * Conn always outlives its node (conn_close resets the node first). */
    auto on_msg = [c](const dart::MessageView &m){ deliver_message(c, m); };
    auto on_evt = [c](const dart::Event    &e){ send_event(c, e); };

    try {
        c->node.emplace(name, on_msg, on_evt, o);
    } catch (const dart::Error &e){
        reply_err(c, seq, std::string("dart_node_open failed: ") + e.what());
        return;
    }
    c->name = name;
    c->node->start();   /* the node's own service thread drives everything */

    /* the ticker: match-state recompute (see PROTOCOL.md). Variable values are pushed
       event-driven by the on_change hook, not polled here. */
    c->stop = false;
    std::shared_ptr<Conn> keep;
    {
        std::lock_guard<std::mutex> g(g_conns_lock);
        for (auto &kv : g_conns) if (kv.second.get() == c) keep = kv.second;
    }
    c->ticker = std::thread([keep, c]{
        while (!c->stop.load()){
            std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
            if (c->stop.load()) break;
            push_match_states(c);
        }
    });

    reply_ok(c, seq, { {"proto", kProtoVersion}, {"name", name} });
    if (g_verbose) printf("[bridge] node '%s' opened\n", name.c_str());
}

/* compile the DSL text at `key` (optional). Returns 0 + replies on error. */
static int compile_schema(Conn *c, const json &req, const json &seq, const char *key,
                          std::optional<dart::Schema> *out){
    std::string text = req.value(key, "");
    if (text.empty()) return 1;
    std::string err;
    *out = dart::Schema::compile(text, &err);
    if (!*out){ reply_err(c, seq, "schema error: " + err); return 0; }
    return 1;
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
    qos.max_rate_hz          = (uint16_t)req.value("max_rate_hz", 0);   /* subscriber best-effort delivery cap */

    /* Optional typed schema. create_topic copies the schema into node memory, so the
     * local one need not outlive the topic; we keep it just long enough to reply. */
    std::optional<dart::Schema> schema;
    if (!compile_schema(c, req, seq, "schema", &schema)) return;

    dart::Topic ch;
    try {
        ch = c->node->create_topic(name, role, schema ? &*schema : nullptr, qos);
    } catch (const dart::Error &e){
        reply_err(c, seq, std::string("create failed: ") + e.what());
        return;
    }

    {
        std::lock_guard<std::mutex> g(c->mu);
        c->topic_ids.push_back(ch.index());
    }
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

/* Runs on the WS handler thread (blocks this connection's receives, nothing else);
 * the C wait machinery works under the node's service thread. */
static void op_settle(Conn *c, const json &req, const json &seq){
    bool settled = c->node->settle(req.value("timeout_ms", -1));
    reply_ok(c, seq, { {"settled", settled} });
}

/* reserve an Ent slot (id = index). Kept on failure paths simple: creates are
 * append-only; a failed create leaves a dead slot rather than shifting later ids. */
static Ent *ent_new(Conn *c, Ent::Kind kind){
    std::lock_guard<std::mutex> g(c->mu);
    auto e = std::make_unique<Ent>();
    e->kind = kind;
    e->id   = (uint16_t)c->ents.size();
    c->ents.push_back(std::move(e));
    return c->ents.back().get();
}

static Ent *ent_get(Conn *c, uint16_t id, Ent::Kind kind){
    std::lock_guard<std::mutex> g(c->mu);
    if (id >= c->ents.size()) return nullptr;
    Ent *e = c->ents[id].get();
    return e->kind == kind ? e : nullptr;
}

static dart::FunctionOptions fn_opts(const json &req){
    dart::FunctionOptions o;
    o.backpressure_wait_us = (uint32_t)req.value("backpressure_wait_ms", 0) * 1000u;
    o.timeout_us           = (uint32_t)req.value("timeout_ms", 0) * 1000u;
    o.keep_last            = (uint16_t)req.value("keep_last", 0);
    return o;
}

/* `function_definition`: the bridge always uses the full handler form and DEFERS every
 * request: {op:"request"} JSON + a binary payload frame go to the client, and the
 * parked Deferred completes when the client answers (or fails at disconnect). */
static void op_function_definition(Conn *c, const json &req, const json &seq){
    std::string name = req.value("name", "");
    if (name.empty()){ reply_err(c, seq, "missing function name"); return; }
    std::optional<dart::Schema> rq, rs;
    if (!compile_schema(c, req, seq, "req_schema", &rq)) return;
    if (!compile_schema(c, req, seq, "rsp_schema", &rs)) return;

    Ent *e = ent_new(c, Ent::FnDef);
    uint16_t id = e->id;
    auto handler = [c, id](dart::Request<> &r){
        uint32_t req_id;
        {
            std::lock_guard<std::mutex> g(c->mu);
            req_id = ++c->next_req;
        }
        json meta = { {"op", "request"}, {"fn", id}, {"req", req_id},
                      {"caller", r.caller()}, {"caller_name", std::string(r.caller_name())} };
        dart::Bytes data = r.data();
        uint64_t written_us = r.written_us();   /* read before defer(): the request view is callback-lived */
        dart::Deferred<> d = r.defer();
        {
            std::lock_guard<std::mutex> g(c->mu);
            c->parked.emplace(req_id, std::move(d));
        }
        send_json(c, meta);
        uint8_t hdr[15];
        hdr[0] = kOpRequest; w16(hdr + 1, id); w32(hdr + 3, req_id);
        w64(hdr + 7, written_us);
        push_frame(c, hdr, 15, data);
    };
    try {
        e->fndef = dart::FunctionDefinition<>(*c->node, name,
                       rq ? &*rq : nullptr, rs ? &*rs : nullptr, handler, fn_opts(req));
    } catch (const dart::Error &err){
        reply_err(c, seq, std::string("create failed: ") + err.what());
        return;
    }
    json r = { {"id", id} };
    if (rq) r["req"] = schema_json(*rq);
    if (rs) r["rsp"] = schema_json(*rs);
    reply_ok(c, seq, r);
}

static void op_remote_function(Conn *c, const json &req, const json &seq){
    std::string name = req.value("name", "");
    if (name.empty()){ reply_err(c, seq, "missing function name"); return; }
    std::optional<dart::Schema> rq, rs;
    if (!compile_schema(c, req, seq, "req_schema", &rq)) return;
    if (!compile_schema(c, req, seq, "rsp_schema", &rs)) return;

    Ent *e = ent_new(c, Ent::FnRemote);
    try {
        e->fnrem = dart::RemoteFunction<>(*c->node, name,
                       rq ? &*rq : nullptr, rs ? &*rs : nullptr, fn_opts(req));
    } catch (const dart::Error &err){
        reply_err(c, seq, std::string("create failed: ") + err.what());
        return;
    }
    json r = { {"id", e->id} };
    if (rq) r["req"] = schema_json(*rq);
    if (rs) r["rsp"] = schema_json(*rs);
    reply_ok(c, seq, r);
}

static dart::TaskOptions task_opts(const json &req){
    dart::TaskOptions o;
    o.progress_best_effort = req.value("progress_best_effort", false);
    o.progress_keep_last   = (uint16_t)req.value("progress_keep_last", 0);
    o.no_cancel            = req.value("no_cancel", false);
    o.exclusive            = req.value("exclusive", false);
    o.multi                = req.value("multi", false);
    o.timeout_us           = (uint32_t)req.value("timeout_ms", 0) * 1000u;
    o.backpressure_wait_us = (uint32_t)req.value("backpressure_wait_ms", 0) * 1000u;
    o.keep_last            = (uint16_t)req.value("keep_last", 0);
    return o;
}

/* `task_definition`: as function_definition plus the progress schema and the task
 * options. Every request is DEFERRED to the client (the PendingTask implies RUNNING to
 * the caller); the client streams 0x03 progress frames and answers with a 0x05 reply
 * whose status may also be 5 (cancelled). A caller's cancel surfaces as an
 * {op:"cancel"} push naming the request id, driven off the C on_cancel hook. */
static void op_task_definition(Conn *c, const json &req, const json &seq){
    std::string name = req.value("name", "");
    if (name.empty()){ reply_err(c, seq, "missing task name"); return; }
    std::optional<dart::Schema> rq, pg, rs;
    if (!compile_schema(c, req, seq, "req_schema", &rq)) return;
    if (!compile_schema(c, req, seq, "prg_schema", &pg)) return;
    if (!compile_schema(c, req, seq, "rsp_schema", &rs)) return;

    Ent *e = ent_new(c, Ent::TaskDef);
    uint16_t id = e->id;
    auto handler = [c, id](dart::TaskRequest<> &r){
        uint32_t req_id;
        {
            std::lock_guard<std::mutex> g(c->mu);
            req_id = ++c->next_req;
        }
        json meta = { {"op", "request"}, {"task", id}, {"req", req_id},
                      {"caller", r.caller()}, {"caller_name", std::string(r.caller_name())} };
        dart::Bytes data = r.data();
        uint64_t written_us = r.written_us();   /* read before defer(): the request view is callback-lived */
        auto tk = std::make_shared<ParkedTask>();
        tk->ent = id;
        tk->pt  = r.defer();                    /* implies RUNNING to the caller */
        {
            std::lock_guard<std::mutex> g(c->mu);
            c->parked_tasks.emplace(req_id, tk);
        }
        send_json(c, meta);
        uint8_t hdr[15];
        hdr[0] = kOpRequest; w16(hdr + 1, id); w32(hdr + 3, req_id);
        w64(hdr + 7, written_us);
        push_frame(c, hdr, 15, data);
    };
    try {
        e->taskdef = dart::TaskDefinition<>(*c->node, name,
                         rq ? &*rq : nullptr, pg ? &*pg : nullptr, rs ? &*rs : nullptr,
                         handler, task_opts(req));
    } catch (const dart::Error &err){
        reply_err(c, seq, std::string("create failed: ") + err.what());
        return;
    }
    /* the token cannot name its request through the public wrapper, so scan this
       definition's parked requests for the newly-cancelled one (flag set before the hook
       fires; cancel_pushed dedupes) */
    e->taskdef.on_cancel([c, id](uint64_t){
        std::vector<std::pair<uint32_t, std::shared_ptr<ParkedTask>>> snap;
        {
            std::lock_guard<std::mutex> g(c->mu);
            for (auto &kv : c->parked_tasks) if (kv.second->ent == id) snap.push_back(kv);
        }
        for (auto &pr : snap)
            if (pr.second->pt.cancelled() && !pr.second->cancel_pushed.exchange(true))
                send_json(c, { {"op", "cancel"}, {"task", id}, {"req", pr.first} });
    });
    json r = { {"id", id} };
    if (rq) r["req"] = schema_json(*rq);
    if (pg) r["prg"] = schema_json(*pg);
    if (rs) r["rsp"] = schema_json(*rs);
    reply_ok(c, seq, r);
}

static void op_remote_task(Conn *c, const json &req, const json &seq){
    std::string name = req.value("name", "");
    if (name.empty()){ reply_err(c, seq, "missing task name"); return; }
    std::optional<dart::Schema> rq, pg, rs;
    if (!compile_schema(c, req, seq, "req_schema", &rq)) return;
    if (!compile_schema(c, req, seq, "prg_schema", &pg)) return;
    if (!compile_schema(c, req, seq, "rsp_schema", &rs)) return;

    Ent *e = ent_new(c, Ent::TaskRemote);
    try {
        e->taskrem = dart::RemoteTask<>(*c->node, name,
                         rq ? &*rq : nullptr, pg ? &*pg : nullptr, rs ? &*rs : nullptr,
                         task_opts(req));
    } catch (const dart::Error &err){
        reply_err(c, seq, std::string("create failed: ") + err.what());
        return;
    }
    json r = { {"id", e->id} };
    if (rq) r["req"] = schema_json(*rq);
    if (pg) r["prg"] = schema_json(*pg);
    if (rs) r["rsp"] = schema_json(*rs);
    reply_ok(c, seq, r);
}

/* `cancel`: request cancellation of an outstanding task call by its client call id. The
 * reply always carries the C verdict as `status`: cooperative cancel is never a fault. */
static void op_cancel(Conn *c, const json &req, const json &seq){
    uint32_t call = (uint32_t)req.value("call", 0);
    uint16_t ent = 0; uint32_t cid = 0; bool found = false;
    {
        std::lock_guard<std::mutex> g(c->mu);
        auto it = c->task_calls.find(call);
        if (it != c->task_calls.end()){ ent = it->second.first; cid = it->second.second; found = true; }
    }
    if (!found){ reply_ok(c, seq, { {"status", "not_pending"} }); return; }   /* already answered */
    Ent *e = ent_get(c, ent, Ent::TaskRemote);
    if (!e){ reply_err(c, seq, "no such task"); return; }
    dart::SendStatus rc = e->taskrem.cancel(cid);
    const char *s = rc == dart::SendStatus::Ok      ? "ok"
                  : rc == dart::SendStatus::BadRole ? "no_cancel"     /* provider declared no_cancel */
                  : rc == dart::SendStatus::State   ? "not_pending"
                  :                                   "error";
    reply_ok(c, seq, { {"status", s} });
}

static void op_variable(Conn *c, const json &req, const json &seq, bool definition){
    std::string name = req.value("name", "");
    if (name.empty()){ reply_err(c, seq, "missing variable name"); return; }
    std::optional<dart::Schema> sc;
    if (!compile_schema(c, req, seq, "schema", &sc)) return;

    dart::VariableOptions<> o;
    o.read_only            = req.value("read_only", false);
    o.allow_force          = req.value("allow_force", false);
    o.catch_up             = (uint16_t)req.value("catch_up", 0);
    o.keep_last            = (uint16_t)req.value("keep_last", 0);
    o.backpressure_wait_us = (uint32_t)req.value("backpressure_wait_ms", 0) * 1000u;
    std::vector<uint8_t> initial = req.value("initial", std::vector<uint8_t>{});   /* raw bytes */
    if (!initial.empty()) o.initial = dart::Bytes(initial.data(), initial.size());

    Ent *e = ent_new(c, definition ? Ent::VarDef : Ent::VarRemote);
    try {
        if (definition) e->vardef = dart::VariableDefinition<>(*c->node, name, sc ? &*sc : nullptr, o);
        else            e->varrem = dart::RemoteVariable<>(*c->node, name, sc ? &*sc : nullptr, o);
    } catch (const dart::Error &err){
        reply_err(c, seq, std::string("create failed: ") + err.what());
        return;
    }
    json r = { {"id", e->id} };
    if (sc){ r["size"] = sc->size(); r["hash"] = hex64(sc->hash()); r["fields"] = fields_json(*sc); }
    reply_ok(c, seq, r);

    /* value updates are pushed event-driven: on_change fires only on an actual state
       change (bytes or the forced flag), so a byte-identical re-set pushes nothing.
       on_write (opt-in) fires on EVERY applied write, tagged in the flag byte so the
       client can route it. Registered AFTER the create reply: the registration replays
       the current value (a definition's initial) and the client must already know the id. */
    uint16_t id = e->id;
    bool wants_write = req.value("on_write", false);
    auto push_var = [c, id](const dart::VariableUpdate &u, uint8_t evbit){
        uint8_t hdr[12];
        hdr[0] = kOpVar; w16(hdr + 1, id);
        hdr[3] = (uint8_t)((u.forced() ? 1 : 0) | evbit);   /* bit0 = forced, bit1 = write-event */
        w64(hdr + 4, u.written_us());     /* the writer's source stamp (0 = a local/unstamped write) */
        push_frame(c, hdr, 12, u.value());
    };
    if (definition){
        e->vardef.on_change([push_var](const dart::VariableUpdate &u){ push_var(u, 0); });
        if (wants_write) e->vardef.on_write([push_var](const dart::VariableUpdate &u){ push_var(u, 2); });
    } else {
        e->varrem.on_change([push_var](const dart::VariableUpdate &u){ push_var(u, 0); });
        if (wants_write) e->varrem.on_write([push_var](const dart::VariableUpdate &u){ push_var(u, 2); });
    }
}

/* ---- built-in logs (the @dart/log topics) ---------------------------------------- */

/* publish a line on a level's log topic */
static void op_log(Conn *c, const json &req, const json &seq){
    dart::LogLevel level;
    if (!log_level_from(req.value("level", ""), &level)){ reply_err(c, seq, "bad log level"); return; }
    std::string text = req.value("text", "");
    dart::SendStatus rc = c->node->log(level, text);
    if (rc != dart::SendStatus::Ok){ reply_err(c, seq, std::string("log failed: ") + send_status_str(rc)); return; }
    reply_ok(c, seq, {});
}

/* subscribe to one or more levels' mesh-wide log stream. Each requested level's lines
 * are pushed as {op:"log", ...} text frames (low rate, so JSON not the binary plane).
 * Idempotent per level (c->log_sub guards against a duplicate handler). */
static void op_log_subscribe(Conn *c, const json &req, const json &seq){
    std::vector<std::string> levels = req.value("levels",
        std::vector<std::string>{ "error", "warn", "info" });
    for (const std::string &ls : levels){
        dart::LogLevel level;
        if (!log_level_from(ls, &level)){ reply_err(c, seq, "bad log level: " + ls); return; }
        uint8_t bit = (uint8_t)(1u << (int)level);
        if (c->log_sub & bit) continue;                 /* already subscribed to this level */
        bool ok = c->node->on_log(level, [c, level](const dart::LogLine &l){
            send_json(c, { {"op", "log"}, {"level", log_level_str(level)},
                           {"node", std::string(l.node.data(), l.node.size())},
                           {"wall_us", l.wall_us}, {"mono_us", l.mono_us},
                           {"recv_us", l.recv_us}, {"written_us", l.written_us},
                           {"text", std::string(l.text.data(), l.text.size())} });
        });
        if (!ok){ reply_err(c, seq, "logs are disabled on this node"); return; }
        c->log_sub |= bit;
    }
    reply_ok(c, seq, {});
}

/* ---- introspection (query-based, pull-only) -------------------------------------- */

/* Local reflection: synchronous reads of what THIS node already knows from discovery.
 * peers()/entities()/peer_entities() take the node lock internally; we hold no Conn::mu,
 * so calling into dart here is safe (see the threading note at the top). */
static void op_peers(Conn *c, const json &seq){
    json arr = json::array();
    for (const dart::Peer &p : c->node->peers())
        arr.push_back({ {"id", p.id}, {"name", p.name}, {"address", p.address},
                        {"active", p.active}, {"fragment_size", p.fragment_size} });
    reply_ok(c, seq, { {"peers", arr} });
}

static void op_entities(Conn *c, const json &seq){
    json arr = json::array();
    for (const dart::Entity &e : c->node->entities()) arr.push_back(entity_json(e));
    reply_ok(c, seq, { {"entities", arr} });
}

static void op_peer_entities(Conn *c, const json &req, const json &seq){
    uint32_t peer = (uint32_t)req.value("peer", 0);
    bool include_dropped = req.value("include_dropped", false);   /* default: live peers only */
    json arr = json::array();
    for (const dart::Entity &e : c->node->peer_entities(peer, include_dropped))
        arr.push_back(entity_json(e));
    reply_ok(c, seq, { {"peer", peer}, {"entities", arr} });
}

/* @dart/meta query: an async directed call to a peer's meta endpoint. The reply fires
 * later from the poll thread, still echoing the request's seq; it never faults, the
 * client inspects `status`. `sections` is an OR of DART_META_* (0 = every section). */
static void op_meta(Conn *c, const json &req, const json &seq){
    uint32_t peer     = (uint32_t)req.value("peer", 0);
    uint32_t sections = (uint32_t)req.value("sections", 0);
    dart::SendStatus rc = c->node->meta_request(peer,
        [c, seq](const dart::MetaSnapshot &s){
            json r = { {"valid", s.valid}, {"status", (int)s.status}, {"provider", s.provider} };
            if (s.valid) r["info"] = mapdict_to_json(s.info);
            reply_ok(c, seq, r);
        }, sections);
    if (rc != dart::SendStatus::Ok)
        reply_err(c, seq, std::string("meta request failed: ") + send_status_str(rc));
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
    if      (op == "topic")               op_topic(c, req, seq);
    else if (op == "role")                op_role(c, req, seq);
    else if (op == "drain")               op_drain(c, req, seq);
    else if (op == "settle")              op_settle(c, req, seq);
    else if (op == "function_definition") op_function_definition(c, req, seq);
    else if (op == "remote_function")     op_remote_function(c, req, seq);
    else if (op == "task_definition")     op_task_definition(c, req, seq);
    else if (op == "remote_task")         op_remote_task(c, req, seq);
    else if (op == "cancel")              op_cancel(c, req, seq);
    else if (op == "variable_definition") op_variable(c, req, seq, true);
    else if (op == "remote_variable")     op_variable(c, req, seq, false);
    else if (op == "log")                 op_log(c, req, seq);
    else if (op == "log_subscribe")       op_log_subscribe(c, req, seq);
    else if (op == "meta")                op_meta(c, req, seq);
    else if (op == "peers")               op_peers(c, seq);
    else if (op == "entities")            op_entities(c, seq);
    else if (op == "peer_entities")       op_peer_entities(c, req, seq);
    else reply_err(c, seq, "unknown op: " + op);
}

/* ---- data plane ------------------------------------------------------------------- */

static void on_publish(Conn *c, const uint8_t *p, size_t n){
    uint16_t id = r16(p + 1);
    /* topic() yields an invalid handle for a bad id; send() then returns NoTopic */
    dart::SendStatus rc = c->node->topic(id).send(dart::Bytes(p + 3, n - 3));
    if (rc != dart::SendStatus::Ok) send_error_event(c, "topic", id, rc);
}

static void on_var_op(Conn *c, const uint8_t *p, size_t n){
    if (n < 4) return;
    uint16_t id   = r16(p + 1);
    uint8_t  mode = p[3];
    Ent *e = ent_get(c, id, Ent::VarDef);
    if (!e) e = ent_get(c, id, Ent::VarRemote);
    if (!e){ send_error_event(c, "entity", id, dart::SendStatus::NoTopic); return; }
    dart::VariableDefinition<> &v = (e->kind == Ent::VarDef)
        ? e->vardef : static_cast<dart::VariableDefinition<> &>(e->varrem);
    dart::Bytes val(p + 4, n - 4);
    dart::SendStatus rc = mode == 0 ? v.set(val)
                        : mode == 1 ? v.force(val)
                        : mode == 2 ? v.unforce()
                        : dart::SendStatus::NoSys;
    if (rc != dart::SendStatus::Ok) send_error_event(c, "entity", id, rc);
}

/* the one call-outcome frame both patterns answer with */
static void push_response(Conn *c, uint32_t call, const dart::ResponseView<> &rv){
    std::string_view m = rv.message();
    size_t ml = m.size() > 255 ? 255 : m.size();   /* one length byte, like the wire */
    uint8_t hdr[19 + 255];
    hdr[0] = kOpCall; w32(hdr + 1, call); hdr[5] = (uint8_t)rv.status();
    w32(hdr + 6, rv.provider());
    w64(hdr + 10, rv.written_us());
    hdr[18] = (uint8_t)ml;
    if (ml) memcpy(hdr + 19, m.data(), ml);
    push_frame(c, hdr, 19 + ml, rv.data());
}

/* a call the bridge refused synchronously: answer Cancelled so the promise settles */
static void push_refused(Conn *c, uint32_t call){
    uint8_t hdr[19];
    hdr[0] = kOpCall; w32(hdr + 1, call); hdr[5] = (uint8_t)dart::CallStatus::Cancelled;
    w32(hdr + 6, 0);
    w64(hdr + 10, 0);            /* synthesized outcome: no source stamp */
    hdr[18] = 0;                 /* no message: the client fills default status text */
    push_frame(c, hdr, 19, dart::Bytes());
}

static void on_call(Conn *c, const uint8_t *p, size_t n){
    if (n < 7) return;
    uint16_t id   = r16(p + 1);
    uint32_t call = r32(p + 3);
    Ent *e = ent_get(c, id, Ent::FnRemote);
    if (e){
        auto cb = [c, call](const dart::ResponseView<> &rv){ push_response(c, call, rv); };
        dart::SendStatus rc = e->fnrem.call_async(dart::Bytes(p + 7, n - 7), cb);
        if (rc != dart::SendStatus::Ok){
            /* the call never launched: carry the reason in a send_error event */
            send_error_event(c, "entity", id, rc);
            push_refused(c, call);
        }
        return;
    }
    e = ent_get(c, id, Ent::TaskRemote);
    if (!e){ send_error_event(c, "entity", id, dart::SendStatus::NoTopic); return; }
    /* a task call: progress frames (empty payload = the RUNNING ack) before the one
     * terminal response, which also releases the cancel mapping */
    auto on_prog = [c, call](const dart::ProgressView<> &pv){
        if (c->ws && c->ws->bufferedAmount() > g_max_buffered){
            c->ws->close(1008, "slow consumer");   /* progress can be a data stream */
            return;
        }
        uint8_t hdr[17];
        hdr[0] = kOpProgress; w32(hdr + 1, call); w32(hdr + 5, pv.provider());
        w64(hdr + 9, pv.written_us());
        push_frame(c, hdr, 17, pv.data());
    };
    auto on_rsp = [c, call](const dart::ResponseView<> &rv){
        {
            std::lock_guard<std::mutex> g(c->mu);
            c->task_calls.erase(call);
        }
        push_response(c, call, rv);
    };
    /* map the client call id before launch, so the terminal response can never race the
     * insert; call_async fills the C call id through the pair in place */
    {
        std::lock_guard<std::mutex> g(c->mu);
        c->task_calls.emplace(call, std::make_pair(id, 0u));
    }
    dart::TaskCall tc = e->taskrem.call_async(dart::Bytes(p + 7, n - 7), on_prog, on_rsp);
    if (!tc.ok()){
        {
            std::lock_guard<std::mutex> g(c->mu);
            c->task_calls.erase(call);
        }
        send_error_event(c, "entity", id, tc.status);
        push_refused(c, call);
        return;
    }
    {
        std::lock_guard<std::mutex> g(c->mu);
        auto it = c->task_calls.find(call);
        if (it != c->task_calls.end()) it->second.second = tc.id;   /* absent = already answered */
    }
}

/* 0x03 client->server: one progress update on a parked task request */
static void on_progress(Conn *c, const uint8_t *p, size_t n){
    if (n < 5) return;
    uint32_t req = r32(p + 1);
    std::shared_ptr<ParkedTask> tk;
    {
        std::lock_guard<std::mutex> g(c->mu);
        auto it = c->parked_tasks.find(req);
        if (it != c->parked_tasks.end()) tk = it->second;
    }
    if (!tk) return;   /* completed/unknown: drop, like an unknown request reply */
    dart::SendStatus rc = tk->pt.progress(dart::Bytes(p + 5, n - 5));
    if (rc != dart::SendStatus::Ok) send_error_event(c, "entity", tk->ent, rc);
}

static void on_request_reply(Conn *c, const uint8_t *p, size_t n){
    if (n < 7) return;
    uint32_t req    = r32(p + 1);
    uint8_t  status = p[5];
    size_t   ml     = p[6];
    if (n < 7 + ml) return;   /* malformed message length */
    std::string_view msg((const char *)p + 7, ml);
    dart::Bytes rsp(p + 7 + ml, n - 7 - ml);
    dart::Deferred<> d;
    {
        std::lock_guard<std::mutex> g(c->mu);
        auto it = c->parked.find(req);
        if (it != c->parked.end()){
            d = std::move(it->second);
            c->parked.erase(it);
        }
    }
    if (d.valid()){
        if (status == 0) d.complete(rsp, msg);
        else             d.fail(msg, rsp);
        return;
    }
    /* a parked TASK: status 0 = ok, 5 = cancelled (the handler honored a cancel),
     * anything else = app_error */
    std::shared_ptr<ParkedTask> tk;
    {
        std::lock_guard<std::mutex> g(c->mu);
        auto it = c->parked_tasks.find(req);
        if (it == c->parked_tasks.end()) return;   /* unknown/duplicate: drop */
        tk = it->second;
        c->parked_tasks.erase(it);
    }
    if      (status == 0) tk->pt.complete(rsp, msg);
    else if (status == 5) tk->pt.complete_cancelled(msg);
    else                  tk->pt.fail(msg, rsp);
}

static void on_binary(Conn *c, const std::string &frame){
    if (frame.size() < 3) return;
    const uint8_t *p = (const uint8_t *)frame.data();
    if (!c->node){
        if (p[0] == kOpData || p[0] == kOpVar)
            send_error_event(c, "topic", r16(p + 1), dart::SendStatus::NoTopic);
        return;
    }
    switch (p[0]){
    case kOpData:     on_publish(c, p, frame.size());       break;
    case kOpVar:      on_var_op(c, p, frame.size());        break;
    case kOpProgress: on_progress(c, p, frame.size());      break;
    case kOpCall:     on_call(c, p, frame.size());          break;
    case kOpRequest:  on_request_reply(c, p, frame.size()); break;
    default: break;   /* reserved ops: drop */
    }
    /* no explicit flush: a send kicks the node's service thread awake */
}

/* ---- connection lifecycle --------------------------------------------------------- */

static void conn_close(const std::shared_ptr<Conn> &c){
    c->stop = true;
    if (c->ticker.joinable()) c->ticker.join();
    /* answer parked requests while the node is still alive, so remote callers get an
     * answer now instead of waiting out their timeout: functions fail, tasks cancel */
    std::unordered_map<uint32_t, dart::Deferred<>>            parked;
    std::unordered_map<uint32_t, std::shared_ptr<ParkedTask>> parked_tasks;
    {
        std::lock_guard<std::mutex> g(c->mu);
        parked       = std::move(c->parked);
        parked_tasks = std::move(c->parked_tasks);
        c->parked.clear();
        c->parked_tasks.clear();
    }
    for (auto &kv : parked)       kv.second.fail("bridge client disconnected");
    for (auto &kv : parked_tasks) kv.second->pt.complete_cancelled("bridge client disconnected");
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
