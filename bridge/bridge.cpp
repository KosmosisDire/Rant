/* The bridge: one WebSocket connection is one full node on the mesh, with a WebRTC data
 * path negotiated over that socket. spec/bridge.md has the design, PROTOCOL.md the wire. */
#include "ramble.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>
#if RAMBLE_BRIDGE_WEBRTC
#include <rtc/rtc.hpp>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>   /* after the socket headers (winsock2 first): the crash filter only */
#endif

#include <atomic>
#include <chrono>
#include <exception>
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

static const int kProtoVersion = 11;

/* The data plane frame, one header for every op in both directions (PROTOCOL.md).
 * written_us is the writer's wall clock, 0 = opted out or synthesized. A client sends 0. */
enum : uint8_t { OP_DATA = 1, OP_VAR = 2, OP_CALL = 3, OP_RESULT = 4, OP_PROGRESS = 5, OP_CANCEL = 6 };
static const size_t kHdr = 29;

static const int    kTickMs      = 30;         /* match-state poll cadence */
static const size_t kLossyBuffer = 1u << 20;   /* best-effort frames drop past this much unsent */

static size_t g_max_buffered = 8u << 20;   /* a reliable carrier this far behind drops the client */
static int    g_verbose      = 0;

#if RAMBLE_BRIDGE_WEBRTC
static int                      g_rtc_enabled = 1;
static int                      g_rtc_debug   = 0;            /* libdatachannel's verbose log */
static std::vector<std::string> g_ice;   /* stun and turn urls, handed to the client too */
static uint16_t                 g_port_lo = 0, g_port_hi = 0; /* ICE port range (0 = any) */
static size_t                   g_mtu = 1200;   /* datagram bound, WebRTC sets DF */
#endif

struct Frame {
    uint8_t  op = 0, flags = 0;
    uint16_t id = 0;
    uint32_t seq = 0, peer = 0;
    uint64_t written = 0;
    uint64_t capture = 0;   /* when the data was true, 0 = none given */
    std::string_view text;
    ramble::Bytes    payload;
};

#if RAMBLE_BRIDGE_WEBRTC
/* what the client asked for a video line: the entity, the VideoFrame field, the call */
struct TrackReq { uint16_t id = 0; std::string path; uint32_t call = 0; bool keep_data = false; };

/* one video line bound to the VideoFrame at `path` of an entity's stream */
struct TrackBinding {
    std::string mid, path;
    uint32_t    call = 0;               /* task progress: the client call id (0 = every sample) */
    bool        keep_data = false;      /* forward the pixels on the frame path too */
    std::string f_codec, f_key, f_pts, f_data;   /* the VideoFrame's fields, dotted */
    int         data_ordinal = -1;      /* the data field's frame in the message tail */
    uint32_t    fixed = 0;              /* the message's fixed-section size */
    std::shared_ptr<rtc::Track>                  track;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtp;
    int     codec = 0, sep = -1;   /* the packetizer currently installed, stream thread only */
    bool    keyed = false;              /* a keyframe opened the stream since the last (re)start */
    uint8_t pt[5] = { 0, 0, 0, 0, 0 };   /* the browser's payload type per codec, 0 = not offered */
};
#endif

/* One created entity, a topic or a pattern handle. Pointers are stable until close. id is
 * client chosen and doubles as the WebRTC data channel id, so both ends open it. */
struct Entity {
    enum Kind { Topic, FnDef, FnRemote, TaskDef, TaskRemote, VarDef, VarRemote } kind = Topic;
    uint16_t id       = 0;
    std::string name;
    bool     reliable = true;   /* the topic QoS, patterns are reliable */
    ramble::Topic                topic;
    ramble::FunctionDefinition<> fndef;
    ramble::RemoteFunction<>     fnrem;
    ramble::TaskDefinition<>     taskdef;
    ramble::RemoteTask<>         taskrem;
    ramble::VariableDefinition<> vardef;
    ramble::RemoteVariable<>     varrem;
    /* the stream's schema, what a video line binds against. Empty when untyped */
    ramble::Schema value_schema, prg_schema;
    /* match-state change detector (packed count*2 + ready), guarded by Conn::mu */
    int last_match = -1;
    /* best-effort drops not yet reported, guarded by Conn::mu */
    uint32_t dropped = 0;
    std::chrono::steady_clock::time_point drop_report{};
#if RAMBLE_BRIDGE_WEBRTC
    std::shared_ptr<rtc::DataChannel>            dc;      /* guarded by Conn::mu */
    std::vector<std::shared_ptr<TrackBinding>> tracks;   /* guarded by Conn::mu */
#endif
};

/* One task request parked at the client, since the bridge defers every task request.
 * Shared: the client's frames complete it, the on_cancel scan polls pt.cancelled(). */
struct ParkedTask {
    uint16_t              ent = 0;
    ramble::PendingTask<> pt;
    std::atomic<bool>     cancel_pushed{ false };   /* one CANCEL frame per request */
};

/* One WebSocket connection is one node. node, ws and name are written only by the
 * connection's own thread, the other threads read them fenced by conn_close. */
struct Conn {
    std::optional<ramble::Node> node;
    ix::WebSocket            *ws = nullptr;
    std::string               name;
    std::weak_ptr<Conn>       self;   /* for callbacks that may outlive the connection thread */

    std::mutex mu;                    /* leaf lock: never call into ramble while holding it */
    std::unordered_map<uint16_t, std::unique_ptr<Entity>>     ents;
    std::unordered_map<uint16_t, std::vector<Entity *>>       by_index;   /* by topic index */
    std::unordered_map<uint32_t, ramble::Deferred<>>          parked;   /* replies by req id */
    std::unordered_map<uint32_t, std::shared_ptr<ParkedTask>> parked_tasks;   /* by req id */
    /* client call id to (remote task entity, C call id), the handle for the cancel op.
     * Inserted before call_async so the terminal response, which erases, never races it. */
    std::unordered_map<uint32_t, std::pair<uint16_t, uint32_t>> task_calls;
    uint32_t next_req = 0;
    uint8_t  log_sub  = 0;            /* bit (1<<level) set once subscribed */

    std::thread       ticker;
    std::atomic<bool> stop{ false };

#if RAMBLE_BRIDGE_WEBRTC
    std::shared_ptr<rtc::PeerConnection> pc;
    std::atomic<bool> rtc_up{ false };
    json              rtc_seq;          /* the client's offer awaiting our answer */
    bool              rtc_seq_pending = false;
    std::unordered_map<std::string, TrackReq>    track_map;   /* by offered video mid */
    std::unordered_map<std::string, std::string> track_result; /* per mid, for the answer reply */
#endif
};

static std::mutex g_conns_lock;
static std::unordered_map<std::string, std::shared_ptr<Conn>> g_conns;

/* ---- small helpers ---------------------------------------------------------------- */

static std::string hex64(uint64_t v){
    char buf[17];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    return buf;
}

