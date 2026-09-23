/* The two node test for rant.hpp. spec/testing.md lists the legs. Single process,
 * discovery pinned to loopback on isolated domains, never 0. Exit 0 = PASS. */
#include "rant.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <functional>
#include <thread>

static int g_failures = 0;
static void chk(const char* what, bool ok) {
    std::printf("  %s  %s\n", ok ? "ok " : "FAIL", what);
    if (!ok) g_failures++;
}
static bool wait_for(int timeout_ms, const std::function<bool()>& pred,
                     rant::Node* pump = nullptr) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        if (pump) pump->poll(2);
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

/* leg 1 and 2: the dynamic schema API */

static const char SCHEMA[] =
    "Sensor{ seq: u32, name: string<16>, note: string, samples: f32[], extras: map }";

static const char NOTE[]    = "a long unbounded note well over sixteen bytes";
static const float SAMPLES[] = { 1.5f, -2.25f, 3.75f };

/* build and send one message. false if a setter was refused or the send failed */
static bool send_one(rant::Publisher<rant::Bytes>& pub, const rant::Schema& schema, uint32_t seq) {
    rant::MessageBuilder s(schema);
    rant::MapWriter mw;
    mw.put_uint("battery", 87).put_int("signed", -5)
      .put_string("state", "docked").put_bool("ok", true);
    mw.open_array("temps"); mw.put_f64(nullptr, 36.2).put_f64(nullptr, 34.9); mw.close();
    mw.open_map("meta");    mw.put_string("fw", "1.2.3").put_uint("rev", 7);   mw.close();

    s.set_uint("seq", seq)
     .set_string("name", "lidar")
     .set_string("note", NOTE)
     .set_array("samples", rant::Bytes(SAMPLES, sizeof SAMPLES))
     .set_map("extras", mw);
    if (!s.ok() || !mw.ok()) { std::printf("FAIL: encode (ok=%d mw=%d)\n", s.ok(), mw.ok()); return false; }
    return pub.send(s) == rant::SendStatus::Ok;
}

/* decoded snapshot filled by the subscriber's handler */
struct Got {
    bool received = false;
    std::string name, note, state, meta_fw;
    uint32_t seq = 0;
    uint64_t battery = 0, meta_rev = 0;
    int64_t sgn = 0;
    bool ok_flag = false;
    size_t n_samples = 0, n_temps = 0;
    float  samples[3] = {0,0,0};
    double temp0 = 0;
    /* the same map decoded via the owning std::map readback (MapReader::to_map) */
    uint64_t    tm_battery = 0, tm_meta_rev = 0;
    std::string tm_state;
    double      tm_temp0 = 0;
} g;

static void decode(const rant::MessageView& m) {
    g.seq  = (uint32_t)m.get_uint("seq");
    g.name = std::string(m.get_string("name"));
    g.note = std::string(m.get_string("note"));
    auto sm = m.get_array("samples");
    g.n_samples = sm.size() / sizeof(float);
    if (g.n_samples > 3) g.n_samples = 3;
    std::memcpy(g.samples, sm.data(), g.n_samples * sizeof(float));
    auto map = m.get_map("extras");
    rant::MapValue v;
    if (map.get("battery", v)) g.battery = v.as_uint();
    if (map.get("signed",  v)) g.sgn     = v.as_int();
    if (map.get("ok",      v)) g.ok_flag = v.as_bool();
    if (map.get("state",   v)) g.state   = std::string(v.as_string());
    if (map.get("temps",   v)) { g.n_temps = v.array_count(); if (g.n_temps) g.temp0 = v.array_at(0).as_f64(); }
    if (map.get("meta",    v)) {
        auto meta = v.as_map();
        rant::MapValue mv;
        if (meta.get("fw",  mv)) g.meta_fw  = std::string(mv.as_string());
        if (meta.get("rev", mv)) g.meta_rev = mv.as_uint();
    }

    /* std::map readback: decode the whole map into an owning tree that outlives the handler */
    rant::MapDict d = m.get_map("extras").to_map();
    if (d.count("battery")) g.tm_battery = d.at("battery").as_uint();
    if (d.count("state"))   g.tm_state   = d.at("state").as_string();
    if (d.count("temps") && d.at("temps").is_array() && !d.at("temps").as_array().empty())
        g.tm_temp0 = d.at("temps").as_array()[0].as_f64();
    if (d.count("meta") && d.at("meta").is_map() && d.at("meta").as_map().count("rev"))
        g.tm_meta_rev = d.at("meta").as_map().at("rev").as_uint();

    g.received = true;
}

static bool verify_dynamic() {
    chk("name (capped)", g.name == "lidar");
    chk("note (vstr)",   g.note == NOTE);
    chk("samples (varr f32)", g.n_samples == 3 && g.samples[0] == 1.5f
        && g.samples[1] == -2.25f && g.samples[2] == 3.75f);
    chk("map battery",   g.battery == 87);
    chk("map signed",    g.sgn == -5);
    chk("map ok",        g.ok_flag);
    chk("map state",     g.state == "docked");
    chk("map array",     g.n_temps == 2 && g.temp0 > 36.1 && g.temp0 < 36.3);
    chk("map nested",    g.meta_fw == "1.2.3" && g.meta_rev == 7);
    chk("to_map scalar/string", g.tm_battery == 87 && g.tm_state == "docked");
    chk("to_map nested array",  g.tm_temp0 > 36.1 && g.tm_temp0 < 36.3);
    chk("to_map nested map",    g.tm_meta_rev == 7);
    return g_failures == 0;
}

/* leg 3: typed schemas for the patterns layer */

struct AddReq { int32_t x; int32_t y; };
RANT_SCHEMA(AddReq, x, y);
struct AddRsp { int64_t sum; };
RANT_SCHEMA(AddRsp, sum);
struct Speed { int32_t v; };
RANT_SCHEMA(Speed, v);

/* padding free: offsets and size equal the wire, the memcpy path */
struct Flat { uint32_t a; float b; };
RANT_SCHEMA(Flat, a, b);
/* padded by the compiler while the wire is packed, plus a capped string: the loop path */
struct Padded {
    uint8_t           a;
    uint32_t          b;
    uint16_t          c;
    double            d;
    rant::String<7> tag;
};
RANT_SCHEMA(Padded, a, b, c, d, tag);
static_assert(sizeof(Flat) == 8, "Flat must be padding-free for the memcpy-path leg");
static_assert(sizeof(Padded) > 1 + 4 + 2 + 8 + 9, "Padded must carry padding for the loop-path leg");

/* the same wire name with a different field set: the subscriber's schema is a subset of
 * the publisher's in another order, so the hashes differ and the rebase path decodes */
namespace pubside { struct Telemetry { uint32_t seq; float volts; uint16_t flags; }; }
RANT_SCHEMA(pubside::Telemetry, seq, volts, flags);
namespace subside { struct Telemetry { uint16_t flags; uint32_t seq; }; }
RANT_SCHEMA(subside::Telemetry, flags, seq);

static rant::Deferred<AddRsp> g_deferred;
static std::mutex               g_deferred_mu;
static std::atomic<bool>        g_deferred_ready{ false };