static void w16(uint8_t *p, uint16_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v){ for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void w64(uint8_t *p, uint64_t v){ for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t r16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t r64(const uint8_t *p){ return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32); }

static std::string frame_build(const Frame &f){
    size_t tl = f.text.size() > 255 ? 255 : f.text.size();
    std::string out;
    out.resize(kHdr + tl + f.payload.size());
    uint8_t *p = (uint8_t *)&out[0];
    p[0] = f.op; p[1] = f.flags; w16(p + 2, f.id); w32(p + 4, f.seq); w32(p + 8, f.peer);
    w64(p + 12, f.written); w64(p + 20, f.capture); p[28] = (uint8_t)tl;
    if (tl) memcpy(p + kHdr, f.text.data(), tl);
    if (f.payload.size()) memcpy(p + kHdr + tl, f.payload.data(), f.payload.size());
    return out;
}

static bool frame_parse(const uint8_t *p, size_t n, Frame &f){
    if (n < kHdr) return false;
    size_t tl = p[28];
    if (n < kHdr + tl) return false;
    f.op = p[0]; f.flags = p[1]; f.id = r16(p + 2); f.seq = r32(p + 4); f.peer = r32(p + 8);
    f.written = r64(p + 12); f.capture = r64(p + 20);
    f.text    = std::string_view((const char *)p + kHdr, tl);
    f.payload = ramble::Bytes(p + kHdr + tl, n - kHdr - tl);
    return true;
}

static const char *kind_str(ramble::FieldType kind){
    switch (kind){
    case ramble::FieldType::U8:   return "u8";   case ramble::FieldType::U16: return "u16";
    case ramble::FieldType::U32:  return "u32";  case ramble::FieldType::U64: return "u64";
    case ramble::FieldType::I8:   return "i8";   case ramble::FieldType::I16: return "i16";
    case ramble::FieldType::I32:  return "i32";  case ramble::FieldType::I64: return "i64";
    case ramble::FieldType::F32:  return "f32";  case ramble::FieldType::F64: return "f64";
    case ramble::FieldType::Bool: return "bool";
    case ramble::FieldType::Array:   return "arr";  case ramble::FieldType::Struct: return "struct";
    case ramble::FieldType::String:  return "string";
    case ramble::FieldType::VString: return "vstring"; case ramble::FieldType::VArray: return "varr";
    case ramble::FieldType::Map:     return "map";
    case ramble::FieldType::Enum:    return "enum";
    case ramble::FieldType::Named:   return "named";
    default: return "?";
    }
}

static const char *send_status_str(ramble::SendStatus rc){
    switch (rc){
    case ramble::SendStatus::NoTopic:     return "no such entity";
    case ramble::SendStatus::TooBig:      return "message too big";
    case ramble::SendStatus::BadRole:     return "role cannot do that";
    case ramble::SendStatus::OutOfMemory: return "out of memory";
    case ramble::SendStatus::State:       return "wrong state";
    case ramble::SendStatus::NoSys:       return "not supported";
    default:                            return "send failed";
    }
}

/* One field of a schema. Every schema is a ramble::Schema, so one row builder serves every
 * table (two once drifted and every named reader then refused the sender). */
struct FieldSrc {
    std::string name, type_name, elem_name;
    ramble::FieldType kind = ramble::FieldType::U8, elem = ramble::FieldType::U8;
    uint16_t count = 0, depth = 0, str_cap = 0, arr_parent = 0xFFFFu;
    uint32_t offset = 0, size = 0, elem_size = 0;
    json variants = json::array();      /* enum only: the option table, already built */
};

/* one row of the protocol's field table: a dotted path, the type and its absolute offset.
 * parents carries the enclosing struct names. The variable kinds report offset and size 0. */
static json field_row_json(const FieldSrc &f, std::vector<std::string> &parents){
    parents.resize(f.depth);            /* leaving a struct shrinks the path */
    std::string path;
    for (const std::string &p : parents){ path += p; path += '.'; }
    path += f.name;
    json row = { {"path", path}, {"kind", kind_str(f.kind)},
                 {"offset", f.offset}, {"size", f.size} };
    if (!f.type_name.empty()) row["named"] = f.type_name;   /* named types read as what they wrap */
    if (f.kind == ramble::FieldType::Array){ row["elem"] = kind_str(f.elem); row["count"] = f.count; }
    if (f.kind == ramble::FieldType::VArray) row["elem"] = kind_str(f.elem);
    if (f.kind == ramble::FieldType::Array || f.kind == ramble::FieldType::VArray){
        if (!f.elem_name.empty()) row["elem_named"] = f.elem_name;
        if (f.elem_size) row["elem_size"] = f.elem_size;
        if (f.elem == ramble::FieldType::Struct) row["elem_struct"] = true;
    }
    if (f.arr_parent != 0xFFFFu) row["in_array"] = f.arr_parent;
    if (f.kind == ramble::FieldType::Enum){
        row["backing"] = kind_str(f.elem);
        row["variants"] = f.variants;
    }
    if (f.str_cap) row["cap"] = f.str_cap;
    /* a struct field, and a struct ARRAY (whose element-0 template follows), open a path level */
    if (f.kind == ramble::FieldType::Struct ||
        ((f.kind == ramble::FieldType::Array || f.kind == ramble::FieldType::VArray)
         && f.elem == ramble::FieldType::Struct))
        parents.push_back(f.name);
    return row;
}

static json fields_json(const ramble::Schema &s){
    json fields = json::array();
    std::vector<std::string> parents;
    uint16_t n = s.field_count();
    for (uint16_t i = 0; i < n; i++){
        ramble::Schema::Field f;
        FieldSrc r;
        if (!s.field_at(i, f)) break;
        r.name.assign(f.name.data(), f.name.size());
        r.type_name.assign(f.type_name.data(), f.type_name.size());
        r.elem_name.assign(f.elem_name.data(), f.elem_name.size());
        r.kind = f.kind; r.elem = f.elem;
        r.count = f.count; r.depth = f.depth; r.str_cap = f.str_cap; r.arr_parent = f.arr_parent;
        r.offset = f.offset; r.size = f.size; r.elem_size = f.elem_size;
        if (f.kind == ramble::FieldType::Enum){
            ramble::Schema::EnumVariant ev;
            for (uint16_t k = 0; k < s.enum_count(i); k++)
                if (s.enum_variant(i, k, ev))
                    r.variants.push_back({ {"name", std::string(ev.name.data(), ev.name.size())},
                                           {"value", ev.value} });
        }
        fields.push_back(field_row_json(r, parents));
    }
    return fields;
}

/* one schema's layout block ({name, size, hash, fields}), as every create reply carries it */
static json schema_json(const ramble::Schema &s){
    return { {"name", std::string(s.name())}, {"size", s.size()}, {"hash", hex64(s.hash())},
             {"fields", fields_json(s)} };
}

/* one decoded @ramble/meta MapItem to JSON */
static json mapitem_to_json(const ramble::MapItem &m){
    if (m.is_bool())   return m.as_bool();
    if (m.is_uint())   return m.as_uint();
    if (m.is_int())    return m.as_int();
    if (m.is_double()) return m.as_f64();
    if (m.is_string()) return m.as_string();
    if (m.is_array()){
        json a = json::array();
        for (const ramble::MapItem &e : m.as_array()) a.push_back(mapitem_to_json(e));
        return a;
    }
    if (m.is_map()){
        json o = json::object();
        for (const auto &kv : m.as_map()) o[kv.first] = mapitem_to_json(kv.second);
        return o;
    }
    return nullptr;
}

static json mapdict_to_json(const ramble::MapDict &d){
    json o = json::object();
    for (const auto &kv : d) o[kv.first] = mapitem_to_json(kv.second);
    return o;
}

static const char *entity_kind_str(ramble::EntityKind k){
    switch (k){
    case ramble::EntityKind::Topic:    return "topic";
    case ramble::EntityKind::Function: return "function";
    case ramble::EntityKind::Variable: return "variable";
    case ramble::EntityKind::Task:     return "task";
    default:                         return "?";
    }
}

static json entity_json(const ramble::Entity &e){
    json j = { {"kind", entity_kind_str(e.kind)}, {"name", e.name},
               {"provides", e.provides}, {"consumes", e.consumes}, {"reliable", e.reliable},
               {"hash", e.hash}, {"providers", e.providers}, {"consumers", e.consumers},
               {"generation", hex64(e.generation)} };
    if (e.providers) j["provider"] = e.provider;
    if (!e.from.empty()) j["from"] = e.from;
    if (e.kind == ramble::EntityKind::Variable){ j["writable"] = e.writable; j["forceable"] = e.forceable; }
    if (e.kind == ramble::EntityKind::Task){
        j["cancellable"] = e.cancellable; j["exclusive"] = e.exclusive; j["multi"] = e.multi;
    }
    if (e.incomplete)               j["incomplete"]           = true;
    if (e.conflict)                 j["conflict"]             = true;
    if (!e.schema.empty())          j["schema_hash"]          = hex64(e.schema.hash());
    if (!e.rsp_schema.empty())      j["rsp_schema_hash"]      = hex64(e.rsp_schema.hash());
    if (!e.progress_schema.empty()) j["progress_schema_hash"] = hex64(e.progress_schema.hash());
    if (!e.schema.empty())          j["schema"] = schema_json(e.schema);
    if (!e.rsp_schema.empty())      j["rsp"]    = schema_json(e.rsp_schema);
    if (!e.progress_schema.empty()) j["prg"]    = schema_json(e.progress_schema);
    return j;
}

static int role_from(const std::string &s, ramble::Role *out){
    if (s == "pubsub")   { *out = ramble::Role::PubSub;   return 1; }
    if (s == "pub")      { *out = ramble::Role::PubOnly;  return 1; }
    if (s == "sub")      { *out = ramble::Role::SubOnly;  return 1; }
    if (s == "inactive") { *out = ramble::Role::Inactive; return 1; }
    return 0;
}

static int log_level_from(const std::string &s, ramble::LogLevel *out){
    if (s == "error") { *out = ramble::LogLevel::Error; return 1; }
    if (s == "warn")  { *out = ramble::LogLevel::Warn;  return 1; }
    if (s == "info")  { *out = ramble::LogLevel::Info;  return 1; }
    return 0;
}
static const char *log_level_str(ramble::LogLevel l){
    return l == ramble::LogLevel::Error ? "error" : l == ramble::LogLevel::Warn ? "warn" : "info";
}

static uint64_t now_wall_us(){
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

/* server to client sends */

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

/* a data frame refused synchronously (bad id, bad role, too big, backpressure timeout) */
static void send_error_event(Conn *c, uint16_t id, ramble::SendStatus rc){
    send_json(c, { {"op", "event"}, {"event", "send_error"},
                   {"id", id}, {"code", (int)rc}, {"error", send_status_str(rc)} });
}

static Entity *ent_get(Conn *c, uint16_t id){
    std::lock_guard<std::mutex> g(c->mu);
    auto it = c->ents.find(id);
    return it == c->ents.end() ? nullptr : it->second.get();
}

/* a best-effort frame dropped for a congested carrier: counted, reported as msg_lost at
 * most once a second per entity (never silent, never a stalled node) */
static void note_drop(Conn *c, Entity *e){
    uint32_t n = 0;
    {
        std::lock_guard<std::mutex> g(c->mu);
        e->dropped++;
        auto now = std::chrono::steady_clock::now();
        if (now - e->drop_report < std::chrono::seconds(1)) return;
        e->drop_report = now;
        n = e->dropped; e->dropped = 0;
    }
    send_json(c, { {"op", "event"}, {"event", "msg_lost"}, {"id", e->id}, {"peer", 0},
                   {"count", n}, {"text", "bridge dropped best-effort frames for a slow client"} });
}

/* Send one frame on the entity's best carrier: its open data channel when the frame fits,
 * else the WebSocket. A best effort frame drops behind a backlog, a reliable one closes. */
static void send_frame(Conn *c, Entity *e, const Frame &f){
    std::string bytes = frame_build(f);
    bool reliable = e ? e->reliable : true;
#if RAMBLE_BRIDGE_WEBRTC
    std::shared_ptr<rtc::DataChannel> dc;
    if (e && c->rtc_up){ std::lock_guard<std::mutex> g(c->mu); dc = e->dc; }
    if (dc && dc->isOpen() && bytes.size() <= dc->maxMessageSize()){
        size_t buffered = dc->bufferedAmount();
        if (!reliable && buffered > kLossyBuffer){ note_drop(c, e); return; }
        if (buffered > g_max_buffered){
            if (c->ws) c->ws->close(1008, "slow consumer");
            return;
        }
        try {
            dc->send(reinterpret_cast<const std::byte *>(bytes.data()), bytes.size());
            return;
        } catch (const std::exception &){ /* channel went away under us: the WebSocket takes it */ }
    }
#endif
    if (!c->ws) return;
    size_t buffered = c->ws->bufferedAmount();
    if (!reliable && buffered > kLossyBuffer){ if (e) note_drop(c, e); return; }
    if (buffered > g_max_buffered){ c->ws->close(1008, "slow consumer"); return; }
    c->ws->send(bytes, true);
}

/* ---- WebRTC ----------------------------------------------------------------------- */
#if RAMBLE_BRIDGE_WEBRTC

static void on_frame(Conn *c, const uint8_t *p, size_t n);

static void rtc_event(Conn *c, const char *state, const std::string &text){
    send_json(c, { {"op", "event"}, {"event", "rtc"}, {"state", state}, {"text", text} });
}

/* the entity's data channel, negotiated out of band with id = entity id (no DCEP round
 * trip, and both ends can open it before the create reply lands). Idempotent. */
static void rtc_attach(Conn *c, Entity *e){
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> g(c->mu);
        if (e->dc || !c->pc) return;
        pc = c->pc;
    }
    rtc::DataChannelInit init;
    init.negotiated = true;
    init.id         = e->id;
    if (!e->reliable){ init.reliability.unordered = true; init.reliability.maxRetransmits = 0; }
    std::shared_ptr<rtc::DataChannel> dc;
    try {
        dc = pc->createDataChannel("d" + std::to_string(e->id), init);
    } catch (const std::exception &ex){
        rtc_event(c, "channel_error", std::string("data channel ") + std::to_string(e->id) + ": " + ex.what());
        return;
    }
    std::weak_ptr<Conn> w = c->self;
    dc->onMessage([w](rtc::binary b){
        auto keep = w.lock();
        if (!keep) return;
        try { on_frame(keep.get(), reinterpret_cast<const uint8_t *>(b.data()), b.size()); }
        catch (const std::exception &ex){ printf("[bridge] frame handler: %s\n", ex.what()); }
    }, [](std::string){});
    uint16_t id = e->id;
    dc->onClosed([w, id]{
        auto keep = w.lock();
        if (!keep) return;
        std::lock_guard<std::mutex> g(keep->mu);
        auto it = keep->ents.find(id);
        if (it != keep->ents.end()) it->second->dc.reset();   /* the WebSocket carries it now */
    });
    std::lock_guard<std::mutex> g(c->mu);
    e->dc = dc;
}

/* A track binding: one recvonly video line the client offered for the VideoFrame at path of
 * one entity's stream. Resolved when the offer arrives, used by media_route. */
static bool is_var_kind(ramble::FieldType k){
    return k == ramble::FieldType::VString || k == ramble::FieldType::VArray || k == ramble::FieldType::Map;
}

/* validate path as a VideoFrame inside s and fill the binding's field names, "" = the
 * message itself is one. Returns the reason it is not, or "" */
static std::string bind_video_field(const ramble::Schema &s, const std::string &path, TrackBinding &b){
    if (s.empty()) return "entity is untyped";
    std::string prefix;
    if (path.empty()){
        if (s.name() != "VideoFrame") return "the message is not a VideoFrame";
    } else {
        int idx = s.field_index(path);
        ramble::Schema::Field f;
        if (idx < 0 || !s.field_at((uint16_t)idx, f)) return "no field '" + path + "'";
        if (f.type_name != "VideoFrame") return "'" + path + "' is not a VideoFrame";
        prefix = path + ".";
    }
    b.f_codec = prefix + "codec"; b.f_key = prefix + "keyframe"; b.f_pts = prefix + "pts";
    b.f_data = prefix + "data";
    int di = s.field_index(b.f_data);
    if (s.field_index(b.f_codec) < 0 || s.field_index(b.f_key) < 0 || s.field_index(b.f_pts) < 0 || di < 0)
        return "VideoFrame fields not found";
    /* the data field's frame in the message tail: count the variable fields before it */
    b.data_ordinal = 0;
    for (uint16_t i = 0; i < (uint16_t)di; i++){
        ramble::Schema::Field f;
        if (s.field_at(i, f) && is_var_kind(f.kind)) b.data_ordinal++;
    }
    b.fixed = s.size();
    return "";
}

/* The client offered a recvonly video line: claim it, stamp our SSRC into the answer's m
 * line so the browser binds the stream, and read the browser's payload types. */
static void rtc_on_track(Conn *c, std::shared_ptr<rtc::Track> track){
    std::string mid = track->mid();
    TrackReq req;
    Entity *e = nullptr;
    {
        std::lock_guard<std::mutex> g(c->mu);
        auto it = c->track_map.find(mid);
        if (it != c->track_map.end()){
            req = it->second;
            auto ei = c->ents.find(req.id);
            if (ei != c->ents.end()) e = ei->second.get();
        }
    }
    auto refuse = [&](const std::string &why){
        rtc::Description::Media d = track->description();
        d.markRemoved();
        track->setDescription(d);
        std::lock_guard<std::mutex> g(c->mu);
        c->track_result[mid] = why;
    };
    if (!e){ refuse("no such entity"); return; }
    auto b = std::make_shared<TrackBinding>();
    b->mid = mid; b->path = req.path; b->call = req.call; b->keep_data = req.keep_data;
    std::string why = bind_video_field(e->kind == Entity::TaskRemote ? e->prg_schema : e->value_schema, req.path, *b);
    if (!why.empty()){ refuse(why); return; }

    std::string cname = "ramble-" + mid;
    uint32_t ssrc = 1000u + (uint32_t)e->id * 16u + (uint32_t)(e->tracks.size() & 15);
    rtc::Description::Media d = track->description();
    d.addSSRC(ssrc, cname, "ramble", mid);
    track->setDescription(d);
    /* the payload types are the browser's: one per codec, H264 preferring packetization-mode=1 */
    bool h264_mode1 = false;
    for (int p : d.payloadTypes()){
        const rtc::Description::Media::RtpMap *m = d.rtpMap(p);
        if (!m || p > 127) continue;
        int codec = m->format == "H264" ? 2 : m->format == "H265" ? 3 : m->format == "AV1" ? 4 : 0;
        if (!codec) continue;
        bool mode1 = false;
        for (const std::string &f : m->fmtps) if (f.find("packetization-mode=1") != std::string::npos) mode1 = true;
        if (codec == 2 && h264_mode1) continue;
        if (!b->pt[codec] || (codec == 2 && mode1)){ b->pt[codec] = (uint8_t)p; if (codec == 2) h264_mode1 = mode1; }
    }
    if (g_verbose) printf("[bridge] entity %u '%s' -> track %s (pt H264=%u H265=%u AV1=%u)\n",
                          e->id, req.path.c_str(), mid.c_str(), b->pt[2], b->pt[3], b->pt[4]);
    b->track = track;
    b->rtp   = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, b->pt[2] ? b->pt[2] : 96, 90000);
    std::lock_guard<std::mutex> g(c->mu);
    /* a re-offer for the same (path, call) replaces the old line */
    for (auto &old : e->tracks)
        if (old->path == b->path && old->call == b->call){ old = b; c->track_result[mid] = "ok"; return; }
    e->tracks.push_back(b);
    c->track_result[mid] = "ok";
}

/* rtc: bare = start and the reply carries the ICE servers, sdp = an offer plus tracks
 * and the reply carries our answer, candidate plus mid = one trickled candidate. */
static void op_rtc(Conn *c, const json &req, const json &seq){
    if (req.contains("sdp") || req.contains("candidate")){
        std::shared_ptr<rtc::PeerConnection> pc;
        { std::lock_guard<std::mutex> g(c->mu); pc = c->pc; }
        if (!pc){ reply_err(c, seq, "webrtc not started"); return; }
        try {
            if (req.contains("sdp")){
                {
                    std::lock_guard<std::mutex> g(c->mu);
                    if (c->rtc_seq_pending){ reply_err(c, seq, "an offer is already being answered"); return; }
                    c->track_result.clear();
                    if (req.contains("tracks") && req["tracks"].is_object())
                        for (auto &kv : req["tracks"].items()){
                            const json &t = kv.value();
                            if (!t.is_object()) continue;
                            TrackReq tr;
                            tr.id        = (uint16_t)t.value("id", 0);
                            tr.path      = t.value("path", std::string());
                            tr.call      = (uint32_t)t.value("call", 0);
                            tr.keep_data = t.value("keep_data", false);
                            c->track_map[kv.key()] = tr;
                        }
                    c->rtc_seq = seq;
                    c->rtc_seq_pending = true;
                }
                pc->setRemoteDescription(rtc::Description(req.value("sdp", ""), "offer"));
                pc->setLocalDescription();      /* the answer: onLocalDescription replies */
            } else {
                pc->addRemoteCandidate(rtc::Candidate(req.value("candidate", ""), req.value("mid", "")));
                reply_ok(c, seq, {});
            }
        } catch (const std::exception &ex){
            { std::lock_guard<std::mutex> g(c->mu); c->rtc_seq_pending = false; }
            reply_err(c, seq, std::string("webrtc: ") + ex.what());
        }
        return;
    }
    if (!g_rtc_enabled){ reply_err(c, seq, "webrtc disabled on this bridge"); return; }
    {
        std::lock_guard<std::mutex> g(c->mu);
        if (c->pc){ reply_err(c, seq, "webrtc already started"); return; }
    }
    rtc::Configuration cfg;
    for (const std::string &u : g_ice) cfg.iceServers.emplace_back(u);
    cfg.disableAutoNegotiation = true;    /* every answer is ours to trigger */
    cfg.forceMediaTransport    = true;    /* video lines may arrive in a later offer */
    if (g_port_lo){ cfg.portRangeBegin = g_port_lo; cfg.portRangeEnd = g_port_hi; }
    cfg.mtu = g_mtu;   /* sizes DTLS and SCTP, the RTP packetizers follow it too */
    auto pc = std::make_shared<rtc::PeerConnection>(cfg);
    std::weak_ptr<Conn> w = c->self;
    pc->onLocalDescription([w](rtc::Description d){
        auto keep = w.lock();
        if (!keep) return;
        Conn *c = keep.get();
        json seq, tracks = json::object(); bool pending;
        {
            std::lock_guard<std::mutex> g(c->mu);
            pending = c->rtc_seq_pending; seq = c->rtc_seq; c->rtc_seq_pending = false;
            for (auto &kv : c->track_result) tracks[kv.first] = kv.second;
        }
        if (pending) reply_ok(c, seq, { {"sdp", std::string(d)}, {"tracks", tracks} });
    });
    pc->onLocalCandidate([w](rtc::Candidate cand){
        auto keep = w.lock();
        if (!keep) return;
        send_json(keep.get(), { {"op", "rtc"}, {"candidate", cand.candidate()}, {"mid", cand.mid()} });
    });
    pc->onTrack([w](std::shared_ptr<rtc::Track> track){
        auto keep = w.lock();
        if (keep) rtc_on_track(keep.get(), std::move(track));
    });
    pc->onStateChange([w](rtc::PeerConnection::State s){
        auto keep = w.lock();
        if (!keep) return;
        Conn *c = keep.get();
        const char *name = s == rtc::PeerConnection::State::Connected    ? "connected"
                         : s == rtc::PeerConnection::State::Connecting   ? "connecting"
                         : s == rtc::PeerConnection::State::Disconnected ? "disconnected"
                         : s == rtc::PeerConnection::State::Failed       ? "failed"
                         : s == rtc::PeerConnection::State::Closed       ? "closed" : "new";
        bool up = s == rtc::PeerConnection::State::Connected;
        c->rtc_up = up;
        rtc_event(c, name, std::string("webrtc ") + name);
        if (!up) return;
        /* every entity gets its channel (a late connection covers those created before) */
        std::vector<Entity *> ents;
        {
            std::lock_guard<std::mutex> g(c->mu);
            for (auto &kv : c->ents) ents.push_back(kv.second.get());
        }
        for (Entity *e : ents) rtc_attach(c, e);
    });
    { std::lock_guard<std::mutex> g(c->mu); c->pc = pc; }
    reply_ok(c, seq, { {"ice_servers", g_ice} });
}