static bool patterns_leg() {
    int fails_at_entry = g_failures;
    rant::NodeOptions opts;
    opts.domain = 43;
    opts.multicast_interface = "127.0.0.1";
    opts.max_topics = 32;
    opts.fetch_details = true;   /* observer: peer entity names resolve, else hash placeholders */

    auto on_evt = [](const char* tag) {
        return [tag](const rant::Event& e) {
            if (e.is_error())
                std::printf("event(%s): %s\n", tag, e.to_string().c_str());
        };
    };

    rant::NodeOptions manual = opts;
    manual.threading = rant::Threading::Manual;   /* B is pumped from this thread */
    rant::Node a("PA", opts);
    rant::Node b("PB", manual);
    a.on_event(on_evt("PA"));
    b.on_event(on_evt("PB"));
    chk("patterns: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;

    /* A runs on its service thread, B is pumped from this thread so the blocking
     * call and wait forms, which drive the caller's loop, are exercised */
    chk("patterns: A runs its service thread", a.threading() == rant::Threading::ServiceThread);
    chk("patterns: B polls", b.threading() == rant::Threading::Manual);

    /* functions */
    auto def_add = a.function_definition<AddReq, AddRsp>("add",
        [](const AddReq& q) { return AddRsp{ (int64_t)q.x + q.y }; });
    auto def_chk = a.function_definition<AddReq, AddRsp>("chk",
        [](const AddReq& q, rant::Request<AddRsp>& rq) {
            if (q.x < 0) rq.fail();
            else         rq.reply(AddRsp{ (int64_t)q.x * q.y });
        });
    auto def_defer = a.function_definition<AddReq, AddRsp>("defr",
        [](const AddReq& q, rant::Request<AddRsp>& rq) {
            (void)q;
            std::lock_guard<std::mutex> g(g_deferred_mu);
            g_deferred = rq.defer();
            g_deferred_ready = true;
        });
    chk("patterns: definitions created", def_add.valid() && def_chk.valid() && def_defer.valid());

    auto rf_add = b.remote_function<AddReq, AddRsp>("add");
    auto rf_chk = b.remote_function<AddReq, AddRsp>("chk");
    auto rf_defer = b.remote_function<AddReq, AddRsp>("defr");
    chk("patterns: remotes created", rf_add.valid() && rf_chk.valid() && rf_defer.valid());

    chk("patterns: definition discovered", wait_for(4000,
        [&] { return rf_add.match_count() > 0 && rf_chk.match_count() > 0 && rf_defer.match_count() > 0; }, &b));
    /* settle B so the rsp lanes from A to B are formed before the first blocking
     * call, since match_count proves only the req direction */
    chk("patterns: settle", b.settle(4000));

    /* blocking round trip (memcpy-path structs both ways) */
    auto r1 = rf_add.call(AddReq{ 20, 22 }, 3000);
    chk("patterns: blocking call Ok 20+22=42",
        r1.ok() && r1.value()->sum == 42 && r1.provider() != 0);

    /* full-form handler replying */
    auto r2 = rf_chk.call(AddReq{ 6, 7 }, 3000);
    chk("patterns: full-form reply 6*7=42", r2.ok() && r2.value()->sum == 42);

    /* full form handler failing: AppError */
    auto r3 = rf_chk.call(AddReq{ -1, 0 }, 3000);
    chk("patterns: fail() -> AppError", !r3.ok() && r3.status() == rant::CallStatus::AppError);

    /* deferred completion from another thread while the caller blocks */
    std::thread completer([] {
        while (!g_deferred_ready)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::lock_guard<std::mutex> g(g_deferred_mu);
        g_deferred.complete(AddRsp{ 11 });
    });
    auto r4 = rf_defer.call(AddReq{ 5, 6 }, 4000);
    completer.join();
    chk("patterns: deferred completion delivers 11", r4.ok() && r4.value()->sum == 11);

    /* async form */
    std::atomic<int64_t> async_sum{ 0 };
    std::atomic<bool>    async_done{ false };
    auto st = rf_add.call_async(AddReq{ 1, 2 },
        [&](const rant::ResponseView<AddRsp>& rv) {
            if (rv.ok()) async_sum = rv.value()->sum;
            async_done = true;
        });
    chk("patterns: call_async accepted", st == rant::SendStatus::Ok);
    chk("patterns: async 1+2=3", wait_for(3000, [&] { return async_done.load(); }, &b)
        && async_sum == 3);

    chk("patterns: definition match_count >= 1", def_add.match_count() >= 1);

    /* variables */
    rant::VariableOptions<Speed> vo;
    vo.initial = Speed{ 7 };
    vo.allow_force = true;
    auto vd = a.variable_definition<Speed>("speed", vo);
    auto rv = b.remote_variable<Speed>("speed");
    chk("var: created", vd.valid() && rv.valid());
    chk("var: remote wait() gets a value", rv.wait(4000));
    { auto v = rv.get(); chk("var: initial 7 replicated", v && v->v == 7); }
    chk("var: remote set accepted", rv.set(Speed{ 25 }) == rant::SendStatus::Ok);
    chk("var: set converges at definition", wait_for(3000,
        [&] { auto v = vd.get(); return v && v->v == 25; }, &b));
    chk("var: force accepted", vd.force(Speed{ 99 }) == rant::SendStatus::Ok);
    chk("var: forced value replicated", wait_for(3000,
        [&] { auto v = rv.get(); return rv.forced() && v && v->v == 99; }, &b));
    chk("var: set absorbed while forced", vd.set(Speed{ 50 }) == rant::SendStatus::Ok);
    chk("var: unforce accepted", vd.unforce() == rant::SendStatus::Ok);
    chk("var: unforce restores absorbed set", wait_for(3000,
        [&] { auto v = rv.get(); return !rv.forced() && v && v->v == 50; }, &b));
    chk("var: match_count both sides", rv.match_count() > 0 && vd.match_count() >= 1);

    /* variable events: on_change replays + dedups, on_write counts every write */
    std::atomic<int> vchg{ 0 }, vwr{ 0 }; std::atomic<int64_t> vlast{ 0 };
    vd.on_change([&](const Speed& s, const rant::VariableUpdate& u) { (void)u; vchg++; vlast = s.v; });
    chk("var: on_change replays current at registration", vchg.load() == 1 && vlast.load() == 50);
    vd.on_write([&](const Speed& s) { (void)s; vwr++; });
    chk("var: on_write does not replay", vwr.load() == 0);
    chk("var: identical re-set accepted", vd.set(Speed{ 50 }) == rant::SendStatus::Ok);
    chk("var: identical re-set is a write, not a change", vchg.load() == 1 && vwr.load() == 1);
    chk("var: new set accepted", vd.set(Speed{ 51 }) == rant::SendStatus::Ok);
    chk("var: change fires inline with the new value",
        vchg.load() == 2 && vlast.load() == 51 && vwr.load() == 2);
    std::atomic<int64_t> rlast{ 0 };
    rv.on_change([&](const Speed& s) { rlast = s.v; });
    chk("var: remote change arrives", wait_for(3000, [&] { return rlast.load() == 51; }, &b));
    vd.on_change(nullptr); vd.on_write(nullptr); rv.on_change(nullptr);

    /* typed pub sub, memcpy path: a padding free struct and identical schemas */
    rant::Qos rel; rel.reliability = rant::Reliability::Reliable;
    std::atomic<bool> flat_ok{ false };
    auto pf = a.publisher<Flat>("flat", rel);
    auto sf = b.subscriber<Flat>("flat",
        [&](const Flat& f) { if (f.a == 7 && f.b == 2.5f) flat_ok = true; }, rel);
    chk("codec: flat pair created", pf.valid() && sf.valid());
    chk("codec: flat matched", wait_for(4000, [&] { return pf.match_count() > 0 && pf.ready(); }, &b));
    chk("codec: flat send", pf.send(Flat{ 7, 2.5f }) == rant::SendStatus::Ok);
    chk("codec: memcpy-path round trip", wait_for(3000, [&] { return flat_ok.load(); }, &b));

    /* typed pub sub, loop path: a padded struct plus a string, with the two argument handler
       that sees the envelope beside the value */
    std::atomic<bool> padded_got{ false };
    Padded      padded_seen{};
    std::string padded_pub, padded_topic;
    auto pp = a.publisher<Padded>("padded", rel);
    auto sp = b.subscriber<Padded>("padded", [&](const Padded& v, const rant::MessageView& m) {
        padded_seen  = v;
        padded_pub   = std::string(m.publisher_name());
        padded_topic = std::string(m.topic_name());
        padded_got   = true;
    }, rel);
    chk("codec: padded pair created", pp.valid() && sp.valid());
    chk("codec: padded matched", wait_for(4000, [&] { return pp.match_count() > 0 && pp.ready(); }, &b));
    Padded out{};
    out.a = 9; out.b = 0x11223344u; out.c = 777; out.d = -3.5;
    chk("codec: string assign", out.tag.assign("robot"));
    chk("codec: string over-cap refused", !out.tag.assign("well over seven"));
    chk("codec: padded send", pp.send(out) == rant::SendStatus::Ok);
    chk("codec: loop-path round trip", wait_for(3000, [&] { return padded_got.load(); }, &b)
        && padded_seen.a == 9 && padded_seen.b == 0x11223344u
        && padded_seen.c == 777 && padded_seen.d == -3.5
        && padded_seen.tag.view() == "robot");
    chk("codec: envelope beside the value", padded_pub == "PA" && padded_topic == "padded");

    /* typed pub sub, schema hash mismatch: a subset subscriber, rebase decode */
    std::atomic<bool> tele_ok{ false };
    auto tp = a.publisher<pubside::Telemetry>("tele", rel);
    auto ts = b.subscriber<subside::Telemetry>("tele",
        [&](const subside::Telemetry& t) { if (t.seq == 31337 && t.flags == 5) tele_ok = true; }, rel);
    chk("codec: telemetry pair created", tp.valid() && ts.valid());
    chk("codec: telemetry matched", wait_for(4000, [&] { return tp.match_count() > 0 && tp.ready(); }, &b));
    chk("codec: telemetry send", tp.send(pubside::Telemetry{ 31337, 12.6f, 5 }) == rant::SendStatus::Ok);
    chk("codec: subset/rebase round trip", wait_for(3000, [&] { return tele_ok.load(); }, &b));

    /* reflection: local and peer entities */
    auto find = [](const std::vector<rant::Entity>& es, rant::EntityKind k, const char* nm)
                -> const rant::Entity* {
        for (const auto& e : es) if (e.kind == k && e.name == nm) return &e;
        return nullptr;
    };
    auto local = a.reflection().entities();
    chk("reflect: local function folded",  find(local, rant::EntityKind::Function, "add") != nullptr);
    chk("reflect: local variable folded",  find(local, rant::EntityKind::Variable, "speed") != nullptr);
    chk("reflect: local topic passes",     find(local, rant::EntityKind::Topic, "flat") != nullptr);
    const rant::Entity* var_ent = find(local, rant::EntityKind::Variable, "speed");
    chk("reflect: variable writable", var_ent && var_ent->writable);

    bool peer_seen = false, peer_fn = false;
    for (const auto& p : b.reflection().peers()) {
        if (p.name != "PA") continue;
        peer_seen = true;
        auto es = b.reflection().entities(p.id);
        const rant::Entity* e = find(es, rant::EntityKind::Function, "add");
        peer_fn = e && e->provides;
    }
    chk("reflect: peer PA visible", peer_seen);
    chk("reflect: peer function entity provides", peer_fn);

    a.close();
    return g_failures == fails_at_entry;
}

/* leg 5: standard types. The mirrors carry a wire name, so a Transform field matches only
 * a Transform. The golden vectors are shared with the C, C# and Python bindings. */
static const uint64_t HASH_FLOAT3    = 0x04aa9469cd08b1ddULL;   /* the `Float3` schema */
static const uint64_t HASH_IMAGE     = 0x489841f99f392b85ULL;
static const uint64_t HASH_VIDEO     = 0xf677bd147b513fbcULL;   /* `VideoFrame` */
static const uint64_t HASH_EXTSTREAM = 0xaae502077016ac13ULL;   /* `ExternalVideoStream` */

struct Track {
    rant::types::Transform at;
    rant::types::Uuid        id;
    rant::types::Timestamp when;
    rant::types::Color       tag;
    rant::types::Float3      velocity;
};
RANT_SCHEMA(Track, at, id, when, tag, velocity);

static std::atomic<int> g_track_recv{0};
static double  g_track_x = 0.0;
static uint8_t g_track_id0 = 0;

static bool stdtypes_leg() {
    int fails_at_entry = g_failures;
    rant::NodeOptions opts;
    opts.domain = 46;
    opts.multicast_interface = "127.0.0.1";
    auto on_evt = [](const rant::Event& e) {
        if (e.is_error() && e.error() != rant::ErrorKind::SchemaMismatch)
            std::printf("event(std): %s\n", e.to_string().c_str());
    };
    rant::NodeOptions manual = opts;
    manual.threading = rant::Threading::Manual;   /* B is pumped by wait_for */
    rant::Node a("std-a", opts);
    rant::Node b("std-b", manual);
    a.on_event(on_evt);
    b.on_event(on_evt);
    chk("std: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;

    /* the mirrors ARE the wire, so the memcpy fast path stays available */
    chk("std: Transform is 88 bytes", sizeof(rant::types::Transform) == 88);
    chk("std: Uuid is 16 bytes", sizeof(rant::types::Uuid) == 16);
    chk("std: Color is 4 bytes", sizeof(rant::types::Color) == 4);

    {   /* a standard type as a whole schema: its name, its canonical hash */
        rant::Schema f3 = a.schema("Float3");
        chk("std: Float3 compiles by name alone", !f3.empty());
        if (f3) {
            chk("std: Float3 golden hash", f3.hash() == HASH_FLOAT3);
            chk("std: Float3 is 12 message bytes", f3.size() == 12);
        }
        rant::Schema tf    = a.schema("Transform");
        rant::Schema twist = a.schema("Twist");
        chk("std: Transform and Twist are distinct names, never mistaken for each other",
            tf && twist && !twist.can_read(tf) && !tf.can_read(twist));
        /* the name NARROWS: an anonymous field of the same shape reads a Transform field,
           never the reverse (a struct ROOT's name stays strict-equal, as before). Two
           shapes of W, so each goes on its own node */
        rant::Schema named_f = a.schema("W { at: Transform }");
        rant::Schema bare_f  = b.schema(
            "W { at: { translation: { x: f64, y: f64, z: f64 },"
            "          rotation: { x: f64, y: f64, z: f64, w: f64 }, parent: string<30> } }");
        chk("std: an anonymous field of the same shape reads a Transform field",
            named_f && bare_f && bare_f.can_read(named_f) && !named_f.can_read(bare_f));
        if (tf) {
            rant::Schema::Field f;
            int i = tf.field_index("translation");
            chk("std: a named member reports its type name",
                i >= 0 && tf.field_at((uint16_t)i, f) && f.type_name == "Double3");
        }
        rant::Schema cloud = a.schema("Cloud { pts: Float3[], at: Transform }");
        chk("std: named types nest and array", !cloud.empty());
        if (cloud) {
            rant::Schema::Field f;
            int i = cloud.field_index("pts");
            chk("std: an array element reports its type name",
                i >= 0 && cloud.field_at((uint16_t)i, f)
                       && f.elem_name == "Float3" && f.elem_size == 12);
        }
        chk("std: the same text is the same handle", a.schema("Transform").raw() == tf.raw());
    }

    {   /* the RANT_SCHEMA codec emits the NAMES, never the inlined shapes */
        auto pub = a.publisher<Track>("std/track");
        auto sc = rant::priv::schema_of<Track>(a);
        std::string txt = sc ? sc->to_dsl() : std::string();
        chk("std: the codec spells named members by name",
            txt.find("at: Transform") != std::string::npos &&
            txt.find("id: Uuid") != std::string::npos &&
            txt.find("when: Timestamp") != std::string::npos &&
            txt.find("velocity: Float3") != std::string::npos);
        chk("std: they print back as hoisted definitions, not inlined shapes",
            txt.find("Transform {") != std::string::npos &&
            txt.find("Uuid = u8[16]") != std::string::npos);
    }

    {   /* end to end through the typed codec */
        auto pub = a.publisher<Track>("std/track2");
        auto sub = b.subscriber<Track>("std/track2", [](const Track& t) {
            g_track_x = t.at.translation.x;
            g_track_id0 = t.id.bytes[0];
            g_track_recv++;
        });
        chk("std: publisher and subscriber match",
            wait_for(4000, [&] { return pub.match_count() == 1; }, &b));
        Track t{};
        t.at.translation = { 4.5, -1.25, 9.0 };
        t.at.rotation = rant::types::identity_rotation();
        t.id.bytes[0] = 0xAB;
        t.when = rant::types::now();
        t.tag = rant::types::color_from_hex(0x112233FFu);
        t.velocity = { 1.0f, 2.0f, 3.0f };
        pub.send(t);
        chk("std: a Track crosses whole",
            wait_for(4000, [&] { return g_track_recv.load() > 0; }, &b)
            && g_track_x == 4.5 && g_track_id0 == 0xAB);
    }

    {   /* the video family: mirrors with a variable member ride the codec's tail path */
        auto im = rant::priv::schema_of<rant::types::Image>(a);
        auto vf = rant::priv::schema_of<rant::types::VideoFrame>(a);
        auto xs = rant::priv::schema_of<rant::types::ExternalVideoStream>(a);
        rant::Schema imc = a.schema("Image");
        rant::Schema vfc = a.schema("VideoFrame");
        rant::Schema xsc = a.schema("ExternalVideoStream");
        chk("std: video codecs compile", im && vf && xs && imc && vfc && xsc);
        chk("std: Image codec == canonical", im && imc && im->hash() == imc.hash());
        chk("std: VideoFrame codec == canonical", vf && vfc && vf->hash() == vfc.hash());
        chk("std: ExternalVideoStream codec == canonical", xs && xsc && xs->hash() == xsc.hash());
        chk("std: video golden hashes (shared with every binding)",
            imc && imc.hash() == HASH_IMAGE && vfc && vfc.hash() == HASH_VIDEO
                && xsc && xsc.hash() == HASH_EXTSTREAM);

        /* an Image end to end: the variable payload crosses beside the fixed fields */
        std::atomic<int> img_recv{ 0 };
        rant::types::Image img_got;
        auto ipub = a.publisher<rant::types::Image>("std/frame");
        auto isub = b.subscriber<rant::types::Image>("std/frame", [&](const rant::types::Image& i) {
            img_got = i;
            img_recv++;
        });
        chk("std: image pair matched",
            wait_for(4000, [&] { return ipub.match_count() == 1 && ipub.ready(); }, &b));
        rant::types::Image img;
        img.width = 320; img.height = 4; img.stride = 320;
        img.format = rant::types::ImageFormat::Mono8;
        img.data.resize((size_t)img.stride * img.height);
        for (size_t i = 0; i < img.data.size(); i++) img.data[i] = (uint8_t)(i * 7);
        chk("std: image send", ipub.send(img) == rant::SendStatus::Ok);
        chk("std: image crosses whole",
            wait_for(4000, [&] { return img_recv.load() > 0; }, &b)
            && img_got.width == 320 && img_got.format == rant::types::ImageFormat::Mono8
            && img_got.data.size() == img.data.size()
            && img_got.data == img.data);

        /* ExternalVideoStream is fully fixed: the latched-variable use it exists for */
        rant::VariableOptions<rant::types::ExternalVideoStream> vo;
        rant::types::ExternalVideoStream st;
        st.kind = rant::types::VideoStreamKind::Rtsp;
        st.codec = rant::types::VideoCodec::H264;
        st.width = 1920; st.height = 1080;
        st.url.value.assign("rtsp://cam.local/main");
        st.name.assign("front door");
        vo.initial = st;
        auto vdef = a.variable_definition<rant::types::ExternalVideoStream>("std/stream", vo);
        auto vrem = b.remote_variable<rant::types::ExternalVideoStream>("std/stream");
        chk("std: stream variable replicates", wait_for(4000, [&] {
                auto v = vrem.get();
                return v && v->kind == rant::types::VideoStreamKind::Rtsp
                         && v->codec == rant::types::VideoCodec::H264
                         && v->width == 1920 && v->height == 1080
                         && v->url.value == "rtsp://cam.local/main"
                         && v->name == "front door";
            }, &b));
    }

    {   /* the thin operations */
        rant::types::Color c = rant::types::color_from_hex(0x11223344u);
        chk("std: Color hex round-trips", c.r == 0x11 && c.a == 0x44
                                          && rant::types::color_to_hex(c) == 0x11223344u);
        chk("std: vector length", rant::types::length(rant::types::Double3{ 3.0, 4.0, 0.0 }) == 5.0);
        rant::types::Double3 r = rant::types::rotate(rant::types::Quaternion{ 0.0, 0.0, 1.0, 0.0 },
                                       rant::types::Double3{ 1.0, 0.0, 0.0 });
        chk("std: quaternion rotate", r.x < -0.999 && r.x > -1.001);
        rant::types::Uuid u1 = rant::types::new_uuid(), u2 = rant::types::new_uuid();
        chk("std: new_uuid is random and version 4",
            !rant::types::is_nil(u1) && std::memcmp(u1.bytes, u2.bytes, 16) != 0
            && (u1.bytes[6] & 0xF0u) == 0x40u);
        chk("std: now() is Unix-epoch microseconds", rant::types::now().us > 1600000000000000LL);
    }
    a.close();
    return g_failures == fails_at_entry;
}

/* leg 4: bare type roots with no RANT_SCHEMA, the type is the schema */
/* The canonical wire of a bare type is its kind alone, so these hashes are the same in
 * every language binding (pinned in C by rant_test's schema-root phase). */
static const uint64_t HASH_BOOL   = 0xee90234f61d2520bULL;
static const uint64_t HASH_F32ARR = 0x314844e3386a1fc4ULL;

/* nested structs spell by name, as C# and Python do */
struct Corner { float x, y; };
RANT_SCHEMA(Corner, x, y);
struct Shape { Corner origin; rant::types::Float2 at; uint8_t id; };
RANT_SCHEMA(Shape, origin, at, id);
/* fixed struct arrays, one nested in another's element */
struct Outline { rant::types::Float2 corners[4]; uint8_t n; };
RANT_SCHEMA(Outline, corners, n);
struct Grid { std::array<Outline, 2> rows; Corner mark[3]; };
RANT_SCHEMA(Grid, rows, mark);
/* variable arrays of structs and of capped strings, beside other tail members */
struct Scene {
    uint32_t seq = 0;
    std::vector<Outline> outlines;
    std::vector<rant::String<8>> tags;
    std::string note;
};
RANT_SCHEMA(Scene, seq, outlines, tags, note);

/* encode then decode through the node's typed codec, no network */
template <class T> static bool round_trip(rant::Node& n, const T& in, T& out) {
    rant::priv::TypeCodec* c = rant::priv::type_codec<T>(n);
    if (!rant::priv::codec_ok(c)) return false;
    std::vector<uint8_t> scratch;
    rant::Bytes by = rant::priv::encode(*c, in, scratch);
    if (by.size() == 0) return false;
    std::vector<uint8_t> copy(by.data(), by.data() + by.size());
    return rant::priv::decode(*c, out, rant::Bytes(copy.data(), copy.size()), nullptr);
}

static bool nested_leg() {
    int fails_at_entry = g_failures;
    rant::NodeOptions opts;
    opts.domain = 47;
    opts.multicast_interface = "127.0.0.1";
    opts.threading = rant::Threading::Manual;
    rant::Node a("nest-a", opts);
    chk("nest: node constructed", a.valid());
    if (!a.valid()) return false;

    {   auto sc = rant::priv::schema_of<Shape>(a);
        std::string txt = sc ? sc->to_dsl() : std::string();
        chk("nest: a nested struct spells by name, defined once above",
            txt.find("origin: Corner") != std::string::npos
            && txt.find("Corner {") != std::string::npos && txt.find("at: Float2") != std::string::npos);
        rant::Schema same = a.schema("Corner { x: f32, y: f32 }\n"
                                     "Shape { origin: Corner, at: Float2, id: u8 }");
        chk("nest: the same text from C# or Python is the same schema",
            sc && same && sc->hash() == same.hash());
        Shape in{}, out{};
        in.origin = { 1.5f, -2.0f }; in.at = { 3.0f, 4.0f }; in.id = 7;
        chk("nest: a nested struct round trips",
            round_trip(a, in, out) && out.origin.x == 1.5f && out.origin.y == -2.0f
            && out.at.y == 4.0f && out.id == 7);
    }
    {   auto sc = rant::priv::schema_of<Grid>(a);
        std::string txt = sc ? sc->to_dsl() : std::string();
        chk("nest: fixed struct arrays spell by element name",
            txt.find("corners: Float2[4]") != std::string::npos
            && txt.find("rows: Outline[2]") != std::string::npos
            && txt.find("mark: Corner[3]") != std::string::npos);
        rant::Schema same = a.schema("Outline { corners: Float2[4], n: u8 }\n"
                                     "Grid { rows: Outline[2], mark: Corner[3] }");
        chk("nest: the same text is the same schema", sc && same && sc->hash() == same.hash());
        Grid in{}, out{};
        in.rows[1].corners[3] = { 5.0f, 6.0f };
        in.rows[0].corners[0] = { 1.0f, 2.0f };
        in.rows[1].n = 9;
        in.mark[2] = { -1.0f, -2.0f };
        chk("nest: a struct array inside a struct array's element round trips",
            round_trip(a, in, out) && out.rows[1].corners[3].y == 6.0f
            && out.rows[0].corners[0].x == 1.0f && out.rows[1].corners[0].x == 0.0f
            && out.rows[1].n == 9 && out.mark[2].y == -2.0f);
        std::vector<uint8_t> scratch;
        rant::priv::TypeCodec* gc = rant::priv::type_codec<Grid>(a);
        rant::Bytes by = gc ? rant::priv::encode(*gc, in, scratch) : rant::Bytes();
        {
            rant::detail::RantBytes mb; mb.data = by.data(); mb.len = by.size();
            chk("nest: the C path reads what the C++ codec wrote",
                gc && rant::detail::rant_get_f32(mb, gc->raw, "rows[1].corners[3].y") == 6.0f
                && rant::detail::rant_get_f32(mb, gc->raw, "mark[2].x") == -1.0f);
        }
    }
    {   auto sc = rant::priv::schema_of<std::array<Corner, 3>>(a);
        std::string txt = sc ? sc->to_dsl() : std::string();
        chk("nest: a fixed struct array is a whole schema too",
            txt.find("Corner[3]") != std::string::npos);
        std::array<Corner, 3> in{}, out{};
        in[2] = { 7.0f, 8.0f };
        chk("nest: a fixed struct array root round trips",
            round_trip(a, in, out) && out[2].x == 7.0f && out[0].y == 0.0f);
    }
    {   auto sc = rant::priv::schema_of<Scene>(a);
        std::string txt = sc ? sc->to_dsl() : std::string();
        chk("nest: variable struct and capped string arrays spell as C# and Python do",
            txt.find("outlines: Outline[]") != std::string::npos
            && txt.find("tags: string<8>[]") != std::string::npos);
        rant::Schema same = a.schema("Scene { seq: u32, outlines: Outline[], tags: string<8>[], "
                                     "note: string }");
        chk("nest: the same text is the same schema", sc && same && sc->hash() == same.hash());
        Scene in, out;
        in.seq = 3;
        in.outlines.resize(2);
        in.outlines[1].corners[2] = { 1.5f, 2.5f };
        in.outlines[0].n = 4;
        in.tags.resize(3);
        in.tags[0].assign("left");
        in.tags[2].assign("eightchr");
        in.note = "after the arrays";
        chk("nest: a variable struct array round trips",
            round_trip(a, in, out) && out.seq == 3 && out.outlines.size() == 2
            && out.outlines[1].corners[2].y == 2.5f && out.outlines[0].n == 4
            && out.outlines[1].corners[0].x == 0.0f);
        chk("nest: a variable capped string array round trips",
            out.tags.size() == 3 && out.tags[0] == "left" && out.tags[1] == ""
            && out.tags[2] == "eightchr" && out.note == "after the arrays");
        std::vector<uint8_t> scratch;
        rant::priv::TypeCodec* c = rant::priv::type_codec<Scene>(a);
        rant::Bytes by = c ? rant::priv::encode(*c, in, scratch) : rant::Bytes();
        rant::detail::RantBytes mb; mb.data = by.data(); mb.len = by.size();
        chk("nest: the C path reads the variable array's elements",
            c && rant::detail::rant_get_f32(mb, c->raw, "outlines[1].corners[2].x") == 1.5f
            && rant::detail::rant_get_array_count(mb, c->raw, "outlines") == 2
            && rant::detail::rant_schema_validate(c->raw, mb));
        Scene none, back;
        back.outlines.resize(5);
        chk("nest: an empty variable struct array decodes empty",
            round_trip(a, none, back) && back.outlines.empty() && back.tags.empty());

        /* a publisher's other layout: an extra field shifts every offset */
        rant::NodeOptions o2 = opts;
        rant::Node w("nest-w", o2);
        rant::Schema wide = w.schema("Outline { corners: Float2[4], n: u8 }\n"
                                     "Scene { extra: u64, seq: u32, outlines: Outline[], "
                                     "tags: string<8>[], note: string }");
        rant::MessageBuilder mbld(wide);
        mbld.set_uint("extra", 99).set_uint("seq", 5)
            .set_array_count("outlines", 2)
            .set_f32("outlines[0].corners[3].y", 8.0f)
            .set_uint("outlines[1].n", 6);
        chk("nest: MessageBuilder sizes a variable struct array and fills it by path", mbld.ok());
        rant::Bytes raw = mbld.bytes();
        std::vector<uint8_t> msg(raw.data(), raw.data() + raw.size());
        rant::detail::RantBytes wb; wb.data = msg.data(); wb.len = msg.size();
        chk("nest: the builder's message reads back by path and count",
            rant::detail::rant_get_array_count(wb, wide.raw(), "outlines") == 2
            && rant::detail::rant_get_f32(wb, wide.raw(), "outlines[0].corners[3].y") == 8.0f);
        Scene rb;
        chk("nest: another layout decodes through its own offsets",
            c && rant::priv::decode(*c, rb, rant::Bytes(msg.data(), msg.size()), wide.raw())
            && rb.seq == 5 && rb.outlines.size() == 2 && rb.outlines[0].corners[3].y == 8.0f
            && rb.outlines[1].n == 6);
    }
    {   auto sc = rant::priv::schema_of<std::vector<Corner>>(a);
        std::string txt = sc ? sc->to_dsl() : std::string();
        chk("nest: a variable struct array is a whole schema too",
            txt.find("Corner[]") != std::string::npos);
        std::vector<Corner> in = { { 1.0f, 2.0f }, { 3.0f, 4.0f } }, out;
        chk("nest: a variable struct array root round trips",
            round_trip(a, in, out) && out.size() == 2 && out[1].x == 3.0f);
    }
    {   rant::types::JointNames in, out;
        in.name.resize(2);
        in.name[0].assign("shoulder");
        in.name[1].assign("elbow");
        chk("nest: the standard JointNames compiles and round trips",
            round_trip(a, in, out) && out.name.size() == 2 && out.name[1] == "elbow");
    }
    return g_failures == fails_at_entry;
}

static bool value_root_leg() {
    int fails_at_entry = g_failures;
    rant::NodeOptions opts;
    opts.domain = 44;
    opts.multicast_interface = "127.0.0.1";
    auto on_evt = [](const char* tag) {
        return [tag](const rant::Event& e) {
            if (e.is_error()) std::printf("event(%s): %s\n", tag, e.to_string().c_str());
        };
    };
    rant::NodeOptions manual = opts;
    manual.threading = rant::Threading::Manual;   /* B is pumped from this thread */
    rant::Node a("VA", opts);
    rant::Node b("VB", manual);
    a.on_event(on_evt("VA"));
    b.on_event(on_evt("VB"));
    chk("root: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;

    /* the canonical-hash pin: the typed codec and the DSL agree, and both agree with C */
    auto sb = rant::priv::schema_of<bool>(a);
    rant::Schema arr = a.schema("f32[]");
    chk("root: bool codec compiles", sb.has_value());
    chk("root: `bool` canonical hash", sb && sb->hash() == HASH_BOOL);
    chk("root: `f32[]` canonical hash", arr && arr.hash() == HASH_F32ARR);
    chk("root: bare root is one anonymous field",
        sb && sb->field_count() == 1 && sb->name().empty());

    rant::Qos rel; rel.reliability = rant::Reliability::Reliable;

    /* bool: the whole payload is one byte */
    std::atomic<int> flags{ 0 };
    auto pb = a.publisher<bool>("flag", rel);
    auto sub_b = b.subscriber<bool>("flag", [&](const bool& v) { if (v) flags++; }, rel);
    chk("root: bool pair created", pb.valid() && sub_b.valid());
    chk("root: bool matched", wait_for(4000, [&] { return pb.match_count() > 0 && pb.ready(); }, &b));
    chk("root: bool send", pb.send(true) == rant::SendStatus::Ok);
    chk("root: bool round trip", wait_for(3000, [&] { return flags.load() == 1; }, &b));

    /* std::string: the unbounded `string` root, one tail frame */
    std::string got;
    auto ps = a.publisher<std::string>("note", rel);
    auto sub_s = b.subscriber<std::string>("note", [&](const std::string& v) { got = v; }, rel);
    chk("root: string pair created", ps.valid() && sub_s.valid());
    chk("root: string matched", wait_for(4000, [&] { return ps.match_count() > 0 && ps.ready(); }, &b));
    chk("root: string send", ps.send(std::string("a bare unbounded string")) == rant::SendStatus::Ok);
    chk("root: string round trip", wait_for(3000, [&] { return got == "a bare unbounded string"; }, &b));

    /* std::array: a fixed array root */
    std::atomic<bool> a3_got{ false };
    std::array<float, 3> a3_seen{};
    auto pa3 = a.publisher<std::array<float, 3>>("xyz", rel);
    auto sa3 = b.subscriber<std::array<float, 3>>("xyz",
        [&](const std::array<float, 3>& v) { a3_seen = v; a3_got = true; }, rel);
    chk("root: array pair created", pa3.valid() && sa3.valid());
    chk("root: array matched", wait_for(4000, [&] { return pa3.match_count() > 0 && pa3.ready(); }, &b));
    chk("root: array send", pa3.send(std::array<float, 3>{ 1.5f, 2.5f, -3.f }) == rant::SendStatus::Ok);
    chk("root: array round trip", wait_for(3000, [&] { return a3_got.load(); }, &b)
        && a3_seen[0] == 1.5f && a3_seen[2] == -3.f);

    /* a variable whose type is a bare double */
    rant::VariableOptions<double> vo;
    vo.initial = 1.25;
    auto vd = a.variable_definition<double>("gain", vo);
    auto rv = b.remote_variable<double>("gain");
    chk("root: variable pair created", vd.valid() && rv.valid());
    chk("root: variable replicates the initial", wait_for(4000,
        [&] { auto v = rv.get(); return v && *v == 1.25; }, &b));
    chk("root: variable set accepted", rv.set(2.5) == rant::SendStatus::Ok);
    chk("root: variable set converges", wait_for(3000,
        [&] { auto v = vd.get(); return v && *v == 2.5; }, &b));

    /* the dynamic API over a bare root: MessageBuilder/FieldView through the "" path */
    rant::Schema f64 = a.schema("f64");
    chk("root: dynamic f64 schema", f64 && f64.field_count() == 1);
    if (f64) {
        std::atomic<int> hits{ 0 };
        double seen = 0;
        auto dp = a.publisher<rant::Bytes>("dyn", rel, f64);
        auto ds = b.subscriber<rant::Bytes>("dyn",
            [&](const rant::MessageView& m) { seen = m.get_f64(""); hits++; }, rel, f64);
        chk("root: dynamic pair created", dp.valid() && ds.valid());
        chk("root: dynamic matched", wait_for(4000, [&] { return dp.match_count() > 0 && dp.ready(); }, &b));
        rant::MessageBuilder mb(f64);
        mb.set_f64("", -7.5);
        chk("root: builder set through the empty path", mb.ok());
        chk("root: dynamic send", dp.send(mb) == rant::SendStatus::Ok);
        chk("root: dynamic round trip", wait_for(3000, [&] { return hits.load() == 1; }, &b));
        chk("root: dynamic value read through the empty path", seen == -7.5);
    }
    a.close();
    return g_failures == fails_at_entry;
}

/* leg 6: variable members, std::vector<E> and std::string, as tail frames */
/* A RANT_SCHEMA struct may carry variable members: each rides the message tail as a
 * length framed section read by path, while the fixed fields keep the static table. */

/* the same wire name, the publisher a superset in another order with an extra variable
 * field ahead of the shared ones, so the rebased decode must find frames by path */
namespace narrow { struct Chunk {
    uint32_t           seq = 0;
    std::vector<float> samples;
    std::string        note;
}; }
RANT_SCHEMA(narrow::Chunk, seq, samples, note);

namespace wide { struct Chunk {
    uint64_t           stamp = 0;
    std::string        debug;
    std::vector<float> samples;
    uint32_t           seq = 0;
    std::string        note;
    float              gain = 0.f;
}; }
RANT_SCHEMA(wide::Chunk, stamp, debug, samples, seq, note, gain);

static bool tails_leg() {
    int fails_at_entry = g_failures;

    rant::NodeOptions opts;
    opts.domain = 47;
    opts.multicast_interface = "127.0.0.1";
    auto on_evt = [](const char* tag) {
        return [tag](const rant::Event& e) {
            if (e.is_error() && e.error() != rant::ErrorKind::SchemaMismatch)
                std::printf("event(%s): %s\n", tag, e.to_string().c_str());
        };
    };
    rant::NodeOptions manual = opts;
    manual.threading = rant::Threading::Manual;   /* B is pumped from this thread */
    rant::Node a("TA", opts);
    rant::Node b("TB", manual);
    a.on_event(on_evt("TA"));
    b.on_event(on_evt("TB"));
    chk("tails: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;
    rant::Qos rel; rel.reliability = rant::Reliability::Reliable;

    {   /* codec-level: spelling, roundtrip, and the bare-vector root pin */
        auto sc = rant::priv::schema_of<narrow::Chunk>(b);
        rant::priv::TypeCodec* cc = rant::priv::type_codec<narrow::Chunk>(b);
        chk("tails: Chunk codec compiles", sc.has_value());
        std::string txt = sc ? sc->to_dsl() : std::string();
        chk("tails: variable members spell as f32[] / string",
            txt.find("samples: f32[]") != std::string::npos &&
            txt.find("note: string") != std::string::npos);

        narrow::Chunk in;
        in.seq = 7;
        in.samples = { 1.5f, -2.5f, 8.75f };
        in.note = "a note well past any small-string buffer, to make the heap real";
        std::vector<uint8_t> scratch;
        rant::Bytes wire = rant::priv::encode(*cc, in, scratch);
        chk("tails: encode produces a message", wire.size() > 0);
        narrow::Chunk out;
        chk("tails: decode roundtrips", rant::priv::decode(*cc, out, wire, nullptr)
            && out.seq == 7 && out.samples == in.samples && out.note == in.note);

        narrow::Chunk empty_in, empty_out;
        empty_out.samples = { 9.f };            /* stale state must be overwritten */
        empty_out.note = "stale";
        rant::Bytes ewire = rant::priv::encode(*cc, empty_in, scratch);
        chk("tails: empty vector and string roundtrip", ewire.size() > 0
            && rant::priv::decode(*cc, empty_out, ewire, nullptr)
            && empty_out.samples.empty() && empty_out.note.empty());

        auto vr = rant::priv::schema_of<std::vector<float>>(a);
        chk("tails: bare std::vector<float> root is canonical `f32[]`",
            vr && vr->hash() == HASH_F32ARR);
    }

    {   /* subset/rebase: wide publisher, narrow subscriber, frames found by path */
        std::atomic<int> got{ 0 };
        narrow::Chunk seen;
        auto pub = a.publisher<wide::Chunk>("tails/chunk", rel);
        auto sub = b.subscriber<narrow::Chunk>("tails/chunk", [&](const narrow::Chunk& c) {
            seen = c;
            got++;
        }, rel);
        chk("tails: subset pair matched",
            wait_for(4000, [&] { return pub.match_count() == 1 && pub.ready(); }, &b));
        wide::Chunk w;
        w.stamp = 0x1122334455667788ULL;
        w.debug = "an extra variable field the subscriber never declared";
        w.samples = { 4.f, 5.f, 6.f, 7.f };
        w.seq = 41;
        w.note = "shared tail";
        w.gain = 2.5f;
        chk("tails: wide send", pub.send(w) == rant::SendStatus::Ok);
        chk("tails: rebased decode reads the right frames",
            wait_for(4000, [&] { return got.load() > 0; }, &b)
            && seen.seq == 41 && seen.samples == w.samples && seen.note == "shared tail");
    }

    {   /* a bare vector root end to end */
        std::atomic<int> got{ 0 };
        std::vector<float> seen;
        auto pv = a.publisher<std::vector<float>>("tails/wave", rel);
        auto sv = b.subscriber<std::vector<float>>("tails/wave",
            [&](const std::vector<float>& v) { seen = v; got++; }, rel);
        chk("tails: vector-root pair matched",
            wait_for(4000, [&] { return pv.match_count() == 1 && pv.ready(); }, &b));
        std::vector<float> wave(256);
        for (size_t i = 0; i < wave.size(); i++) wave[i] = (float)i * 0.5f;
        chk("tails: vector-root send", pv.send(wave) == rant::SendStatus::Ok);
        chk("tails: vector-root round trip",
            wait_for(4000, [&] { return got.load() > 0; }, &b) && seen == wave);
    }

    a.close();
    return g_failures == fails_at_entry;
}

/* leg 7: tasks, a function with progress and cancellation */

struct MoveReq { double target; };
RANT_SCHEMA(MoveReq, target);
struct MoveProgress { double remaining; };
RANT_SCHEMA(MoveProgress, remaining);
struct MoveRsp { double final_position; };
RANT_SCHEMA(MoveRsp, final_position);

static rant::PendingTask<MoveProgress, MoveRsp> g_move_pending;
static std::mutex        g_move_mu;
static std::atomic<int>  g_move_parked{ 0 };
static rant::PendingTask<MoveProgress, MoveRsp> g_fixed_pending;
static std::mutex        g_fixed_mu;
static std::atomic<int>  g_fixed_parked{ 0 };

static bool tasks_leg() {
    int fails_at_entry = g_failures;
    rant::NodeOptions opts;
    opts.domain = 48;
    opts.multicast_interface = "127.0.0.1";
    opts.max_topics = 32;
    opts.fetch_details = true;   /* the peer entity view resolves names, schemas, attrs */
    auto on_evt = [](const char* tag) {
        return [tag](const rant::Event& e) {
            if (e.is_error()) std::printf("event(%s): %s\n", tag, e.to_string().c_str());
        };
    };
    rant::NodeOptions manual = opts;
    manual.threading = rant::Threading::Manual;   /* B is pumped from this thread */
    rant::Node a("KA", opts);
    rant::Node b("KB", manual);
    a.on_event(on_evt("KA"));
    b.on_event(on_evt("KB"));
    chk("task: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;

    /* the "move" handler parks every call for a thread the TEST owns */
    auto def_move = a.task_definition<MoveReq, MoveProgress, MoveRsp>("move",
        [](const MoveReq& q, rant::TaskRequest<MoveProgress, MoveRsp>& rq) {
            (void)q;
            std::lock_guard<std::mutex> g(g_move_mu);
            g_move_pending = rq.defer();      /* implies RUNNING */
            g_move_parked++;
        });
    rant::TaskOptions fixed_opts;
    fixed_opts.no_cancel = true;
    auto def_fixed = a.task_definition<MoveReq, MoveProgress, MoveRsp>("fixed",
        [](const MoveReq& q, rant::TaskRequest<MoveProgress, MoveRsp>& rq) {
            (void)q;
            std::lock_guard<std::mutex> g(g_fixed_mu);
            g_fixed_pending = rq.defer();
            g_fixed_parked++;
        }, fixed_opts);
    chk("task: definitions created", def_move.valid() && def_fixed.valid());

    std::atomic<uint64_t> cancel_token{ 0 };
    def_move.on_cancel([&](uint64_t token) { cancel_token = token; });

    auto rt_move = b.remote_task<MoveReq, MoveProgress, MoveRsp>("move");
    auto rt_fixed = b.remote_task<MoveReq, MoveProgress, MoveRsp>("fixed");
    chk("task: remotes created", rt_move.valid() && rt_fixed.valid());
    chk("task: definition discovered", wait_for(4000,
        [&] { return rt_move.match_count() > 0 && rt_fixed.match_count() > 0; }, &b));
    chk("task: settle", b.settle(4000));

    /* blocking call with progress: the handler defers to a test thread that streams typed
       progress and completes. on_progress sees the RUNNING ack first, then the values */
    std::atomic<int> worker_bad{ 0 };
    std::atomic<int> stale_rc{ -1 };
    std::thread worker([&] {
        while (!g_move_parked.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        rant::PendingTask<MoveProgress, MoveRsp> pending;
        { std::lock_guard<std::mutex> g(g_move_mu); pending = std::move(g_move_pending); }
        if (!pending.valid()) worker_bad++;
        if (pending.cancelled()) worker_bad++;       /* nobody cancelled this call */
        for (int i = 3; i >= 1; i--)
            if (pending.progress(MoveProgress{ (double)i }) != rant::SendStatus::Ok) worker_bad++;
        if (pending.complete(MoveRsp{ 5.0 }, "arrived") != rant::SendStatus::Ok) worker_bad++;
        stale_rc = (int)pending.complete(MoveRsp{ 0.0 });   /* the handle emptied: refused */
    });
    std::vector<std::pair<bool, double>> updates;   /* (has_value, value) in arrival order */
    auto r1 = rt_move.call(MoveReq{ 5.0 },
        [&](const rant::ProgressView<MoveProgress>& p) {
            updates.emplace_back(p.has_value(), p.has_value() ? p.value().remaining : 0.0);
        }, 8000);
    worker.join();
    chk("task: blocking call ends Ok", r1.ok() && r1.value()->final_position == 5.0 && r1.provider() != 0);
    chk("task: completion message carried", r1.message() == "arrived");
    chk("task: RUNNING ack first (no value)", updates.size() >= 1 && !updates[0].first);
    chk("task: typed progress in order", updates.size() == 4
        && updates[1].first && updates[1].second == 3.0
        && updates[2].first && updates[2].second == 2.0
        && updates[3].first && updates[3].second == 1.0);
    chk("task: every token verb accepted", worker_bad.load() == 0);
    chk("task: an answered handle refuses a second complete",
        stale_rc.load() == (int)rant::SendStatus::State);

    /* reflection: the folded task entity carries the attrs and the progress schema */
    auto find = [](const std::vector<rant::Entity>& es, rant::EntityKind k, const char* nm)
                -> const rant::Entity* {
        for (const auto& e : es) if (e.kind == k && e.name == nm) return &e;
        return nullptr;
    };
    chk("reflect: local task folded",
        find(a.reflection().entities(), rant::EntityKind::Task, "move") != nullptr);
    bool peer_task = false, attrs_ok = false, no_cancel_ok = false, prg_schema_ok = false;
    for (const auto& p : b.reflection().peers()) {
        if (p.name != "KA") continue;
        auto es = b.reflection().entities(p.id);
        const rant::Entity* mv = find(es, rant::EntityKind::Task, "move");
        const rant::Entity* fx = find(es, rant::EntityKind::Task, "fixed");
        peer_task     = mv && fx && mv->provides;
        attrs_ok      = mv && mv->cancellable && !mv->exclusive;
        no_cancel_ok  = fx && !fx->cancellable;
        prg_schema_ok = mv && mv->progress_schema.hash() != 0
                        && !mv->progress_schema.empty()
                        && mv->progress_schema.name() == "MoveProgress";
    }
    chk("reflect: peer task entities fold", peer_task);
    chk("reflect: cancellable attr surfaced", attrs_ok);
    chk("reflect: no_cancel clears cancellable", no_cancel_ok);
    chk("reflect: progress schema surfaced", prg_schema_ok);

    /* cancel honored: async call, remote cancels, definition polls + answers Cancelled */
    std::atomic<bool> cancel_done{ false };
    std::atomic<int>  cancel_status{ -1 };
    std::string       cancel_message;   /* written on the polling thread == this one */
    auto tc = rt_move.call_async(MoveReq{ 9.0 }, {},
        [&](const rant::ResponseView<MoveRsp>& rv) {
            cancel_status = (int)rv.status();
            cancel_message = std::string(rv.message());
            cancel_done = true;
        });
    chk("task: call_async returns the id", tc.ok() && tc.id != 0);
    chk("task: definition parked", wait_for(4000, [&] { return g_move_parked.load() >= 2; }, &b));
    chk("task: cancel accepted", rt_move.cancel(tc.id) == rant::SendStatus::Ok);
    chk("task: definition sees cancelled()", wait_for(4000, [&] {
            std::lock_guard<std::mutex> g(g_move_mu);
            return g_move_pending.cancelled();
        }, &b));
    chk("task: on_cancel slot fired", wait_for(2000, [&] { return cancel_token.load() != 0; }, &b));
    {
        std::lock_guard<std::mutex> g(g_move_mu);
        chk("task: complete_cancelled accepted",
            g_move_pending.complete_cancelled("stopped") == rant::SendStatus::Ok);
    }
    chk("task: caller sees Cancelled", wait_for(4000, [&] { return cancel_done.load(); }, &b)
        && cancel_status.load() == (int)rant::CallStatus::Cancelled && cancel_message == "stopped");

    /* no_cancel: cancel refused locally, the call still completes normally */
    std::atomic<bool> fixed_done{ false };
    std::atomic<int>  fixed_status{ -1 };
    auto tf = rt_fixed.call_async(MoveReq{ 1.0 }, {},
        [&](const rant::ResponseView<MoveRsp>& rv) { fixed_status = (int)rv.status(); fixed_done = true; });
    chk("task: fixed call committed", tf.ok() && tf.id != 0);
    chk("task: fixed parked", wait_for(4000, [&] { return g_fixed_parked.load() >= 1; }, &b));
    chk("task: no_cancel refused locally (BadRole)",
        rt_fixed.cancel(tf.id) == rant::SendStatus::BadRole);
    {
        std::lock_guard<std::mutex> g(g_fixed_mu);
        chk("task: fixed completes Ok anyway",
            g_fixed_pending.complete(MoveRsp{ 1.0 }) == rant::SendStatus::Ok);
    }
    chk("task: fixed caller sees Ok", wait_for(4000, [&] { return fixed_done.load(); }, &b)
        && fixed_status.load() == (int)rant::CallStatus::Ok);

    /* retire mid-run: the deferred call answers Cancelled while the channels are up */
    std::atomic<bool> retired_done{ false };
    std::atomic<int>  retired_status{ -1 };
    auto tr = rt_move.call_async(MoveReq{ 2.0 }, {},
        [&](const rant::ResponseView<MoveRsp>& rv) { retired_status = (int)rv.status(); retired_done = true; });
    chk("task: retire-leg call committed", tr.ok());
    chk("task: retire-leg parked", wait_for(4000, [&] { return g_move_parked.load() >= 3; }, &b));
    {   /* the handle dies with the definition: DROP it before retiring */
        std::lock_guard<std::mutex> g(g_move_mu);
        g_move_pending = rant::PendingTask<MoveProgress, MoveRsp>();
    }
    chk("task: retire mid-run", def_move.close() == rant::SendStatus::Ok);
    chk("task: retire resolves the caller Cancelled",
        wait_for(4000, [&] { return retired_done.load(); }, &b)
        && retired_status.load() == (int)rant::CallStatus::Cancelled);

    a.close();
    return g_failures == fails_at_entry;
}

/* leg 8: what each codec path costs, run as `rant_cpp_test bench`. The codec alone first
 * with no node, then one message at a time end to end on loopback with both nodes polled
 * from this thread, and a C node pair on the same footing as the reference. */

static std::atomic<int> g_bench_recv{ 0 };
static void bench_c_message(const rant::detail::RantMsg*) { g_bench_recv++; }

template <class F> static double ns_per(int n, F&& f) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) f();
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
}

/* send, then pump until the message lands, n times. Microseconds per message, negative
 * when a send was refused (-1) or a message never arrived (-2). */
template <class Send, class Pump> static double us_per_round_trip(int n, Send&& send, Pump&& pump) {
    g_bench_recv = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) {
        if (!send()) return -1;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (g_bench_recv.load() <= i) {
            pump();
            if (std::chrono::steady_clock::now() > deadline) return -2;
        }
    }
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
}

/* dispatch threading: B's subscriber, async response, events and log lines park on its node
 * queue and run only at b.dispatch() on this thread, and a handle on its own queue runs at
 * that queue's dispatch, while both service threads run */
static bool queue_leg() {
    int fails_at_entry = g_failures;
    rant::NodeOptions opts;
    opts.domain = 50;
    opts.multicast_interface = "127.0.0.1";
    opts.max_topics = 32;
    rant::Node a("QA", opts);
    rant::NodeOptions dopts = opts;
    dopts.threading = rant::Threading::Dispatch;
    rant::Node b("QB", dopts);
    chk("queue: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;
    chk("queue: B dispatches", b.threading() == rant::Threading::Dispatch);
    chk("queue: poll is refused off Manual", b.poll(0) == (int)rant::SendStatus::State);
    chk("queue: dispatch is refused off Dispatch", a.dispatch() == (int)rant::SendStatus::State);
    std::atomic<int> events{ 0 };
    std::thread::id me = std::this_thread::get_id();
    std::atomic<bool> stray{ false };
    auto mark = [&] { if (std::this_thread::get_id() != me) stray = true; };
    b.on_event([&](const rant::Event&) { mark(); events++; });

    std::vector<std::string> lines;
    a.log(rant::LogLevel::Warn, "before");
    chk("queue: on_log binds", b.on_log([&](const rant::LogLine& l) {
        mark();
        if (l.level == rant::LogLevel::Warn) lines.emplace_back(l.text);
    }));

    rant::Qos rel; rel.reliability = rant::Reliability::Reliable;
    std::vector<int32_t> got;
    auto pub = a.publisher<Speed>("q/speed", rel);
    auto sub = b.subscriber<Speed>("q/speed", [&](const Speed& s) { mark(); got.push_back(s.v); }, rel);
    auto def = a.function_definition<AddReq, AddRsp>("q/add",
        [](const AddReq& r) { return AddRsp{ (int64_t)r.x + r.y }; });
    auto rf = b.remote_function<AddReq, AddRsp>("q/add");
    auto drained = [&](const std::function<bool()>& pred) {
        return wait_for(4000, [&] { b.dispatch(0, 0); return pred(); });
    };
    chk("queue: matched", drained([&] { return pub.match_count() > 0 && pub.ready()
                                             && rf.match_count() > 0; }));
    chk("queue: the replayed line arrives at dispatch",
        drained([&] { return !lines.empty() && lines[0] == "before"; }));

    chk("queue: send", pub.send(Speed{ 5 }) == rant::SendStatus::Ok);
    wait_for(300, [] { return false; });
    chk("queue: the message waits for dispatch", got.empty());
    chk("queue: dispatch runs it", drained([&] { return got.size() == 1 && got[0] == 5; }));

    int64_t sum = 0;
    chk("queue: async call accepted", rf.call_async(AddReq{ 2, 3 },
        [&](const rant::ResponseView<AddRsp>& rv) { mark(); if (rv.ok()) sum = rv.value()->sum; })
        == rant::SendStatus::Ok);
    chk("queue: the response arrives at dispatch", drained([&] { return sum == 5; }));
    a.log(rant::LogLevel::Warn, "live");
    chk("queue: a live line arrives at dispatch",
        drained([&] { return lines.size() >= 2 && lines.back() == "live"; }));
    chk("queue: events arrive at dispatch", drained([&] { return events.load() > 0; }));

    /* a handle on its own queue runs at that queue's dispatch, never at the node's */
    rant::Queue q = b.create_queue();
    chk("queue: created", q.valid());
    rant::Qos own = rel; own.queue = &q;
    std::vector<int32_t> extra;
    auto xpub = a.publisher<Speed>("q/extra", rel);
    auto xsub = b.subscriber<Speed>("q/extra", [&](const Speed& s) { mark(); extra.push_back(s.v); }, own);
    chk("queue: own queue matched", wait_for(4000, [&] { return xpub.match_count() > 0 && xpub.ready(); }));
    xpub.send(Speed{ 9 });
    chk("queue: the node dispatch leaves it",
        wait_for(4000, [&] { b.dispatch(0, 0); return q.stats().waiting > 0; }) && extra.empty());
    chk("queue: its own dispatch runs it",
        wait_for(4000, [&] { q.dispatch(0, 0); return extra.size() == 1 && extra[0] == 9; }));
    chk("queue: every callback ran on this thread", !stray.load());
    bool refused = false;
#if defined(__cpp_exceptions)
    try { auto other = b.publisher<Speed>("q/extra", rel); }
    catch (const rant::Error&) { refused = true; }
    chk("queue: a same name handle on another queue is refused", refused);
#endif
/* pull: no handler, the app takes when it wants and nothing parks on a queue */
    auto ppub = a.publisher<Speed>("q/pull", rel);
    auto pulled = b.subscriber<Speed>("q/pull", rel);
    chk("pull: matched", wait_for(4000, [&] { return ppub.match_count() > 0 && ppub.ready(); }));
    for (int32_t v = 1; v <= 3; v++) ppub.send(Speed{ v });
    auto first = pulled.take(2000);
    chk("pull: take waits for the oldest", first && first->v == 1);
    std::optional<Speed> newest;
    wait_for(4000, [&] { auto l = pulled.take_latest(); if (l) newest = l; return newest && newest->v == 3; });
    chk("pull: take_latest reaches the newest", newest && newest->v == 3);
    chk("pull: nothing left after take_latest", !pulled.take());
#if defined(__cpp_exceptions)
    refused = false;
    try { (void)sub.take(); } catch (const rant::Error&) { refused = true; }
    chk("pull: take on a handler subscriber throws", refused);
#endif
    return g_failures == fails_at_entry;
}

/* handle lifetime: holds per side on a shared name, moves, the last close retiring the
 * name, handles and deferred replies that outlive their node, a close refused in a callback */
static bool lifetime_leg() {
    int fails_at_entry = g_failures;
    rant::NodeOptions opts;
    opts.domain = 52;
    opts.multicast_interface = "127.0.0.1";
    opts.max_topics = 32;
    rant::Node a("LA", opts);
    rant::Node b("LB", opts);
    chk("life: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;
    rant::Qos rel; rel.reliability = rant::Reliability::Reliable;

    /* two publishers share a name: closing one leaves the other live */
    auto p1 = a.publisher<Speed>("l/speed", rel);
    auto p2 = a.publisher<Speed>("l/speed", rel);
    std::atomic<int> first{ 0 }, second{ 0 };
    std::atomic<int32_t> last{ 0 };
    auto s1 = b.subscriber<Speed>("l/speed", [&](const Speed& s) { first++; last = s.v; }, rel);
    auto s2 = b.subscriber<Speed>("l/speed", [&](const Speed&) { second++; }, rel);
    chk("life: matched", wait_for(4000, [&] { return p2.match_count() > 0 && p2.ready(); }));
    chk("life: closing one publisher succeeds", p1.close() == rant::SendStatus::Ok && !p1.valid());
    chk("life: the other publisher still sends", p2.valid() && p2.send(Speed{ 1 }) == rant::SendStatus::Ok);
    chk("life: both subscribers get it", wait_for(3000, [&] { return first.load() == 1 && second.load() == 1; }));

    /* a closed subscriber's handler stops, the other keeps its own */
    chk("life: closing one subscriber succeeds", s2.close() == rant::SendStatus::Ok);
    p2.send(Speed{ 2 });
    chk("life: the open subscriber still receives", wait_for(3000, [&] { return last.load() == 2; }));
    chk("life: the closed one no longer fires", second.load() == 1);

    /* moves carry the hold, the moved from handle is empty */
    auto p3 = std::move(p2);
    chk("life: a moved handle is empty", !p2.valid() && p3.valid());
    chk("life: the moved to handle sends", p3.send(Speed{ 3 }) == rant::SendStatus::Ok);
    chk("life: its message arrives", wait_for(3000, [&] { return last.load() == 3; }));

    /* the last handle on a name retires it, so the name takes another schema */
#if defined(__cpp_exceptions)
    bool refused = false;
    try { auto clash = a.publisher<Flat>("l/speed", rel); }
    catch (const rant::Error&) { refused = true; }
    chk("life: a live name refuses another schema", refused);
#endif
    p3.close();
    auto retyped = a.publisher<Flat>("l/speed", rel);
    chk("life: after the last close the name retypes", retyped.valid());
    retyped.close();

    /* a definition closed by scope frees its name for a successor */
    {
        auto def = a.function_definition<AddReq, AddRsp>("l/add",
            [](const AddReq& r) { return AddRsp{ (int64_t)r.x + r.y }; });
        chk("life: definition made", def.valid());
    }
    auto again = a.function_definition<AddReq, AddRsp>("l/add",
        [](const AddReq& r) { return AddRsp{ (int64_t)r.x * r.y }; });
    chk("life: the name is free after scope end", again.valid());

    /* handles and a deferred reply that outlive their node answer NoTopic, never crash */
    rant::NodeOptions copts = opts;
    copts.threading = rant::Threading::Manual;
    auto c = std::make_unique<rant::Node>("LC", copts);
    auto orphan = c->publisher<Speed>("l/orphan");
    auto orphan_var = c->variable_definition<float>("l/var", { 1.0f });
    rant::Deferred<AddRsp> parked;
    auto deferring = c->function_definition<AddReq, AddRsp>("l/defer",
        [&](const AddReq&, rant::Request<AddRsp>& rq) { parked = rq.defer(); });
    auto caller = a.remote_function<AddReq, AddRsp>("l/defer");
    wait_for(4000, [&] { return caller.match_count() > 0; }, c.get());
    (void)caller.call_async(AddReq{ 1, 2 }, [](const rant::ResponseView<AddRsp>&) {});
    wait_for(3000, [&] { return parked.valid(); }, c.get());
    chk("life: a deferred reply is parked", parked.valid());
    c->close();
    chk("life: a handle outliving its node is not valid", !orphan.valid());
    chk("life: its send answers NoTopic", orphan.send(Speed{ 1 }) == rant::SendStatus::NoTopic);
    chk("life: a variable outliving its node reads nothing", !orphan_var.get());
    chk("life: a deferred reply outliving its node is not valid", !parked.valid()
        && !parked.complete(AddRsp{ 3 }));
    c.reset();
    chk("life: its close answers Ok and it stays empty",
        orphan.close() == rant::SendStatus::Ok && !orphan.valid());

    /* a handle destroyed inside its own inline callback cannot retire: an error event says so */
    rant::NodeOptions mopts = opts;
    mopts.threading = rant::Threading::Manual;
    rant::Node d("LD", mopts);
    std::atomic<bool> state_seen{ false };
    d.on_event([&](const rant::Event& e) {
        if (e.error() == rant::ErrorKind::State && e.topic_name() == "l/self") state_seen = true;
    });
    auto self_pub = a.publisher<Speed>("l/self", rel);
    std::optional<rant::Subscriber<Speed>> self_sub;
    self_sub.emplace(d.subscriber<Speed>("l/self", [&](const Speed&) { self_sub.reset(); }, rel));
    chk("life: self closing subscriber matched",
        wait_for(4000, [&] { return self_pub.match_count() > 0 && self_pub.ready(); }, &d));
    self_pub.send(Speed{ 4 });
    chk("life: the refused close raises a State event",
        wait_for(3000, [&] { return state_seen.load(); }, &d) && !self_sub);
    return g_failures == fails_at_entry;
}

/* loud failures: a handler's throw text reaches the caller, a cancelled task's partial result
 * reads, a blocking call inside an inline callback throws */
static bool failure_leg() {
    int fails_at_entry = g_failures;
    rant::NodeOptions opts;
    opts.domain = 53;
    opts.multicast_interface = "127.0.0.1";
    opts.max_topics = 32;
    rant::Node a("XA", opts);
    rant::Node b("XB", opts);
    chk("fail: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;

#if defined(__cpp_exceptions)
    auto boom = a.function_definition<AddReq, AddRsp>("x/boom",
        [](const AddReq&) -> AddRsp { throw std::runtime_error("the answer is not ready"); });
    auto boom_r = b.remote_function<AddReq, AddRsp>("x/boom");
    chk("fail: matched", wait_for(4000, [&] { return boom_r.match_count() > 0; }));
    auto r = boom_r.call(AddReq{ 1, 2 }, 3000);
    chk("fail: a throwing handler answers AppError", !r && r.status() == rant::CallStatus::AppError);
    chk("fail: with the exception text", r.message() == "the answer is not ready");
    chk("fail: and no value", !r.value());
#endif

    /* a task honoring a cancel with a partial result: the value reads though not Ok */
    rant::PendingTask<MoveProgress, MoveRsp> parked;
    std::mutex parked_mu;
    auto slow = a.task_definition<MoveReq, MoveProgress, MoveRsp>("x/slow",
        [&](const MoveReq&, rant::TaskRequest<MoveProgress, MoveRsp>& rq) {
            std::lock_guard<std::mutex> g(parked_mu);
            parked = rq.defer();
        });
    auto slow_r = b.remote_task<MoveReq, MoveProgress, MoveRsp>("x/slow");
    chk("fail: task matched", wait_for(4000, [&] { return slow_r.match_count() > 0; }));
    uint32_t call_id = 0;
    std::atomic<bool> done{ false };
    std::optional<MoveRsp> partial;
    rant::CallStatus final_status = rant::CallStatus::Ok;
    rant::CallOptions co; co.id_out = &call_id;
    (void)slow_r.call_async(MoveReq{ 9.0 }, {}, [&](const rant::ResponseView<MoveRsp>& rv) {
        final_status = rv.status();
        partial = rv.value();
        done = true;
    }, co);
    chk("fail: the task parked", wait_for(3000, [&] { std::lock_guard<std::mutex> g(parked_mu); return parked.valid(); }));
    slow_r.cancel(call_id);
    chk("fail: the cancel arrived", wait_for(3000, [&] { std::lock_guard<std::mutex> g(parked_mu); return parked.cancelled(); }));
    {
        std::lock_guard<std::mutex> g(parked_mu);
        parked.complete_cancelled("stopped halfway", MoveRsp{ 4.5 });
    }
    chk("fail: the caller got Cancelled", wait_for(3000, [&] { return done.load(); })
        && final_status == rant::CallStatus::Cancelled);
    chk("fail: with the partial result", partial && partial->final_position == 4.5);

    rant::Qos rel; rel.reliability = rant::Reliability::Reliable;
#if defined(__cpp_exceptions)
    /* a blocking call from the loop thread would stall the loop it waits on */
    auto add = a.function_definition<AddReq, AddRsp>("x/add",
        [](const AddReq& q) { return AddRsp{ (int64_t)q.x + q.y }; });
    auto add_r = b.remote_function<AddReq, AddRsp>("x/add");
    chk("fail: add matched", wait_for(4000, [&] { return add_r.match_count() > 0; }));
    std::atomic<int> threw{ 0 };
    auto ping = a.publisher<Speed>("x/ping", rel);
    auto trigger = b.subscriber<Speed>("x/ping", [&](const Speed&) {
        try { (void)add_r.call(AddReq{ 1, 1 }, 500); threw = 1; }
        catch (const rant::Error&) { threw = 2; }
    }, rel);
    chk("fail: ping matched", wait_for(4000, [&] { return ping.match_count() > 0 && ping.ready(); }));
    ping.send(Speed{ 1 });
    chk("fail: a blocking call inside an inline callback throws",
        wait_for(3000, [&] { return threw.load() != 0; }) && threw.load() == 2);
#endif
    return g_failures == fails_at_entry;
}

/* the node factories: a raw handle typed by the mesh, and the refusals */
static bool factory_leg() {
    int fails_at_entry = g_failures;
    rant::NodeOptions opts;
    opts.domain = 51;
    opts.multicast_interface = "127.0.0.1";
    rant::Node a("FA", opts);
    rant::Node b("FB", opts);
    chk("factory: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;

    rant::Qos mesh; mesh.reflect_from_mesh = true;
    std::atomic<int> hits{ 0 };
    std::atomic<uint64_t> seen{ 0 };
    auto pub = a.publisher<Speed>("f/speed");
    auto raw = b.subscriber<rant::Bytes>("f/speed",
        [&](const rant::MessageView& m) { seen = (uint64_t)m.get_int("v"); hits++; }, mesh);
    chk("factory: reflect_from_mesh subscriber matched",
        wait_for(4000, [&] { return pub.match_count() > 0 && pub.ready(); }));
    pub.send(Speed{ 7 });
    chk("factory: reflect_from_mesh subscriber reads by name",
        wait_for(3000, [&] { return hits.load() > 0; }) && seen.load() == 7);

    /* reflection: find A among B's peers, then its meta snapshot both ways */
    uint32_t a_id = 0;
    wait_for(4000, [&] {
        for (const auto& p : b.reflection().peers()) if (p.active && p.name == "FA") a_id = p.id;
        return a_id != 0;
    });
    chk("reflection: B sees A", a_id != 0);
    rant::MetaSnapshot snap = b.reflection().meta(a_id, rant::MetaNode, 2000);
    chk("reflection: blocking meta answers", snap.valid && snap.node.name == "FA");
    std::atomic<bool> async_named{ false };
    chk("reflection: meta_async launches", b.reflection().meta_async(a_id,
        [&](const rant::MetaSnapshot& s) { async_named = s.valid && s.node.name == "FA"; })
        == rant::SendStatus::Ok);
    chk("reflection: meta_async answers", wait_for(3000, [&] { return async_named.load(); }));
    chk("reflection: epoch moved", b.reflection().epoch() != 0);

#if defined(__cpp_exceptions)
    auto refuses = [](auto make) {
        try { make(); } catch (const rant::Error&) { return true; }
        return false;
    };
    rant::Schema s = a.schema("{ v: i32 }");
    chk("factory: a typed handle refuses a schema",
        refuses([&] { (void)a.publisher<Speed>("f/typed", {}, s); }));
    chk("factory: a typed handle refuses reflect_from_mesh",
        refuses([&] { (void)a.subscriber<Speed>("f/typed", mesh); }));
    chk("factory: a definition refuses an empty handler",
        refuses([&] { (void)a.function_definition<rant::Bytes, rant::Bytes>("f/fn",
                          rant::FunctionDefinition<rant::Bytes, rant::Bytes>::Handler{}); }));
    chk("factory: a subscriber refuses an empty handler",
        refuses([&] { (void)a.subscriber<rant::Bytes>("f/raw", rant::Node::MessageHandler{}); }));
    rant::NodeOptions quiet = opts;
    quiet.disable_logs = true;
    quiet.threading = rant::Threading::Manual;
    rant::Node nolog("FQ", quiet);
    chk("factory: on_log refuses when logs are disabled",
        refuses([&] { (void)nolog.on_log([](const rant::LogLine&) {}); }));
#endif
    return g_failures == fails_at_entry;
}

static bool bench_leg() {
    bool ok = true;
    size_t sink = 0;   /* keeps the optimizer from dropping an encode or decode */
    std::vector<uint8_t> scratch;

    Flat flat{ 7, 2.5f }, flat_out{};
    Padded padded{}, padded_out{};
    padded.a = 9; padded.b = 0x11223344u; padded.c = 777; padded.d = -3.5; padded.tag.assign("robot");
    narrow::Chunk chunk, chunk_out;
    chunk.seq = 7; chunk.samples = { 1.5f, -2.5f, 8.75f }; chunk.note = "tail";
    rant::NodeOptions copt;
    copt.domain = 49;
    copt.multicast_interface = "127.0.0.1";
    copt.threading = rant::Threading::Manual;
    rant::Node cn("bench-codec", copt);   /* holds the codecs */
    auto ps = rant::priv::schema_of<Padded>(cn);
    rant::priv::TypeCodec* cflat = rant::priv::type_codec<Flat>(cn);
    rant::priv::TypeCodec* cpad = rant::priv::type_codec<Padded>(cn);
    rant::priv::TypeCodec* cchunk = rant::priv::type_codec<narrow::Chunk>(cn);
    if (!ps || !cflat || !cchunk) { std::printf("bench: no Padded schema\n"); return false; }

    const int N = 200000;
    std::printf("bench: codec only, ns per operation, %d each\n", N);
    std::printf("  %-36s %10s %10s\n", "path", "encode", "decode");
    auto row = [](const char* name, double a, double b) { std::printf("  %-36s %10.1f %10.1f\n", name, a, b); };
    {
        rant::Bytes w = rant::priv::encode(*cflat, flat, scratch);
        row("typed memcpy (Flat)",
            ns_per(N, [&] { sink += rant::priv::encode(*cflat, flat, scratch).size(); }),
            ns_per(N, [&] { sink += rant::priv::decode(*cflat, flat_out, w, nullptr); }));
    }
    {
        std::vector<uint8_t> keep;
        rant::Bytes w = rant::priv::encode(*cpad, padded, keep);
        row("typed loop (Padded, string<7>)",
            ns_per(N, [&] { sink += rant::priv::encode(*cpad, padded, scratch).size(); }),
            ns_per(N, [&] { sink += rant::priv::decode(*cpad, padded_out, w, nullptr); }));
    }
    {
        std::vector<uint8_t> keep;
        rant::Bytes w = rant::priv::encode(*cchunk, chunk, keep);
        row("typed tails (Chunk, f32[] string)",
            ns_per(N, [&] { sink += rant::priv::encode(*cchunk, chunk, scratch).size(); }),
            ns_per(N, [&] { sink += rant::priv::decode(*cchunk, chunk_out, w, nullptr); }));
    }
    {
        rant::MessageBuilder one(*ps);
        one.set_uint("a", 9).set_uint("b", 0x11223344u).set_uint("c", 777).set_f64("d", -3.5).set_string("tag", "robot");
        rant::Bytes w = one.bytes();
        row("dynamic (MessageBuilder, FieldView)",
            ns_per(N, [&] {
                rant::MessageBuilder mb(*ps);
                mb.set_uint("a", 9).set_uint("b", 0x11223344u).set_uint("c", 777).set_f64("d", -3.5).set_string("tag", "robot");
                sink += mb.bytes().size();
            }),
            ns_per(N, [&] {   /* the C accessors FieldView forwards to, since a view only exists in a handler */
                rant::detail::RantBytes d = rant::detail::rant_bytes(w.data(), w.size());
                const rant::detail::RantSchema* sc = ps->raw();
                sink += (size_t)(rant::detail::rant_get_uint(d, sc, "a") + rant::detail::rant_get_uint(d, sc, "b")
                                 + rant::detail::rant_get_uint(d, sc, "c") + (uint64_t)rant::detail::rant_get_f64(d, sc, "d")
                                 + rant::detail::rant_get_string(d, sc, "tag").len);
            }));
    }

    const int M = 2000;
    std::printf("bench: loopback, one message at a time, both nodes polled here, us per message, %d each\n", M);
    auto line = [&](const char* name, double us) {
        std::printf("  %-36s %10.1f%s\n", name, us, us < 0 ? "  (failed)" : "");
        if (us < 0) ok = false;
    };

    {   /* the C reference: two C nodes, a typed topic, a C callback that only counts */
        rant::detail::RantNodeOpts co{};
        co.domain = 50;
        co.net.multicast_interface = "127.0.0.1";
        rant::detail::RantAllocator ca = rant::detail::rant_allocator_heap(0);
        rant::detail::RantAllocator cb = rant::detail::rant_allocator_heap(0);
        rant::detail::RantNode* na = rant::detail::rant_node_open(&ca, "CA", nullptr, nullptr, &co);
        rant::detail::RantNode* nb = rant::detail::rant_node_open(&cb, "CB", bench_c_message, nullptr, &co);
        const rant::detail::RantSchema* fs = na ? rant::detail::rant_node_schema(na, "Flat { a: u32, b: f32 }") : nullptr;
        rant::detail::RantTopicOpts to{};
        rant::detail::RantTopic* ta = na && fs ? rant::detail::rant_node_create_topic(na, "flat", rant::detail::RANT_PUB_ONLY, fs, &to) : nullptr;
        rant::detail::RantTopic* tb = nb && fs ? rant::detail::rant_node_create_topic(nb, "flat", rant::detail::RANT_SUB_ONLY, fs, &to) : nullptr;
        auto pump = [&] { rant::detail::rant_node_poll(na, 0); rant::detail::rant_node_poll(nb, 0); };
        bool matched = ta && tb && wait_for(4000, [&] { pump(); return rant::detail::rant_topic_match_count(ta) > 0 && rant::detail::rant_topic_ready(ta) == 1; });
        line("C (reference)", matched ? us_per_round_trip(M,
            [&] { return rant::detail::rant_topic_send(ta, rant::detail::rant_bytes(&flat, sizeof flat), nullptr) == (int)rant::SendStatus::Ok; },
            pump) : -1.0);
        if (na) rant::detail::rant_node_close(na, 1);
        if (nb) rant::detail::rant_node_close(nb, 1);
    }

    rant::NodeOptions opts;
    opts.domain = 49;
    opts.multicast_interface = "127.0.0.1";
    opts.threading = rant::Threading::Manual;   /* both pumped from the bench thread */
    rant::Node a("BA", opts);
    rant::Node b("BB", opts);
    if (!a.valid() || !b.valid()) { std::printf("bench: nodes failed\n"); return false; }
    auto pump = [&] { a.poll(0); b.poll(0); };
    auto matched = [&](auto& pub) { return wait_for(4000, [&] { pump(); return pub.match_count() > 0 && pub.ready(); }); };

    {
        auto pub = a.publisher<Flat>("b/flat");
        auto sub = b.subscriber<Flat>("b/flat", [](const Flat&) { g_bench_recv++; });
        line("typed memcpy (Flat)", matched(pub) ? us_per_round_trip(M,
            [&] { return pub.send(flat) == rant::SendStatus::Ok; }, pump) : -1.0);
    }
    {
        auto pub = a.publisher<Padded>("b/padded");
        auto sub = b.subscriber<Padded>("b/padded", [](const Padded&) { g_bench_recv++; });
        line("typed loop (Padded, string<7>)", matched(pub) ? us_per_round_trip(M,
            [&] { return pub.send(padded) == rant::SendStatus::Ok; }, pump) : -1.0);
    }
    {
        auto pub = a.publisher<narrow::Chunk>("b/chunk");
        auto sub = b.subscriber<narrow::Chunk>("b/chunk", [](const narrow::Chunk&) { g_bench_recv++; });
        line("typed tails (Chunk, f32[] string)", matched(pub) ? us_per_round_trip(M,
            [&] { return pub.send(chunk) == rant::SendStatus::Ok; }, pump) : -1.0);
    }
    {
        auto pub = a.publisher<rant::Bytes>("b/dyn", {}, *ps);
        auto sub = b.subscriber<rant::Bytes>("b/dyn", [&](const rant::MessageView& v) {
            sink += (size_t)(v.get_uint("a") + v.get_uint("b") + v.get_uint("c") + (uint64_t)v.get_f64("d") + v.get_string("tag").size());
            g_bench_recv++;
        }, {}, *ps);
        line("dynamic (MessageBuilder, FieldView)", matched(pub) ? us_per_round_trip(M, [&] {
                rant::MessageBuilder mb(*ps);
                mb.set_uint("a", 9).set_uint("b", 0x11223344u).set_uint("c", 777).set_f64("d", -3.5).set_string("tag", "robot");
                return pub.send(mb) == rant::SendStatus::Ok;
            }, pump) : -1.0);
    }
    std::printf("bench: %s (sink %zu)\n", ok ? "PASS" : "FAIL", sink);
    return ok;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "bench") == 0) return bench_leg() ? 0 : 2;
#if defined(__cpp_exceptions)
  try {
#endif
    rant::NodeOptions opts;
    opts.domain = 42;
    opts.multicast_interface = "127.0.0.1";   /* single-host discovery */

    auto on_msg = [](const rant::MessageView& m) { decode(m); };
    auto on_evt = [](const char* tag) {
        return [tag](const rant::Event& e) { std::printf("event(%s): %s\n", tag, e.to_string().c_str()); };
    };

    rant::NodeOptions manual = opts;
    manual.threading = rant::Threading::Manual;   /* both pumped from this thread first */
    rant::Node a("A", manual);
    rant::Node b("B", manual);
    if (!a.valid() || !b.valid()) { std::printf("FAIL: node construction\n"); return 1; }
    a.on_event(on_evt("A"));
    b.on_event(on_evt("B"));

    rant::Schema schema = a.schema(SCHEMA);
    if (!schema) { std::printf("FAIL: schema: %s\n", a.last_error().c_str()); return 1; }
    std::printf("schema '%.*s' size=%u fields=%u\n",
                (int)schema.name().size(), schema.name().data(), schema.size(), schema.field_count());

    auto pub = a.publisher<rant::Bytes>("t", { rant::Reliability::Reliable }, schema);
    auto sub = b.subscriber<rant::Bytes>("t", on_msg, { rant::Reliability::Reliable }, schema);
    if (!pub.valid() || !sub.valid()) { std::printf("FAIL: topic construction\n"); return 1; }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    uint32_t seq = 0;
    while (!g.received && std::chrono::steady_clock::now() < deadline) {
        if (pub.match_count() > 0 && !send_one(pub, schema, ++seq)) return 1;
        a.poll(1);
        b.poll(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!g.received) { std::printf("FAIL: no message within timeout (match_count=%d)\n", pub.match_count()); return 1; }

    std::printf("B received seq=%u, checking fields:\n", g.seq);
    bool dyn_ok = verify_dynamic();
    std::printf("%s\n", dyn_ok ? "PASS: variable kinds crossed and decoded" : "FAIL: decoded value mismatch");

    /* threaded mode: a fresh pair on its service threads, no poll() from us */
    a.close();
    b.close();
    rant::Node ta("TA", opts);
    rant::Node tb("TB", opts);
    if (!ta.valid() || !tb.valid()) { std::printf("FAIL: threaded node construction\n"); return 3; }
    ta.on_event(on_evt("TA"));
    tb.on_event(on_evt("TB"));
    if (ta.poll(0) != (int)rant::SendStatus::State) { std::printf("FAIL: poll not refused on the service thread\n"); return 3; }
    rant::Schema tschema = ta.schema(SCHEMA);
    auto tpub = ta.publisher<rant::Bytes>("t", { rant::Reliability::Reliable }, tschema);
    auto tsub = tb.subscriber<rant::Bytes>("t", on_msg, { rant::Reliability::Reliable }, tschema);
    g.received = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!(tpub.match_count() > 0 && tpub.ready()) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!send_one(tpub, tschema, ++seq)) { std::printf("FAIL: threaded send\n"); return 3; }
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!g.received && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!g.received || g.note != NOTE) { std::printf("FAIL: threaded delivery\n"); return 3; }
    std::printf("PASS: threaded delivery on the service thread (evicted_unsent=%u)\n", ta.stats().evicted_unsent);
    ta.close();
    tb.close();

    /* patterns + typed codec leg */
    std::printf("patterns leg:\n");
    bool pat_ok = patterns_leg();
    std::printf("%s\n", pat_ok ? "PASS: patterns + typed codec" : "FAIL: patterns leg");

    /* bare-type roots: the type itself is the schema */
    std::printf("value-root leg:\n");
    bool root_ok = value_root_leg();
    std::printf("%s\n", root_ok ? "PASS: bare-type roots" : "FAIL: value-root leg");

    /* nested structs and struct arrays */
    std::printf("nested leg:\n");
    bool nest_ok = nested_leg();
    std::printf("%s\n", nest_ok ? "PASS: nested structs" : "FAIL: nested leg");

    /* the standard type library */
    std::printf("stdtypes leg:\n");
    bool std_ok = stdtypes_leg();
    std::printf("%s\n", std_ok ? "PASS: standard types" : "FAIL: stdtypes leg");

    /* variable members as tail frames */
    std::printf("tail-member leg:\n");
    bool tl_ok = tails_leg();
    std::printf("%s\n", tl_ok ? "PASS: variable members" : "FAIL: tail-member leg");

    /* tasks: progress + cancellation over the function machinery */
    std::printf("tasks leg:\n");
    bool task_ok = tasks_leg();
    std::printf("%s\n", task_ok ? "PASS: tasks" : "FAIL: tasks leg");

    /* callback queues */
    std::printf("queue leg:\n");
    bool queue_ok = queue_leg();
    std::printf("%s\n", queue_ok ? "PASS: callback queues" : "FAIL: queue leg");

    std::printf("lifetime leg:\n");
    bool life_ok = lifetime_leg();
    std::printf("%s\n", life_ok ? "PASS: handle lifetime" : "FAIL: lifetime leg");

    std::printf("failure leg:\n");
    bool fail_ok = failure_leg();
    std::printf("%s\n", fail_ok ? "PASS: loud failures" : "FAIL: failure leg");

    std::printf("factory leg:\n");
    bool factory_ok = factory_leg();
    std::printf("%s\n", factory_ok ? "PASS: node factories" : "FAIL: factory leg");

    std::printf("%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 2;
#if defined(__cpp_exceptions)
  } catch (const rant::Error& e) {
    std::printf("FAIL: unexpected rant::Error: %s\n", e.what());
    return 4;
  }
#endif
}