static bool starts_with_startcode(const uint8_t *p, size_t n){
    return n >= 3 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (n >= 4 && p[2] == 0 && p[3] == 1));
}

/* the keyframe flag, or the bitstream's own say-so (an IDR NAL) when the publisher left it unset */
static bool is_keyframe(int codec, const uint8_t *p, size_t n, bool flag){
    if (flag || codec == 4) return flag;
    for (size_t i = 0; i + 3 < n; i++){
        if (p[i] != 0 || p[i + 1] != 0) continue;
        size_t h = p[i + 2] == 1 ? i + 3 : (p[i + 2] == 0 && i + 4 < n && p[i + 3] == 1) ? i + 4 : 0;
        if (!h || h >= n) continue;
        int t = codec == 2 ? (p[h] & 0x1f) : ((p[h] >> 1) & 0x3f);
        if (codec == 2 ? t == 5 : (t == 19 || t == 20)) return true;
    }
    return false;
}

/* one encoded frame onto a binding's track. true when the track carries this stream, false
 * when the frame path must. Runs on the thread delivering the stream, serializing state. */
static bool media_send(Conn *c, Entity *e, TrackBinding *b, int codec, bool flag, int64_t pts,
                       const uint8_t *bs, size_t len, uint64_t written_us){
    if (!c->rtc_up || !b->track || !b->track->isOpen()) return false;
    if (codec < 2 || codec > 4 || len == 0 || !b->pt[codec]) return false;
    bool key = is_keyframe(codec, bs, len, flag);
    int sep = codec == 4 ? 0 : (starts_with_startcode(bs, len) ? 1 : 2);
    if (codec != b->codec || sep != b->sep){
        auto nsep = sep == 1 ? rtc::NalUnit::Separator::StartSequence : rtc::NalUnit::Separator::Length;
        std::shared_ptr<rtc::MediaHandler> pk;
        b->rtp->payloadType = b->pt[codec];
        size_t frag = g_mtu - 12 - 8 - 40 - 10;             /* RTP / UDP / IPv6 / SRTP tag */
        if (codec == 2)      pk = std::make_shared<rtc::H264RtpPacketizer>(nsep, b->rtp, frag);
        else if (codec == 3) pk = std::make_shared<rtc::H265RtpPacketizer>(nsep, b->rtp, frag);
        else pk = std::make_shared<rtc::AV1RtpPacketizer>(rtc::AV1RtpPacketizer::Packetization::TemporalUnit, b->rtp, frag);
        pk->addToChain(std::make_shared<rtc::RtcpSrReporter>(b->rtp));
        pk->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        b->track->setMediaHandler(pk);
        b->codec = codec; b->sep = sep; b->keyed = false;
    }
    if (!b->keyed){
        if (!key){ note_drop(c, e); return true; }   /* a decoder must start on a keyframe */
        b->keyed = true;
    }
    uint64_t us = pts > 0 ? (uint64_t)pts : written_us ? written_us : now_wall_us();
    b->rtp->timestamp = (uint32_t)(us * 9 / 100);          /* microseconds to the 90 kHz clock */
    try { b->track->send(reinterpret_cast<const std::byte *>(bs), len); }
    catch (const std::exception &){ return false; }
    return true;
}

/* the message with the tail frame `ordinal` emptied (the pixels went by track) */
static std::string strip_tail_frame(ramble::Bytes data, uint32_t fixed, int ordinal){
    const uint8_t *p = data.data();
    size_t n = data.size();
    std::string out;
    if (fixed > n) return std::string((const char *)p, n);
    out.assign((const char *)p, fixed);
    size_t pos = fixed;
    for (int k = 0; pos + 4 <= n; k++){
        uint32_t len = r32(p + pos);
        if (pos + 4 + len > n) break;
        if (k == ordinal){ uint8_t z[4] = { 0, 0, 0, 0 }; out.append((const char *)z, 4); }
        else out.append((const char *)p + pos, 4 + len);
        pos += 4 + len;
    }
    return out;
}

/* Route one stream sample of e through its track bindings: every bound VideoFrame goes onto
 * its track and the forwarded message has that field emptied. Returns the payload to forward. */
static ramble::Bytes media_route(Conn *c, Entity *e, uint32_t call, ramble::Bytes data,
                               const ramble::detail::RambleSchema *schema, uint64_t written_us,
                               std::string &scratch){
    std::vector<std::shared_ptr<TrackBinding>> bindings;
    {
        std::lock_guard<std::mutex> g(c->mu);
        for (auto &b : e->tracks) if (b->call == 0 || b->call == call) bindings.push_back(b);
    }
    if (bindings.empty() || !schema) return data;
    ramble::detail::RambleBytes msg = ramble::detail::ramble_bytes(data.data(), data.size());
    ramble::Bytes out = data;
    for (auto &b : bindings){
        int      codec = (int)ramble::detail::ramble_get_uint(msg, schema, b->f_codec.c_str());
        bool     flag  = ramble::detail::ramble_get_uint(msg, schema, b->f_key.c_str()) != 0;
        int64_t  pts   = ramble::detail::ramble_get_int(msg, schema, b->f_pts.c_str());
        ramble::detail::RambleBytes bs = ramble::detail::ramble_get_array(msg, schema, b->f_data.c_str());
        if (!bs.data) continue;
        if (media_send(c, e, b.get(), codec, flag, pts, bs.data, bs.len, written_us) && !b->keep_data){
            scratch = strip_tail_frame(data, b->fixed, b->data_ordinal);
            out = ramble::Bytes((const uint8_t *)scratch.data(), scratch.size());
        }
    }
    return out;
}

static void rtc_teardown(Conn *c){
    std::shared_ptr<rtc::PeerConnection> pc;
    std::vector<std::shared_ptr<rtc::DataChannel>> dcs;
    std::vector<std::shared_ptr<rtc::Track>> tracks;
    {
        std::lock_guard<std::mutex> g(c->mu);
        pc = std::move(c->pc);
        for (auto &kv : c->ents){
            if (kv.second->dc) dcs.push_back(std::move(kv.second->dc));
            for (auto &b : kv.second->tracks) if (b->track) tracks.push_back(std::move(b->track));
            kv.second->tracks.clear();
        }
    }
    c->rtc_up = false;
    for (auto &dc : dcs){ dc->resetCallbacks(); dc->close(); }
    for (auto &tr : tracks){ tr->resetCallbacks(); tr->close(); }
    if (pc){ pc->resetCallbacks(); pc->close(); }
}
#endif

/* The match state push: recompute each entity's match summary with read only queries and
 * push the ones that changed. Runs on the service thread and the ticker, the cache dedupes. */
static void push_match_states(Conn *c){
    if (!c->node || !c->ws) return;
    std::vector<Entity *> ents;
    {
        std::lock_guard<std::mutex> g(c->mu);
        for (auto &kv : c->ents) ents.push_back(kv.second.get());
    }
    for (Entity *e : ents){
        int count = 0, ready = 1;
        switch (e->kind){
        case Entity::Topic:      count = e->topic.match_count(); ready = e->topic.ready() ? 1 : 0; break;
        case Entity::FnDef:      count = e->fndef.caller_count(); break;
        case Entity::TaskDef:    count = e->taskdef.caller_count(); break;
        case Entity::VarDef:     count = e->vardef.remote_count(); break;
        case Entity::FnRemote:   count = e->fnrem.has_definition() ? 1 : 0;   ready = count; break;
        case Entity::TaskRemote: count = e->taskrem.has_definition() ? 1 : 0; ready = count; break;
        case Entity::VarRemote:  count = e->varrem.has_definition() ? 1 : 0;  ready = count; break;
        }
        int packed = count * 2 + ready;
        {
            std::lock_guard<std::mutex> g(c->mu);
            if (e->last_match == packed) continue;
            e->last_match = packed;
        }
        send_json(c, { {"op", "match"}, {"id", e->id}, {"count", count}, {"ready", ready != 0} });
    }
}

/* the entity ids bound to a node topic index (several when a client created one name twice) */
static std::vector<uint16_t> ids_for_index(Conn *c, uint16_t index){
    std::vector<uint16_t> ids;
    std::lock_guard<std::mutex> g(c->mu);
    auto it = c->by_index.find(index);
    if (it != c->by_index.end()) for (Entity *e : it->second) ids.push_back(e->id);
    return ids;
}

/* Format one event as JSON and send it. Runs inside the ramble event handler (on the
 * node's service thread, or on the connection thread for events a control op triggered). */
static void send_event(Conn *c, const ramble::Event &ev){
    if (!c->ws) return;
    json e = { {"op", "event"}, {"text", ev.to_string()} };
    std::vector<uint16_t> ids;
    switch (ev.kind()){
    case ramble::EventKind::PeerUp:
        e["event"] = "peer_up"; e["peer"] = ev.peer();
        break;
    case ramble::EventKind::PeerDown:
        e["event"] = "peer_down"; e["peer"] = ev.peer();
        break;
    case ramble::EventKind::PeerInterest:
        e["event"] = "peer_interest"; e["peer"] = ev.peer();
        e["publishes"] = ev.publish_topics(); e["receives"] = ev.receive_topics();
        break;
    case ramble::EventKind::MessageLost:
        e["event"] = "msg_lost"; e["peer"] = ev.peer();
        e["first"] = ev.lost_first(); e["count"] = ev.lost_count();
        ids = ids_for_index(c, ev.topic());
        break;
    case ramble::EventKind::Error:   /* one catch-all: "text" carries the message, "error" the code */
        e["event"] = "error"; e["error"] = (int)ev.error();
        if (!ev.topic_name().empty()) e["topic_name"] = std::string(ev.topic_name());
        if (ev.peer())                e["peer"]       = ev.peer();
        if (ev.os_error())            e["os_error"]   = ev.os_error();
        if (ev.topic()) ids = ids_for_index(c, ev.topic());
        break;
    default:
        e["event"] = "unknown";
        break;
    }
    if (ids.empty()) send_json(c, e);
    else for (uint16_t id : ids){ e["id"] = id; send_json(c, e); }
    if (ev.kind() == ramble::EventKind::PeerUp || ev.kind() == ramble::EventKind::PeerDown ||
        ev.kind() == ramble::EventKind::PeerInterest)
        push_match_states(c);
}

/* Delivery: one message to one DATA frame per entity bound to the topic index, or the media
 * track. Runs on the service thread. Pattern channels never reach this handler. */
static void deliver_message(Conn *c, const ramble::MessageView &m){
    if (!c->ws) return;
    std::vector<Entity *> targets;
    {
        std::lock_guard<std::mutex> g(c->mu);
        auto it = c->by_index.find(m.topic_index());
        if (it != c->by_index.end()) targets = it->second;
    }
    for (Entity *e : targets){
        Frame f;
        f.op = OP_DATA; f.id = e->id; f.peer = m.publisher_id(); f.written = m.written_us();
        f.capture = m.capture_us();
        f.payload = m.data();
#if RAMBLE_BRIDGE_WEBRTC
        std::string scratch;
        f.payload = media_route(c, e, 0, m.data(), m.raw_schema(), m.written_us(), scratch);
#endif
        send_frame(c, e, f);
    }
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

    ramble::NodeOptions o;
    o.domain               = (uint16_t)req.value("domain", 0);
    o.max_topics           = (uint16_t)req.value("max_topics", 0);
    o.disable_shm          = req.value("disable_shm", false);
    o.multicast_interface  = req.value("interface", std::string());
    o.fragment_size        = (uint16_t)req.value("fragment_size", 0);
    o.recv_buffer_bytes    = (uint32_t)req.value("recv_buffer_bytes", 0);
    o.send_buffer_bytes    = (uint32_t)req.value("send_buffer_bytes", 0);
    o.announce_interval_us = (uint32_t)req.value("announce_interval_ms", 0) * 1000u;
    o.peer_timeout_us      = (uint32_t)req.value("peer_timeout_ms", 0) * 1000u;
    o.max_peers            = (uint16_t)req.value("max_peers", 0);
    o.match_wait_ms        = req.value("match_wait_ms", 0);
    o.disable_logs         = req.value("disable_logs", false);
    o.disable_meta         = req.value("disable_meta", false);
    o.disable_error_logs   = req.value("disable_error_logs", false);
    o.fetch_details        = req.value("fetch_details", false);
    for (const auto &s : req.value("seed_peers", std::vector<std::string>{}))
        o.seed_peers.push_back(s);   /* "ip" or "ip:port", the wrapper rejects bad ones */
    o.unicast_only         = req.value("unicast_only", false);
    o.self_ip              = req.value("self_ip", std::string());
    o.advertise_port       = (uint16_t)req.value("advertise_port", 0);
    if (o.max_topics == 0) o.max_topics = 8;

    /* handlers capture the stable Conn*. The Conn always outlives its node, since
     * conn_close resets the node first */
    auto on_msg = [c](const ramble::MessageView &m){ deliver_message(c, m); };
    auto on_evt = [c](const ramble::Event    &e){ send_event(c, e); };

    try {
        c->node.emplace(name, on_msg, on_evt, o);
    } catch (const ramble::Error &e){
        reply_err(c, seq, std::string("ramble_node_open failed: ") + e.what());
        return;
    }
    c->name = name;
    c->node->start();   /* the node's own service thread drives everything */

    c->stop = false;
    std::shared_ptr<Conn> keep = c->self.lock();
    c->ticker = std::thread([keep, c]{
        while (!c->stop.load()){
            std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
            if (c->stop.load()) break;
            push_match_states(c);
        }
    });

    json r = { {"proto", kProtoVersion}, {"name", name} };
#if RAMBLE_BRIDGE_WEBRTC
    r["webrtc"] = g_rtc_enabled != 0;
#else
    r["webrtc"] = false;
#endif
    reply_ok(c, seq, r);
    if (g_verbose) printf("[bridge] node '%s' opened\n", name.c_str());
}

/* compile the DSL text at `key` (optional). Returns 0 + replies on error. */
static int compile_schema(Conn *c, const json &req, const json &seq, const char *key,
                          std::optional<ramble::Schema> *out){
    std::string text = req.value(key, "");
    if (text.empty()) return 1;
    std::string err;
    *out = ramble::Schema::compile(text, &err);
    if (!*out){ reply_err(c, seq, std::string("schema error in ") + key + ": " + err); return 0; }
    return 1;
}

/* the schema blocks of a create reply, under the keys the request used */
static void add_schema(json &r, const char *key, const std::optional<ramble::Schema> &s){
    if (s) r[key] = schema_json(*s);
}

/* register a built entity. A thread safe insert, the pointer stays stable until close */
static Entity *ent_add(Conn *c, std::unique_ptr<Entity> e){
    std::lock_guard<std::mutex> g(c->mu);
    Entity *raw = e.get();
    c->ents.emplace(raw->id, std::move(e));
    if (raw->kind == Entity::Topic) c->by_index[raw->topic.index()].push_back(raw);
    return raw;
}

static ramble::FunctionOptions fn_opts(const json &req){
    ramble::FunctionOptions o;
    o.backpressure_wait_us = (uint32_t)req.value("backpressure_wait_ms", 0) * 1000u;
    o.timeout_us           = (uint32_t)req.value("timeout_ms", 0) * 1000u;
    o.keep_last            = (uint16_t)req.value("keep_last", 0);
    return o;
}

static ramble::TaskOptions task_opts(const json &req){
    ramble::TaskOptions o;
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

/* A reflect_from_mesh handle took its types from the mesh: read them back off this node's
 * entity walk into the reply, so the client codes against the adopted layout. */
static void fill_reflected(Conn *c, Entity *e, ramble::EntityKind kind, const std::string &name, json &r){
    for (const ramble::Entity &ent : c->node->entities()){
        if (ent.kind != kind || ent.name != name) continue;
        e->reliable = ent.reliable || kind != ramble::EntityKind::Topic;
        r["reliable"] = e->reliable;
        r["reflected"] = true;
        if (kind == ramble::EntityKind::Topic || kind == ramble::EntityKind::Variable){
            e->value_schema = ent.schema;
            if (!ent.schema.empty()) r["schema"] = schema_json(ent.schema);
        } else {
            e->value_schema = ent.schema;
            e->prg_schema   = ent.progress_schema;
            if (!ent.schema.empty())          r["req"] = schema_json(ent.schema);
            if (!ent.progress_schema.empty()) r["prg"] = schema_json(ent.progress_schema);
            if (!ent.rsp_schema.empty())      r["rsp"] = schema_json(ent.rsp_schema);
        }
        return;
    }
    r["reflected"] = false;   /* nothing on the mesh yet: untyped until refresh */
}

static int entity_kind_from(const std::string &s, ramble::EntityKind *out){
    if (s == "topic")    { *out = ramble::EntityKind::Topic;    return 1; }
    if (s == "function") { *out = ramble::EntityKind::Function; return 1; }
    if (s == "task")     { *out = ramble::EntityKind::Task;     return 1; }
    if (s == "variable") { *out = ramble::EntityKind::Variable; return 1; }
    return 0;
}

/* refresh: re type a reflect_from_mesh handle in place when the mesh moved. The reply
 * carries the current tables either way, retyped says whether they changed */
static void op_refresh(Conn *c, const json &req, const json &seq){
    Entity *e = ent_get(c, (uint16_t)req.value("id", 0));
    if (!e){ reply_err(c, seq, "no such entity"); return; }
    bool retyped = false;
    ramble::EntityKind kind = ramble::EntityKind::Topic;
    switch (e->kind){
    case Entity::Topic:      retyped = e->topic.refresh();   kind = ramble::EntityKind::Topic;    break;
    case Entity::FnDef:      retyped = e->fndef.refresh();   kind = ramble::EntityKind::Function; break;
    case Entity::FnRemote:   retyped = e->fnrem.refresh();   kind = ramble::EntityKind::Function; break;
    case Entity::TaskDef:    retyped = e->taskdef.refresh(); kind = ramble::EntityKind::Task;     break;
    case Entity::TaskRemote: retyped = e->taskrem.refresh(); kind = ramble::EntityKind::Task;     break;
    case Entity::VarDef:     retyped = e->vardef.refresh();  kind = ramble::EntityKind::Variable; break;
    case Entity::VarRemote:  retyped = e->varrem.refresh();  kind = ramble::EntityKind::Variable; break;
    }
    json r = { {"id", e->id}, {"retyped", retyped} };
    fill_reflected(c, e, kind, e->name, r);
    reply_ok(c, seq, r);
}

/* mesh and mesh_find: the whole mesh folded, one entity per kind and name, schemas from
 * the provider. epoch moves on every change */
static void op_mesh(Conn *c, const json &req, const json &seq, bool find){
    if (find){
        ramble::EntityKind kind;
        if (!entity_kind_from(req.value("kind", ""), &kind)){ reply_err(c, seq, "bad kind"); return; }
        std::optional<ramble::Entity> e = c->node->mesh_find(kind, req.value("name", ""));
        reply_ok(c, seq, { {"entity", e ? entity_json(*e) : json(nullptr)}, {"epoch", c->node->mesh_epoch()} });
        return;
    }
    json arr = json::array();
    for (const ramble::Entity &e : c->node->mesh()) arr.push_back(entity_json(e));
    reply_ok(c, seq, { {"entities", arr}, {"epoch", c->node->mesh_epoch()} });
}

/* a definition's incoming request: the CALL frame carries the caller and its name, and the
 * bridge parks the reply until the client's RESULT frame answers */
static void push_request(Conn *c, Entity *e, uint32_t req_id, uint32_t caller,
                         std::string_view caller_name, uint64_t written_us, ramble::Bytes data){
    Frame f;
    f.op = OP_CALL; f.id = e->id; f.seq = req_id; f.peer = caller; f.written = written_us;
    f.text = caller_name; f.payload = data;
    send_frame(c, e, f);
}

static void create_topic(Conn *c, const json &req, const json &seq, std::unique_ptr<Entity> e){
    std::string name = req.value("name", "");
    ramble::Role role;
    if (!role_from(req.value("role", "pubsub"), &role)){ reply_err(c, seq, "bad role"); return; }

    ramble::Qos qos;
    qos.reliability          = req.value("reliable", false) ? ramble::Reliability::Reliable
                                                            : ramble::Reliability::BestEffort;
    qos.keep_last            = (uint16_t)req.value("keep_last", 0);
    qos.catch_up             = (uint16_t)req.value("catch_up", 0);
    qos.max_message_bytes    = (uint32_t)req.value("max_message_bytes", 0);
    qos.heartbeat_us         = (uint32_t)req.value("heartbeat_ms", 0) * 1000u;
    qos.repair_delay_us      = (uint32_t)req.value("repair_delay_ms", 0) * 1000u;
    qos.backpressure_wait_us = (uint32_t)req.value("backpressure_wait_ms", 0) * 1000u;
    qos.shm_max_bytes        = (uint32_t)req.value("shm_max_bytes", 0);
    qos.max_rate_hz          = (uint16_t)req.value("max_rate_hz", 0);

    std::optional<ramble::Schema> schema;
    if (!compile_schema(c, req, seq, "schema", &schema)) return;

    bool reflect = req.value("reflect", false);
    try {
        if (reflect) e->topic = c->node->create_topic(name, role, ramble::reflect_from_mesh, qos);
        else         e->topic = c->node->create_topic(name, role, schema ? &*schema : nullptr, qos);
    } catch (const ramble::Error &err){
        reply_err(c, seq, std::string("create failed: ") + err.what());
        return;
    }
    e->reliable = qos.reliability == ramble::Reliability::Reliable;
    if (schema) e->value_schema = *schema;
    Entity *ent = ent_add(c, std::move(e));
    json r = { {"id", ent->id}, {"reliable", ent->reliable} };
    if (reflect) fill_reflected(c, ent, ramble::EntityKind::Topic, name, r);
    else         add_schema(r, "schema", schema);
    reply_ok(c, seq, r);
#if RAMBLE_BRIDGE_WEBRTC
    rtc_attach(c, ent);   /* its video line, if any, comes with the client's next offer */
#endif
}

/* function_definition: every request is deferred to the client as a CALL frame. The parked
 * Deferred completes when the RESULT frame arrives, or at disconnect. */
static void create_function(Conn *c, const json &req, const json &seq, std::unique_ptr<Entity> e, bool definition){
    std::string name = req.value("name", "");
    std::optional<ramble::Schema> rq, rs;
    if (!compile_schema(c, req, seq, "req", &rq)) return;
    if (!compile_schema(c, req, seq, "rsp", &rs)) return;
    Entity *ent = e.get();
    uint16_t id = ent->id;
    bool reflect = req.value("reflect", false);
    try {
        if (definition){
            auto handler = [c, ent](ramble::Request<> &r){
                uint32_t req_id;
                { std::lock_guard<std::mutex> g(c->mu); req_id = ++c->next_req; }
                ramble::Bytes data = r.data();
                uint32_t caller = r.caller();
                std::string caller_name(r.caller_name());
                uint64_t written_us = r.written_us();   /* read before defer() */
                ramble::Deferred<> d = r.defer();
                { std::lock_guard<std::mutex> g(c->mu); c->parked.emplace(req_id, std::move(d)); }
                push_request(c, ent, req_id, caller, caller_name, written_us, data);
            };
            if (reflect) ent->fndef = ramble::FunctionDefinition<>(*c->node, name, ramble::reflect_from_mesh, handler, fn_opts(req));
            else ent->fndef = ramble::FunctionDefinition<>(*c->node, name,
                                  rq ? &*rq : nullptr, rs ? &*rs : nullptr, handler, fn_opts(req));
        } else {
            if (reflect) ent->fnrem = ramble::RemoteFunction<>(*c->node, name, ramble::reflect_from_mesh, fn_opts(req));
            else ent->fnrem = ramble::RemoteFunction<>(*c->node, name,
                                  rq ? &*rq : nullptr, rs ? &*rs : nullptr, fn_opts(req));
        }
    } catch (const ramble::Error &err){
        reply_err(c, seq, std::string("create failed: ") + err.what());
        return;
    }
    ent_add(c, std::move(e));
    json r = { {"id", id}, {"reliable", true} };
    if (reflect) fill_reflected(c, ent, ramble::EntityKind::Function, name, r);
    else { add_schema(r, "req", rq); add_schema(r, "rsp", rs); }
    reply_ok(c, seq, r);
#if RAMBLE_BRIDGE_WEBRTC
    rtc_attach(c, ent);
#endif
}

/* task_definition: as a function plus the progress schema and options. Every request is
 * deferred, the client streams PROGRESS and answers with RESULT, a cancel is a CANCEL frame. */
static void create_task(Conn *c, const json &req, const json &seq, std::unique_ptr<Entity> e, bool definition){
    std::string name = req.value("name", "");
    std::optional<ramble::Schema> rq, pg, rs;
    if (!compile_schema(c, req, seq, "req", &rq)) return;
    if (!compile_schema(c, req, seq, "prg", &pg)) return;
    if (!compile_schema(c, req, seq, "rsp", &rs)) return;
    Entity *ent = e.get();
    uint16_t id = ent->id;
    bool reflect = req.value("reflect", false);
    try {
        if (definition){
            auto handler = [c, ent](ramble::TaskRequest<> &r){
                uint32_t req_id;
                { std::lock_guard<std::mutex> g(c->mu); req_id = ++c->next_req; }
                ramble::Bytes data = r.data();
                uint32_t caller = r.caller();
                std::string caller_name(r.caller_name());
                uint64_t written_us = r.written_us();
                auto tk = std::make_shared<ParkedTask>();
                tk->ent = ent->id;
                tk->pt  = r.defer();                    /* implies RUNNING to the caller */
                { std::lock_guard<std::mutex> g(c->mu); c->parked_tasks.emplace(req_id, tk); }
                push_request(c, ent, req_id, caller, caller_name, written_us, data);
            };
            if (reflect) ent->taskdef = ramble::TaskDefinition<>(*c->node, name, ramble::reflect_from_mesh, handler, task_opts(req));
            else ent->taskdef = ramble::TaskDefinition<>(*c->node, name,
                                    rq ? &*rq : nullptr, pg ? &*pg : nullptr, rs ? &*rs : nullptr,
                                    handler, task_opts(req));
            /* the token cannot name its request through the public wrapper, so scan this
               definition's parked requests for the newly-cancelled one (cancel_pushed dedupes) */
            ent->taskdef.on_cancel([c, ent](uint64_t){
                std::vector<std::pair<uint32_t, std::shared_ptr<ParkedTask>>> snap;
                {
                    std::lock_guard<std::mutex> g(c->mu);
                    for (auto &kv : c->parked_tasks) if (kv.second->ent == ent->id) snap.push_back(kv);
                }
                for (auto &pr : snap)
                    if (pr.second->pt.cancelled() && !pr.second->cancel_pushed.exchange(true)){
                        Frame f; f.op = OP_CANCEL; f.id = ent->id; f.seq = pr.first;
                        send_frame(c, ent, f);
                    }
            });
        } else {
            if (reflect) ent->taskrem = ramble::RemoteTask<>(*c->node, name, ramble::reflect_from_mesh, task_opts(req));
            else ent->taskrem = ramble::RemoteTask<>(*c->node, name,
                                    rq ? &*rq : nullptr, pg ? &*pg : nullptr, rs ? &*rs : nullptr,
                                    task_opts(req));
        }
    } catch (const ramble::Error &err){
        reply_err(c, seq, std::string("create failed: ") + err.what());
        return;
    }
    if (pg) ent->prg_schema = *pg;
    ent_add(c, std::move(e));
    json r = { {"id", id}, {"reliable", true} };
    if (reflect) fill_reflected(c, ent, ramble::EntityKind::Task, name, r);
    else { add_schema(r, "req", rq); add_schema(r, "prg", pg); add_schema(r, "rsp", rs); }
    reply_ok(c, seq, r);
#if RAMBLE_BRIDGE_WEBRTC
    rtc_attach(c, ent);
#endif
}

static void create_variable(Conn *c, const json &req, const json &seq, std::unique_ptr<Entity> e, bool definition){
    std::string name = req.value("name", "");
    std::optional<ramble::Schema> sc;
    if (!compile_schema(c, req, seq, "schema", &sc)) return;

    ramble::VariableOptions<> o;
    o.read_only            = req.value("read_only", false);
    o.allow_force          = req.value("allow_force", false);
    o.catch_up             = (uint16_t)req.value("catch_up", 0);
    o.keep_last            = (uint16_t)req.value("keep_last", 0);
    o.backpressure_wait_us = (uint32_t)req.value("backpressure_wait_ms", 0) * 1000u;

    Entity *ent = e.get();
    uint16_t id = ent->id;
    bool reflect = req.value("reflect", false);
    try {
        if (reflect){
            if (definition) ent->vardef = ramble::VariableDefinition<>(*c->node, name, ramble::reflect_from_mesh, o);
            else            ent->varrem = ramble::RemoteVariable<>(*c->node, name, ramble::reflect_from_mesh, o);
        } else {
            if (definition) ent->vardef = ramble::VariableDefinition<>(*c->node, name, sc ? &*sc : nullptr, o);
            else            ent->varrem = ramble::RemoteVariable<>(*c->node, name, sc ? &*sc : nullptr, o);
        }
    } catch (const ramble::Error &err){
        reply_err(c, seq, std::string("create failed: ") + err.what());
        return;
    }
    if (sc) ent->value_schema = *sc;
    ent_add(c, std::move(e));
    json r = { {"id", id}, {"reliable", true} };
    if (reflect) fill_reflected(c, ent, ramble::EntityKind::Variable, name, r);
    else         add_schema(r, "schema", sc);
    reply_ok(c, seq, r);
#if RAMBLE_BRIDGE_WEBRTC
    rtc_attach(c, ent);
#endif

    /* value updates are pushed off the hooks: on_change on a state change, on_write on every
     * write. Registered after the create reply, since the registration replays the value. */
    bool wants_write = req.value("on_write", false);
    auto push_var = [c, ent](const ramble::VariableUpdate &u, uint8_t evbit){
        Frame f;
        f.op = OP_VAR; f.id = ent->id; f.flags = (uint8_t)((u.forced() ? 1 : 0) | evbit);
        f.peer = u.source(); f.written = u.written_us(); f.payload = u.value();
#if RAMBLE_BRIDGE_WEBRTC
        std::string scratch;
        f.payload = media_route(c, ent, 0, u.value(), u.raw_schema(), u.written_us(), scratch);
#endif
        send_frame(c, ent, f);
    };
    if (definition){
        ent->vardef.on_change([push_var](const ramble::VariableUpdate &u){ push_var(u, 0); });
        if (wants_write) ent->vardef.on_write([push_var](const ramble::VariableUpdate &u){ push_var(u, 2); });
    } else {
        ent->varrem.on_change([push_var](const ramble::VariableUpdate &u){ push_var(u, 0); });
        if (wants_write) ent->varrem.on_write([push_var](const ramble::VariableUpdate &u){ push_var(u, 2); });
    }
}

/* create: the one constructor op. id is client chosen and doubles as the data channel id,
 * kind picks the entity. */
static void op_create(Conn *c, const json &req, const json &seq){
    int id = req.value("id", -1);
    if (id < 1 || id > 0xFFFE){ reply_err(c, seq, "bad id (1..65534)"); return; }
    if (req.value("name", "").empty()){ reply_err(c, seq, "missing name"); return; }
    if (ent_get(c, (uint16_t)id)){ reply_err(c, seq, "id already in use"); return; }
    std::string kind = req.value("kind", "");
    auto e = std::make_unique<Entity>();
    e->id   = (uint16_t)id;
    e->name = req.value("name", "");
    if      (kind == "topic")               { e->kind = Entity::Topic;      create_topic(c, req, seq, std::move(e)); }
    else if (kind == "function_definition") { e->kind = Entity::FnDef;      create_function(c, req, seq, std::move(e), true); }
    else if (kind == "remote_function")     { e->kind = Entity::FnRemote;   create_function(c, req, seq, std::move(e), false); }
    else if (kind == "task_definition")     { e->kind = Entity::TaskDef;    create_task(c, req, seq, std::move(e), true); }
    else if (kind == "remote_task")         { e->kind = Entity::TaskRemote; create_task(c, req, seq, std::move(e), false); }
    else if (kind == "variable_definition") { e->kind = Entity::VarDef;     create_variable(c, req, seq, std::move(e), true); }
    else if (kind == "remote_variable")     { e->kind = Entity::VarRemote;  create_variable(c, req, seq, std::move(e), false); }
    else reply_err(c, seq, "unknown kind: " + kind);
}

static Entity *topic_arg(Conn *c, const json &req, const json &seq){
    Entity *e = ent_get(c, (uint16_t)req.value("id", 0));
    if (!e || e->kind != Entity::Topic){ reply_err(c, seq, "no such topic"); return nullptr; }
    return e;
}

static void op_role(Conn *c, const json &req, const json &seq){
    ramble::Role role;
    if (!role_from(req.value("role", ""), &role)){ reply_err(c, seq, "bad role"); return; }
    Entity *e = topic_arg(c, req, seq);
    if (!e) return;
    if (e->topic.set_role(role) != ramble::SendStatus::Ok){ reply_err(c, seq, "set_role failed"); return; }
    reply_ok(c, seq, {});
}

static void op_drain(Conn *c, const json &req, const json &seq){
    Entity *e = topic_arg(c, req, seq);
    if (!e) return;
    reply_ok(c, seq, { {"drained", e->topic.drain(req.value("timeout_ms", 1000))} });
}

/* Runs on the WS handler thread and blocks only this connection's receives. The C wait
 * machinery works under the node's service thread. */
static void op_settle(Conn *c, const json &req, const json &seq){
    reply_ok(c, seq, { {"settled", c->node->settle(req.value("timeout_ms", -1))} });
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
    if (!found){ reply_ok(c, seq, { {"status", "not_pending"} }); return; }
    Entity *e = ent_get(c, ent);
    if (!e || e->kind != Entity::TaskRemote){ reply_err(c, seq, "no such task"); return; }
    ramble::SendStatus rc = e->taskrem.cancel(cid);
    const char *s = rc == ramble::SendStatus::Ok      ? "ok"
                  : rc == ramble::SendStatus::BadRole ? "no_cancel"   /* declared no_cancel */
                  : rc == ramble::SendStatus::State   ? "not_pending"
                  :                                   "error";
    reply_ok(c, seq, { {"status", s} });
}

/* ---- built-in logs (the @ramble/log topics) ---------------------------------------- */

static void op_log(Conn *c, const json &req, const json &seq){
    ramble::LogLevel level;
    if (!log_level_from(req.value("level", ""), &level)){ reply_err(c, seq, "bad log level"); return; }
    ramble::SendStatus rc = c->node->log(level, req.value("text", ""));
    if (rc != ramble::SendStatus::Ok){ reply_err(c, seq, std::string("log failed: ") + send_status_str(rc)); return; }
    reply_ok(c, seq, {});
}

/* subscribe to one or more levels' mesh wide log stream. Lines are pushed as JSON text
 * frames, idempotent per level. */
static void op_log_subscribe(Conn *c, const json &req, const json &seq){
    std::vector<std::string> levels = req.value("levels",
        std::vector<std::string>{ "error", "warn", "info" });
    for (const std::string &ls : levels){
        ramble::LogLevel level;
        if (!log_level_from(ls, &level)){ reply_err(c, seq, "bad log level: " + ls); return; }
        uint8_t bit = (uint8_t)(1u << (int)level);
        if (c->log_sub & bit) continue;
        bool ok = c->node->on_log(level, [c, level](const ramble::LogLine &l){
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

static void op_peers(Conn *c, const json &seq){
    json arr = json::array();
    for (const ramble::Peer &p : c->node->peers())
        arr.push_back({ {"id", p.id}, {"name", p.name}, {"address", p.address},
                        {"active", p.active}, {"fragment_size", p.fragment_size},
                        {"epoch", p.epoch}, {"last_heard_us", p.last_heard_us},
                        {"catching_up", p.catching_up},
                        {"rtt_us", p.rtt_us}, {"rtt_jitter_us", p.rtt_jitter_us},
                        {"rtt_min_us", p.rtt_min_us}, {"rtt_samples", p.rtt_samples} });
    reply_ok(c, seq, { {"peers", arr} });
}

static void op_entities(Conn *c, const json &seq){
    json arr = json::array();
    for (const ramble::Entity &e : c->node->entities()) arr.push_back(entity_json(e));
    reply_ok(c, seq, { {"entities", arr} });
}

static void op_peer_entities(Conn *c, const json &req, const json &seq){
    uint32_t peer = (uint32_t)req.value("peer", 0);
    json arr = json::array();
    for (const ramble::Entity &e : c->node->entities(peer))   /* a dropped peer gives its last view */
        arr.push_back(entity_json(e));
    reply_ok(c, seq, { {"peer", peer}, {"entities", arr} });
}

/* @ramble/meta query: an async directed call to a peer's endpoint. The reply fires later
 * from the poll thread echoing the request's seq and never faults, the client reads status. */
static void op_meta(Conn *c, const json &req, const json &seq){
    uint32_t peer     = (uint32_t)req.value("peer", 0);
    uint32_t sections = (uint32_t)req.value("sections", 0);
    ramble::SendStatus rc = c->node->meta_request(peer,
        [c, seq](const ramble::MetaSnapshot &s){
            json r = { {"valid", s.valid}, {"status", (int)s.status}, {"provider", s.provider} };
            if (s.valid) r["info"] = mapdict_to_json(s.info);
            reply_ok(c, seq, r);
        }, sections);
    if (rc != ramble::SendStatus::Ok)
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
    if      (op == "create")        op_create(c, req, seq);
    else if (op == "role")          op_role(c, req, seq);
    else if (op == "drain")         op_drain(c, req, seq);
    else if (op == "settle")        op_settle(c, req, seq);
    else if (op == "cancel")        op_cancel(c, req, seq);
    else if (op == "log")           op_log(c, req, seq);
    else if (op == "log_subscribe") op_log_subscribe(c, req, seq);
    else if (op == "meta")          op_meta(c, req, seq);
    else if (op == "peers")         op_peers(c, seq);
    else if (op == "entities")      op_entities(c, seq);
    else if (op == "peer_entities") op_peer_entities(c, req, seq);
    else if (op == "mesh")          op_mesh(c, req, seq, false);
    else if (op == "mesh_find")     op_mesh(c, req, seq, true);
    else if (op == "refresh")       op_refresh(c, req, seq);
#if RAMBLE_BRIDGE_WEBRTC
    else if (op == "rtc")           op_rtc(c, req, seq);
#else
    else if (op == "rtc")           reply_err(c, seq, "webrtc not built into this bridge");
#endif
    else reply_err(c, seq, "unknown op: " + op);
}

/* ---- data plane ------------------------------------------------------------------- */

/* the one call-outcome frame both patterns answer with */
static void push_result(Conn *c, Entity *e, uint32_t call, const ramble::ResponseView<> &rv){
    Frame f;
    f.op = OP_RESULT; f.flags = (uint8_t)rv.status(); f.id = e->id; f.seq = call;
    f.peer = rv.provider(); f.written = rv.written_us(); f.text = rv.message(); f.payload = rv.data();
    send_frame(c, e, f);
}

/* a call the bridge refused synchronously: answer Cancelled so the promise settles */
static void push_refused(Conn *c, Entity *e, uint32_t call){
    Frame f;
    f.op = OP_RESULT; f.flags = (uint8_t)ramble::CallStatus::Cancelled; f.id = e->id; f.seq = call;
    send_frame(c, e, f);
}

static void on_call(Conn *c, Entity *e, const Frame &f){
    uint32_t call = f.seq;
    if (e->kind == Entity::FnRemote){
        auto cb = [c, e, call](const ramble::ResponseView<> &rv){ push_result(c, e, call, rv); };
        ramble::SendStatus rc = e->fnrem.call_async(f.payload, cb);
        if (rc != ramble::SendStatus::Ok){ send_error_event(c, e->id, rc); push_refused(c, e, call); }
        return;
    }
    if (e->kind != Entity::TaskRemote){ send_error_event(c, e->id, ramble::SendStatus::BadRole); return; }
    /* a task call: progress frames (empty payload = the RUNNING ack) before the one
     * terminal response, which also releases the cancel mapping */
    auto on_prog = [c, e, call](const ramble::ProgressView<> &pv){
        Frame p;
        p.op = OP_PROGRESS; p.id = e->id; p.seq = call; p.peer = pv.provider();
        p.written = pv.written_us(); p.payload = pv.data();
#if RAMBLE_BRIDGE_WEBRTC
        std::string scratch;
        p.payload = media_route(c, e, call, pv.data(), pv.raw_schema(), pv.written_us(), scratch);
#endif
        send_frame(c, e, p);
    };
    auto on_rsp = [c, e, call](const ramble::ResponseView<> &rv){
        { std::lock_guard<std::mutex> g(c->mu); c->task_calls.erase(call); }
        push_result(c, e, call, rv);
    };
    /* map the client call id before launch so the terminal response never races the
     * insert. call_async fills the C call id through the pair in place */
    { std::lock_guard<std::mutex> g(c->mu); c->task_calls.emplace(call, std::make_pair(e->id, 0u)); }
    ramble::TaskCall tc = e->taskrem.call_async(f.payload, on_prog, on_rsp);
    if (!tc.ok()){
        { std::lock_guard<std::mutex> g(c->mu); c->task_calls.erase(call); }
        send_error_event(c, e->id, tc.status);
        push_refused(c, e, call);
        return;
    }
    std::lock_guard<std::mutex> g(c->mu);
    auto it = c->task_calls.find(call);
    if (it != c->task_calls.end()) it->second.second = tc.id;   /* absent = already answered */
}

/* PROGRESS from the client: one update on a parked task request */
static void on_progress(Conn *c, const Frame &f){
    std::shared_ptr<ParkedTask> tk;
    {
        std::lock_guard<std::mutex> g(c->mu);
        auto it = c->parked_tasks.find(f.seq);
        if (it != c->parked_tasks.end()) tk = it->second;
    }
    if (!tk) return;   /* completed/unknown: drop, like an unknown request reply */
    ramble::SendStatus rc = tk->pt.progress(f.payload);
    if (rc != ramble::SendStatus::Ok) send_error_event(c, tk->ent, rc);
}

/* RESULT from the client: the reply to a parked request (function or task) */
static void on_reply(Conn *c, const Frame &f){
    ramble::Deferred<> d;
    {
        std::lock_guard<std::mutex> g(c->mu);
        auto it = c->parked.find(f.seq);
        if (it != c->parked.end()){ d = std::move(it->second); c->parked.erase(it); }
    }
    if (d.valid()){
        if (f.flags == 0) d.complete(f.payload, f.text);
        else              d.fail(f.text, f.payload);
        return;
    }
    /* a parked TASK: status 0 = ok, 5 = cancelled (the handler honored a cancel),
     * anything else = app_error */
    std::shared_ptr<ParkedTask> tk;
    {
        std::lock_guard<std::mutex> g(c->mu);
        auto it = c->parked_tasks.find(f.seq);
        if (it == c->parked_tasks.end()) return;   /* unknown/duplicate: drop */
        tk = it->second;
        c->parked_tasks.erase(it);
    }
    if      (f.flags == 0) tk->pt.complete(f.payload, f.text);
    else if (f.flags == 5) tk->pt.complete_cancelled(f.text);
    else                   tk->pt.fail(f.text, f.payload);
}

/* one data-plane frame from either carrier */
static void on_frame(Conn *c, const uint8_t *p, size_t n){
    Frame f;
    if (!frame_parse(p, n, f)) return;
    if (!c->node){ send_error_event(c, f.id, ramble::SendStatus::NoTopic); return; }
    if (f.op == OP_RESULT){ on_reply(c, f); return; }
    if (f.op == OP_PROGRESS){ on_progress(c, f); return; }
    Entity *e = ent_get(c, f.id);
    if (!e){ send_error_event(c, f.id, ramble::SendStatus::NoTopic); return; }
    ramble::SendStatus rc = ramble::SendStatus::Ok;
    switch (f.op){
    case OP_DATA:
        rc = e->kind == Entity::Topic
               ? e->topic.send(f.payload, ramble::Timestamp{ (int64_t)f.capture })
               : ramble::SendStatus::BadRole;
        break;
    case OP_VAR: {
        if (e->kind != Entity::VarDef && e->kind != Entity::VarRemote){ rc = ramble::SendStatus::BadRole; break; }
        ramble::VariableDefinition<> &v = e->kind == Entity::VarDef
            ? e->vardef : static_cast<ramble::VariableDefinition<> &>(e->varrem);
        rc = f.flags == 0 ? v.set(f.payload)
           : f.flags == 1 ? v.force(f.payload)
           : f.flags == 2 ? v.unforce()
           : ramble::SendStatus::NoSys;
        break;
    }
    case OP_CALL:
        on_call(c, e, f);
        return;
    default:
        return;   /* reserved / server-only ops: drop */
    }
    if (rc != ramble::SendStatus::Ok) send_error_event(c, e->id, rc);
    /* no explicit flush: a send kicks the node's service thread awake */
}

/* ---- connection lifecycle --------------------------------------------------------- */

static void conn_close(const std::shared_ptr<Conn> &c){
    c->stop = true;
    if (c->ticker.joinable()) c->ticker.join();
#if RAMBLE_BRIDGE_WEBRTC
    rtc_teardown(c.get());   /* no WebRTC callback can reach the node once this returns */
#endif
    /* answer parked requests while the node is still alive, so remote callers get an
     * answer now instead of waiting out their timeout: functions fail, tasks cancel */
    std::unordered_map<uint32_t, ramble::Deferred<>>          parked;
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
        /* ~Node stops and joins the service thread first, so no handler is mid flight once it
           returns. It closes with a BYE and frees everything */
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
#if RAMBLE_BRIDGE_WEBRTC
        else if (!strcmp(argv[i], "--ice") && i + 1 < argc) g_ice.push_back(argv[++i]);
        else if (!strcmp(argv[i], "--no-webrtc")) g_rtc_enabled = 0;
        else if (!strcmp(argv[i], "--webrtc-debug")) g_rtc_debug = 1;
        else if (!strcmp(argv[i], "--mtu") && i + 1 < argc){
            int v = atoi(argv[++i]);
            if (v < 576 || v > 9000){ fprintf(stderr, "ramble_bridge: --mtu wants 576..9000\n"); return 1; }
            g_mtu = (size_t)v;
        }
        else if (!strcmp(argv[i], "--rtc-ports") && i + 1 < argc){
            int lo = 0, hi = 0;
            if (sscanf(argv[++i], "%d-%d", &lo, &hi) != 2 || lo < 1 || hi < lo || hi > 65535){
                fprintf(stderr, "ramble_bridge: --rtc-ports wants lo-hi\n"); return 1;
            }
            g_port_lo = (uint16_t)lo; g_port_hi = (uint16_t)hi;
        }
#endif
        else {
            printf("usage: ramble_bridge [--port 7480] [--bind 0.0.0.0] [--max-buffered bytes] [--verbose]\n"
#if RAMBLE_BRIDGE_WEBRTC
                   "                   [--ice stun:host:port | turn:user:pass@host:port]... [--rtc-ports lo-hi]\n"
                   "                   [--mtu 1200] [--no-webrtc] [--webrtc-debug]\n"
#endif
                   "One WebSocket connection = one Ramble node; see bridge/PROTOCOL.md.\n"
                   "--bind 0.0.0.0 exposes the bridge (and full mesh access) beyond this host.\n"
                   "--ice is handed to the client too, so one flag configures both ends.\n"
                   "--mtu bounds every WebRTC datagram (sent with DF set): lower it on a VPN or tunnel\n"
                   "  path that logs 'datagram is too large'.\n");
            return strcmp(argv[i], "--help") && strcmp(argv[i], "-h") ? 1 : 0;
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);   /* rare lifecycle prints, kept visible when redirected */
    std::set_terminate([]{
        const char *what = "unknown";
        try { std::exception_ptr p = std::current_exception(); if (p) std::rethrow_exception(p); }
        catch (const std::exception &e){ what = e.what(); }
        catch (...){ what = "non-standard exception"; }
        printf("[bridge] FATAL: terminate called: %s\n", what);
        fflush(stdout);
        abort();
    });
#ifdef _WIN32
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *ep) -> LONG {
        printf("[bridge] FATAL: unhandled exception 0x%08lx at %p\n",
               (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
        fflush(stdout);
        return EXCEPTION_EXECUTE_HANDLER;
    });
#endif
    ix::initNetSystem();
#if RAMBLE_BRIDGE_WEBRTC
    rtc::InitLogger(g_rtc_debug ? rtc::LogLevel::Verbose : g_verbose ? rtc::LogLevel::Info : rtc::LogLevel::Warning,
                    [](rtc::LogLevel, std::string msg){ printf("[webrtc] %s\n", msg.c_str()); });
    rtc::Preload();
#endif
    ix::WebSocketServer server(port, bind);
    server.setOnClientMessageCallback(
        [](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket &ws,
           const ix::WebSocketMessagePtr &msg){
            switch (msg->type){
            case ix::WebSocketMessageType::Open: {
                auto c = std::make_shared<Conn>();
                c->ws   = &ws;
                c->self = c;
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
                try {
                    if (msg->binary) on_frame(c.get(), (const uint8_t *)msg->str.data(), msg->str.size());
                    else             on_text(c.get(), msg->str);
                } catch (const std::exception &ex){
                    printf("[bridge] request failed: %s\n", ex.what());
                }
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
                if (c){
                    try { conn_close(c); }
                    catch (const std::exception &ex){ printf("[bridge] close failed: %s\n", ex.what()); }
                }
                break;
            }
            default:
                break;
            }
        });

    auto res = server.listen();
    if (!res.first){
        fprintf(stderr, "ramble_bridge: listen on %s:%d failed: %s\n", bind.c_str(), port, res.second.c_str());
        return 1;
    }
    server.start();
    printf("ramble_bridge listening on ws://%s:%d (one connection = one node%s)\n", bind.c_str(), port,
#if RAMBLE_BRIDGE_WEBRTC
           g_rtc_enabled ? ", webrtc data path on" : ", webrtc off"
#else
           ""
#endif
           );
    server.wait();
#if RAMBLE_BRIDGE_WEBRTC
    rtc::Cleanup();
#endif
    ix::uninitNetSystem();
    return 0;
}
