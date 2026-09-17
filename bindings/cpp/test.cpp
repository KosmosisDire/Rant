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

    rant::Node a("PA", {}, on_evt("PA"), opts);
    rant::Node b("PB", {}, on_evt("PB"), opts);
    chk("patterns: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;

    /* A runs on its service thread, B is pumped from this thread so the blocking
     * call and wait forms, which drive the caller's loop, are exercised */
    chk("patterns: A started", a.start());

    /* functions */
    rant::FunctionDefinition<AddReq, AddRsp> def_add(a, "add",
        [](const AddReq& q) { return AddRsp{ (int64_t)q.x + q.y }; });
    rant::FunctionDefinition<AddReq, AddRsp> def_chk(a, "chk",
        [](const AddReq& q, rant::Request<AddRsp>& rq) {
            if (q.x < 0) rq.fail();
            else         rq.reply(AddRsp{ (int64_t)q.x * q.y });
        });
    rant::FunctionDefinition<AddReq, AddRsp> def_defer(a, "defr",
        [](const AddReq& q, rant::Request<AddRsp>& rq) {
            (void)q;
            std::lock_guard<std::mutex> g(g_deferred_mu);
            g_deferred = rq.defer();
            g_deferred_ready = true;
        });
    chk("patterns: definitions created", def_add.valid() && def_chk.valid() && def_defer.valid());

    rant::RemoteFunction<AddReq, AddRsp> rf_add(b, "add");
    rant::RemoteFunction<AddReq, AddRsp> rf_chk(b, "chk");
    rant::RemoteFunction<AddReq, AddRsp> rf_defer(b, "defr");
    chk("patterns: remotes created", rf_add.valid() && rf_chk.valid() && rf_defer.valid());

    chk("patterns: definition discovered", wait_for(4000,
        [&] { return rf_add.match_count() > 0 && rf_chk.match_count() > 0 && rf_defer.match_count() > 0; }, &b));
    /* settle B so the rsp lanes from A to B are formed before the first blocking
     * call, since match_count proves only the req direction */
    chk("patterns: settle", b.settle(4000));

    /* blocking round trip (memcpy-path structs both ways) */
    auto r1 = rf_add.call(AddReq{ 20, 22 }, 3000);
    chk("patterns: blocking call Ok 20+22=42",
        r1.ok() && r1->sum == 42 && r1.provider() != 0);

    /* full-form handler replying */
    auto r2 = rf_chk.call(AddReq{ 6, 7 }, 3000);
    chk("patterns: full-form reply 6*7=42", r2.ok() && r2->sum == 42);

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
    chk("patterns: deferred completion delivers 11", r4.ok() && r4->sum == 11);

    /* async form */
    std::atomic<int64_t> async_sum{ 0 };
    std::atomic<bool>    async_done{ false };
    auto st = rf_add.call_async(AddReq{ 1, 2 },
        [&](const rant::ResponseView<AddRsp>& rv) {
            if (rv.ok()) async_sum = rv->sum;
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
    rant::VariableDefinition<Speed> vd(a, "speed", vo);
    rant::RemoteVariable<Speed>       rv(b, "speed");
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
    rant::Publisher<Flat>    pf(a, "flat", rel);
    rant::Subscriber<Flat> sf(b, "flat",
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
    rant::Publisher<Padded>    pp(a, "padded", rel);
    rant::Subscriber<Padded> sp(b, "padded", [&](const Padded& v, const rant::MessageView& m) {
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
    rant::Publisher<pubside::Telemetry>    tp(a, "tele", rel);
    rant::Subscriber<subside::Telemetry> ts(b, "tele",
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
    auto local = a.entities();
    chk("reflect: local function folded",  find(local, rant::EntityKind::Function, "add") != nullptr);
    chk("reflect: local variable folded",  find(local, rant::EntityKind::Variable, "speed") != nullptr);
    chk("reflect: local topic passes",     find(local, rant::EntityKind::Topic, "flat") != nullptr);
    const rant::Entity* var_ent = find(local, rant::EntityKind::Variable, "speed");
    chk("reflect: variable writable", var_ent && var_ent->writable);

    bool peer_seen = false, peer_fn = false;
    for (const auto& p : b.peers()) {
        if (p.name != "PA") continue;
        peer_seen = true;
        auto es = b.entities(p.id);
        const rant::Entity* e = find(es, rant::EntityKind::Function, "add");
        peer_fn = e && e->provides;
    }
    chk("reflect: peer PA visible", peer_seen);
    chk("reflect: peer function entity provides", peer_fn);

    a.stop();
    return g_failures == fails_at_entry;
}

/* leg 5: standard types. The mirrors carry a wire name, so a Transform field matches only
 * a Transform. The golden vectors are shared with the C, C# and Python bindings. */
static const uint64_t HASH_FLOAT3    = 0x04aa9469cd08b1ddULL;   /* the `Float3` schema */
static const uint64_t HASH_IMAGE     = 0x489841f99f392b85ULL;
static const uint64_t HASH_VIDEO     = 0xf677bd147b513fbcULL;   /* `VideoFrame` */
static const uint64_t HASH_EXTSTREAM = 0xaae502077016ac13ULL;   /* `ExternalVideoStream` */

struct Track {
    rant::Transform at;
    rant::Uuid        id;
    rant::Timestamp when;
    rant::Color       tag;
    rant::Float3      velocity;
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
    rant::Node a("std-a", [](const rant::MessageView&) {}, on_evt, opts);
    rant::Node b("std-b", [](const rant::MessageView&) {}, on_evt, opts);
    chk("std: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;
    chk("std: A started", a.start());     /* A on its service thread, B pumped by wait_for */

    /* the mirrors ARE the wire, so the memcpy fast path stays available */
    chk("std: Transform is 88 bytes", sizeof(rant::Transform) == 88);
    chk("std: Uuid is 16 bytes", sizeof(rant::Uuid) == 16);
    chk("std: Color is 4 bytes", sizeof(rant::Color) == 4);

    {   /* a standard type as a whole schema: its name, its canonical hash */
        auto f3 = rant::Schema::compile("Float3");
        chk("std: Float3 compiles by name alone", f3.has_value());
        if (f3) {
            chk("std: Float3 golden hash", f3->hash() == HASH_FLOAT3);
            chk("std: Float3 is 12 message bytes", f3->size() == 12);
        }
        auto tf    = rant::Schema::compile("Transform");
        auto twist = rant::Schema::compile("Twist");
        chk("std: Transform and Twist are distinct names, never mistaken for each other",
            tf && twist && !twist->can_read(*tf) && !tf->can_read(*twist));
        /* the name NARROWS: an anonymous field of the same shape reads a Transform field,
           never the reverse (a struct ROOT's name stays strict-equal, as before) */
        auto named_f = rant::Schema::compile("W { at: Transform }");
        auto bare_f  = rant::Schema::compile(
            "W { at: { translation: { x: f64, y: f64, z: f64 },"
            "          rotation: { x: f64, y: f64, z: f64, w: f64 }, parent: string<30> } }");
        chk("std: an anonymous field of the same shape reads a Transform field",
            named_f && bare_f && bare_f->can_read(*named_f) && !named_f->can_read(*bare_f));
        if (tf) {
            rant::Schema::Field f;
            int i = tf->field_index("translation");
            chk("std: a named member reports its type name",
                i >= 0 && tf->field_at((uint16_t)i, f) && f.type_name == "Double3");
        }
        auto cloud = rant::Schema::compile("Cloud { pts: Float3[], at: Transform }");
        chk("std: named types nest and array", cloud.has_value());
        if (cloud) {
            rant::Schema::Field f;
            int i = cloud->field_index("pts");
            chk("std: an array element reports its type name",
                i >= 0 && cloud->field_at((uint16_t)i, f)
                       && f.elem_name == "Float3" && f.elem_size == 12);
        }
    }

    {   /* the RANT_SCHEMA codec emits the NAMES, never the inlined shapes */
        rant::Publisher<Track> pub(a, "std/track");
        const rant::Schema* sc = rant::priv::schema_of<Track>();
        std::string txt = sc ? sc->to_dsl() : std::string();
        chk("std: the codec spells named members by name",
            txt.find("at: Transform") != std::string::npos &&
            txt.find("id: Uuid") != std::string::npos &&
            txt.find("when: Timestamp") != std::string::npos &&
            txt.find("velocity: Float3") != std::string::npos);
        chk("std: they print back as hoisted definitions, not inlined shapes",
            txt.find("Transform = {") != std::string::npos &&
            txt.find("Uuid = u8[16]") != std::string::npos);
    }

    {   /* end to end through the typed codec */
        rant::Publisher<Track> pub(a, "std/track2");
        rant::Subscriber<Track> sub(b, "std/track2", [](const Track& t) {
            g_track_x = t.at.translation.x;
            g_track_id0 = t.id.bytes[0];
            g_track_recv++;
        });
        chk("std: publisher and subscriber match",
            wait_for(4000, [&] { return pub.match_count() == 1; }, &b));
        Track t{};
        t.at.translation = { 4.5, -1.25, 9.0 };
        t.at.rotation = rant::identity_rotation();
        t.id.bytes[0] = 0xAB;
        t.when = rant::now();
        t.tag = rant::color_from_hex(0x112233FFu);
        t.velocity = { 1.0f, 2.0f, 3.0f };
        pub.send(t);
        chk("std: a Track crosses whole",
            wait_for(4000, [&] { return g_track_recv.load() > 0; }, &b)
            && g_track_x == 4.5 && g_track_id0 == 0xAB);
    }

    {   /* the video family: mirrors with a variable member ride the codec's tail path */
        const rant::Schema* im = rant::priv::schema_of<rant::Image>();
        const rant::Schema* vf = rant::priv::schema_of<rant::VideoFrame>();
        const rant::Schema* xs = rant::priv::schema_of<rant::ExternalVideoStream>();
        auto imc = rant::Schema::compile("Image");
        auto vfc = rant::Schema::compile("VideoFrame");
        auto xsc = rant::Schema::compile("ExternalVideoStream");
        chk("std: video codecs compile", im && vf && xs && imc && vfc && xsc);
        chk("std: Image codec == canonical", im && imc && im->hash() == imc->hash());
        chk("std: VideoFrame codec == canonical", vf && vfc && vf->hash() == vfc->hash());
        chk("std: ExternalVideoStream codec == canonical", xs && xsc && xs->hash() == xsc->hash());
        chk("std: video golden hashes (shared with every binding)",
            imc && imc->hash() == HASH_IMAGE && vfc && vfc->hash() == HASH_VIDEO
                && xsc && xsc->hash() == HASH_EXTSTREAM);

        /* an Image end to end: the variable payload crosses beside the fixed fields */
        std::atomic<int> img_recv{ 0 };
        rant::Image img_got;
        rant::Publisher<rant::Image> ipub(a, "std/frame");
        rant::Subscriber<rant::Image> isub(b, "std/frame", [&](const rant::Image& i) {
            img_got = i;
            img_recv++;
        });
        chk("std: image pair matched",
            wait_for(4000, [&] { return ipub.match_count() == 1 && ipub.ready(); }, &b));
        rant::Image img;
        img.width = 320; img.height = 4; img.stride = 320;
        img.format = rant::ImageFormat::Mono8;
        img.data.resize((size_t)img.stride * img.height);
        for (size_t i = 0; i < img.data.size(); i++) img.data[i] = (uint8_t)(i * 7);
        chk("std: image send", ipub.send(img) == rant::SendStatus::Ok);
        chk("std: image crosses whole",
            wait_for(4000, [&] { return img_recv.load() > 0; }, &b)
            && img_got.width == 320 && img_got.format == rant::ImageFormat::Mono8
            && img_got.data.size() == img.data.size()
            && img_got.data == img.data);

        /* ExternalVideoStream is fully fixed: the latched-variable use it exists for */
        rant::VariableOptions<rant::ExternalVideoStream> vo;
        rant::ExternalVideoStream st;
        st.kind = rant::VideoStreamKind::Rtsp;
        st.codec = rant::VideoCodec::H264;
        st.width = 1920; st.height = 1080;
        st.url.value.assign("rtsp://cam.local/main");
        st.name.assign("front door");
        vo.initial = st;
        rant::VariableDefinition<rant::ExternalVideoStream> vdef(a, "std/stream", vo);
        rant::RemoteVariable<rant::ExternalVideoStream>         vrem(b, "std/stream");
        chk("std: stream variable replicates", wait_for(4000, [&] {
                auto v = vrem.get();
                return v && v->kind == rant::VideoStreamKind::Rtsp
                         && v->codec == rant::VideoCodec::H264
                         && v->width == 1920 && v->height == 1080
                         && v->url.value == "rtsp://cam.local/main"
                         && v->name == "front door";
            }, &b));
    }

    {   /* the thin operations */
        rant::Color c = rant::color_from_hex(0x11223344u);
        chk("std: Color hex round-trips", c.r == 0x11 && c.a == 0x44
                                          && rant::color_to_hex(c) == 0x11223344u);
        chk("std: vector length", rant::length(rant::Double3{ 3.0, 4.0, 0.0 }) == 5.0);
        rant::Double3 r = rant::rotate(rant::Quaternion{ 0.0, 0.0, 1.0, 0.0 },
                                       rant::Double3{ 1.0, 0.0, 0.0 });
        chk("std: quaternion rotate", r.x < -0.999 && r.x > -1.001);
        rant::Uuid u1 = rant::new_uuid(), u2 = rant::new_uuid();
        chk("std: new_uuid is random and version 4",
            !rant::is_nil(u1) && std::memcmp(u1.bytes, u2.bytes, 16) != 0
            && (u1.bytes[6] & 0xF0u) == 0x40u);
        chk("std: now() is Unix-epoch microseconds", rant::now().us > 1600000000000000LL);
    }
    a.stop();
    return g_failures == fails_at_entry;
}

/* leg 4: bare type roots with no RANT_SCHEMA, the type is the schema */
/* The canonical wire of a bare type is its kind alone, so these hashes are the same in
 * every language binding (pinned in C by rant_test's schema-root phase). */
static const uint64_t HASH_BOOL   = 0xee90234f61d2520bULL;
static const uint64_t HASH_F32ARR = 0x314844e3386a1fc4ULL;

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
    rant::Node a("VA", {}, on_evt("VA"), opts);
    rant::Node b("VB", {}, on_evt("VB"), opts);
    chk("root: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;
    chk("root: A started", a.start());   /* A on its service thread, B pumped by wait_for */

    /* the canonical-hash pin: the typed codec and the DSL agree, and both agree with C */
    const rant::Schema* sb = rant::priv::schema_of<bool>();
    auto arr = rant::Schema::compile("f32[]");
    chk("root: bool codec compiles", sb != nullptr);
    chk("root: `bool` canonical hash", sb && sb->hash() == HASH_BOOL);
    chk("root: `f32[]` canonical hash", arr && arr->hash() == HASH_F32ARR);
    chk("root: bare root is one anonymous field",
        sb && sb->field_count() == 1 && sb->name().empty());

    rant::Qos rel; rel.reliability = rant::Reliability::Reliable;

    /* bool: the whole payload is one byte */
    std::atomic<int> flags{ 0 };
    rant::Publisher<bool>    pb(a, "flag", rel);
    rant::Subscriber<bool> sub_b(b, "flag", [&](const bool& v) { if (v) flags++; }, rel);
    chk("root: bool pair created", pb.valid() && sub_b.valid());
    chk("root: bool matched", wait_for(4000, [&] { return pb.match_count() > 0 && pb.ready(); }, &b));
    chk("root: bool send", pb.send(true) == rant::SendStatus::Ok);
    chk("root: bool round trip", wait_for(3000, [&] { return flags.load() == 1; }, &b));

    /* std::string: the unbounded `string` root, one tail frame */
    std::string got;
    rant::Publisher<std::string>    ps(a, "note", rel);
    rant::Subscriber<std::string> sub_s(b, "note", [&](const std::string& v) { got = v; }, rel);
    chk("root: string pair created", ps.valid() && sub_s.valid());
    chk("root: string matched", wait_for(4000, [&] { return ps.match_count() > 0 && ps.ready(); }, &b));
    chk("root: string send", ps.send(std::string("a bare unbounded string")) == rant::SendStatus::Ok);
    chk("root: string round trip", wait_for(3000, [&] { return got == "a bare unbounded string"; }, &b));

    /* std::array: a fixed array root */
    std::atomic<bool> a3_got{ false };
    std::array<float, 3> a3_seen{};
    rant::Publisher<std::array<float, 3>>    pa3(a, "xyz", rel);
    rant::Subscriber<std::array<float, 3>> sa3(b, "xyz",
        [&](const std::array<float, 3>& v) { a3_seen = v; a3_got = true; }, rel);
    chk("root: array pair created", pa3.valid() && sa3.valid());
    chk("root: array matched", wait_for(4000, [&] { return pa3.match_count() > 0 && pa3.ready(); }, &b));
    chk("root: array send", pa3.send(std::array<float, 3>{ 1.5f, 2.5f, -3.f }) == rant::SendStatus::Ok);
    chk("root: array round trip", wait_for(3000, [&] { return a3_got.load(); }, &b)
        && a3_seen[0] == 1.5f && a3_seen[2] == -3.f);

    /* a variable whose type is a bare double */
    rant::VariableOptions<double> vo;
    vo.initial = 1.25;
    rant::VariableDefinition<double> vd(a, "gain", vo);
    rant::RemoteVariable<double>       rv(b, "gain");
    chk("root: variable pair created", vd.valid() && rv.valid());
    chk("root: variable replicates the initial", wait_for(4000,
        [&] { auto v = rv.get(); return v && *v == 1.25; }, &b));
    chk("root: variable set accepted", rv.set(2.5) == rant::SendStatus::Ok);
    chk("root: variable set converges", wait_for(3000,
        [&] { auto v = vd.get(); return v && *v == 2.5; }, &b));

    /* the dynamic API over a bare root: MessageBuilder/FieldView through the "" path */
    auto f64 = rant::Schema::compile("f64");
    chk("root: dynamic f64 schema", f64 && f64->field_count() == 1);
    if (f64) {
        std::atomic<int> hits{ 0 };
        double seen = 0;
        rant::Publisher<rant::Bytes>  dp(a, "dyn", &*f64, rel);
        rant::Subscriber<rant::Bytes> ds(b, "dyn", &*f64,
            [&](const rant::MessageView& m) { seen = m.get_f64(""); hits++; }, rel);
        chk("root: dynamic pair created", dp.valid() && ds.valid());
        chk("root: dynamic matched", wait_for(4000, [&] { return dp.match_count() > 0 && dp.ready(); }, &b));
        rant::MessageBuilder mb(*f64);
        mb.set_f64("", -7.5);
        chk("root: builder set through the empty path", mb.ok());
        chk("root: dynamic send", dp.send(mb) == rant::SendStatus::Ok);
        chk("root: dynamic round trip", wait_for(3000, [&] { return hits.load() == 1; }, &b));
        chk("root: dynamic value read through the empty path", seen == -7.5);
    }
    a.stop();
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

    {   /* codec-level: spelling, roundtrip, and the bare-vector root pin */
        const rant::Schema* sc = rant::priv::schema_of<narrow::Chunk>();
        chk("tails: Chunk codec compiles", sc != nullptr);
        std::string txt = sc ? sc->to_dsl() : std::string();
        chk("tails: variable members spell as f32[] / string",
            txt.find("samples: f32[]") != std::string::npos &&
            txt.find("note: string") != std::string::npos);

        narrow::Chunk in;
        in.seq = 7;
        in.samples = { 1.5f, -2.5f, 8.75f };
        in.note = "a note well past any small-string buffer, to make the heap real";
        std::vector<uint8_t> scratch;
        rant::Bytes wire = rant::priv::encode(in, scratch);
        chk("tails: encode produces a message", wire.size() > 0);
        narrow::Chunk out;
        chk("tails: decode roundtrips", rant::priv::decode(out, wire, nullptr)
            && out.seq == 7 && out.samples == in.samples && out.note == in.note);

        narrow::Chunk empty_in, empty_out;
        empty_out.samples = { 9.f };            /* stale state must be overwritten */
        empty_out.note = "stale";
        rant::Bytes ewire = rant::priv::encode(empty_in, scratch);
        chk("tails: empty vector and string roundtrip", ewire.size() > 0
            && rant::priv::decode(empty_out, ewire, nullptr)
            && empty_out.samples.empty() && empty_out.note.empty());

        const rant::Schema* vr = rant::priv::schema_of<std::vector<float>>();
        chk("tails: bare std::vector<float> root is canonical `f32[]`",
            vr && vr->hash() == HASH_F32ARR);
    }

    rant::NodeOptions opts;
    opts.domain = 47;
    opts.multicast_interface = "127.0.0.1";
    auto on_evt = [](const char* tag) {
        return [tag](const rant::Event& e) {
            if (e.is_error() && e.error() != rant::ErrorKind::SchemaMismatch)
                std::printf("event(%s): %s\n", tag, e.to_string().c_str());
        };
    };
    rant::Node a("TA", {}, on_evt("TA"), opts);
    rant::Node b("TB", {}, on_evt("TB"), opts);
    chk("tails: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;
    chk("tails: A started", a.start());   /* A on its service thread, B pumped by wait_for */
    rant::Qos rel; rel.reliability = rant::Reliability::Reliable;

    {   /* subset/rebase: wide publisher, narrow subscriber, frames found by path */
        std::atomic<int> got{ 0 };
        narrow::Chunk seen;
        rant::Publisher<wide::Chunk>      pub(a, "tails/chunk", rel);
        rant::Subscriber<narrow::Chunk> sub(b, "tails/chunk", [&](const narrow::Chunk& c) {
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
        rant::Publisher<std::vector<float>>    pv(a, "tails/wave", rel);
        rant::Subscriber<std::vector<float>> sv(b, "tails/wave",
            [&](const std::vector<float>& v) { seen = v; got++; }, rel);
        chk("tails: vector-root pair matched",
            wait_for(4000, [&] { return pv.match_count() == 1 && pv.ready(); }, &b));
        std::vector<float> wave(256);
        for (size_t i = 0; i < wave.size(); i++) wave[i] = (float)i * 0.5f;
        chk("tails: vector-root send", pv.send(wave) == rant::SendStatus::Ok);
        chk("tails: vector-root round trip",
            wait_for(4000, [&] { return got.load() > 0; }, &b) && seen == wave);
    }

    a.stop();
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
    rant::Node a("KA", {}, on_evt("KA"), opts);
    rant::Node b("KB", {}, on_evt("KB"), opts);
    chk("task: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;
    chk("task: A started", a.start());   /* A on its service thread, B pumped from here */

    /* the "move" handler parks every call for a thread the TEST owns */
    rant::TaskDefinition<MoveReq, MoveProgress, MoveRsp> def_move(a, "move",
        [](const MoveReq& q, rant::TaskRequest<MoveProgress, MoveRsp>& rq) {
            (void)q;
            std::lock_guard<std::mutex> g(g_move_mu);
            g_move_pending = rq.defer();      /* implies RUNNING */
            g_move_parked++;
        });
    rant::TaskOptions fixed_opts;
    fixed_opts.no_cancel = true;
    rant::TaskDefinition<MoveReq, MoveProgress, MoveRsp> def_fixed(a, "fixed",
        [](const MoveReq& q, rant::TaskRequest<MoveProgress, MoveRsp>& rq) {
            (void)q;
            std::lock_guard<std::mutex> g(g_fixed_mu);
            g_fixed_pending = rq.defer();
            g_fixed_parked++;
        }, fixed_opts);
    chk("task: definitions created", def_move.valid() && def_fixed.valid());

    std::atomic<uint64_t> cancel_token{ 0 };
    def_move.on_cancel([&](uint64_t token) { cancel_token = token; });

    rant::RemoteTask<MoveReq, MoveProgress, MoveRsp> rt_move(b, "move");
    rant::RemoteTask<MoveReq, MoveProgress, MoveRsp> rt_fixed(b, "fixed");
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
    chk("task: blocking call ends Ok", r1.ok() && r1->final_position == 5.0 && r1.provider() != 0);
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
        find(a.entities(), rant::EntityKind::Task, "move") != nullptr);
    bool peer_task = false, attrs_ok = false, no_cancel_ok = false, prg_schema_ok = false;
    for (const auto& p : b.peers()) {
        if (p.name != "KA") continue;
        auto es = b.entities(p.id);
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
    chk("task: retire mid-run", def_move.retire() == rant::SendStatus::Ok);
    chk("task: retire resolves the caller Cancelled",
        wait_for(4000, [&] { return retired_done.load(); }, &b)
        && retired_status.load() == (int)rant::CallStatus::Cancelled);

    a.stop();
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

static bool bench_leg() {
    bool ok = true;
    size_t sink = 0;   /* keeps the optimizer from dropping an encode or decode */
    std::vector<uint8_t> scratch;

    Flat flat{ 7, 2.5f }, flat_out{};
    Padded padded{}, padded_out{};
    padded.a = 9; padded.b = 0x11223344u; padded.c = 777; padded.d = -3.5; padded.tag.assign("robot");
    narrow::Chunk chunk, chunk_out;
    chunk.seq = 7; chunk.samples = { 1.5f, -2.5f, 8.75f }; chunk.note = "tail";
    const rant::Schema* ps = rant::priv::schema_of<Padded>();
    if (!ps) { std::printf("bench: no Padded schema\n"); return false; }

    const int N = 200000;
    std::printf("bench: codec only, ns per operation, %d each\n", N);
    std::printf("  %-36s %10s %10s\n", "path", "encode", "decode");
    auto row = [](const char* name, double a, double b) { std::printf("  %-36s %10.1f %10.1f\n", name, a, b); };
    {
        rant::Bytes w = rant::priv::encode(flat, scratch);
        row("typed memcpy (Flat)",
            ns_per(N, [&] { sink += rant::priv::encode(flat, scratch).size(); }),
            ns_per(N, [&] { sink += rant::priv::decode(flat_out, w, nullptr); }));
    }
    {
        std::vector<uint8_t> keep;
        rant::Bytes w = rant::priv::encode(padded, keep);
        row("typed loop (Padded, string<7>)",
            ns_per(N, [&] { sink += rant::priv::encode(padded, scratch).size(); }),
            ns_per(N, [&] { sink += rant::priv::decode(padded_out, w, nullptr); }));
    }
    {
        std::vector<uint8_t> keep;
        rant::Bytes w = rant::priv::encode(chunk, keep);
        row("typed tails (Chunk, f32[] string)",
            ns_per(N, [&] { sink += rant::priv::encode(chunk, scratch).size(); }),
            ns_per(N, [&] { sink += rant::priv::decode(chunk_out, w, nullptr); }));
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
        auto fs = rant::Schema::compile("Flat { a: u32, b: f32 }");
        rant::detail::RantTopicOpts to{};
        rant::detail::RantTopic* ta = na && fs ? rant::detail::rant_node_create_topic(na, "flat", rant::detail::RANT_PUB_ONLY, fs->raw(), &to) : nullptr;
        rant::detail::RantTopic* tb = nb && fs ? rant::detail::rant_node_create_topic(nb, "flat", rant::detail::RANT_SUB_ONLY, fs->raw(), &to) : nullptr;
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
    auto on_evt = [](const rant::Event& e) { if (e.is_error()) std::printf("event(bench): %s\n", e.to_string().c_str()); };
    rant::Node a("BA", {}, on_evt, opts);
    rant::Node b("BB", {}, on_evt, opts);
    if (!a.valid() || !b.valid()) { std::printf("bench: nodes failed\n"); return false; }
    auto pump = [&] { a.poll(0); b.poll(0); };
    auto matched = [&](auto& pub) { return wait_for(4000, [&] { pump(); return pub.match_count() > 0 && pub.ready(); }); };

    {
        rant::Publisher<Flat> pub(a, "b/flat");
        rant::Subscriber<Flat> sub(b, "b/flat", [](const Flat&) { g_bench_recv++; });
        line("typed memcpy (Flat)", matched(pub) ? us_per_round_trip(M,
            [&] { return pub.send(flat) == rant::SendStatus::Ok; }, pump) : -1.0);
    }
    {
        rant::Publisher<Padded> pub(a, "b/padded");
        rant::Subscriber<Padded> sub(b, "b/padded", [](const Padded&) { g_bench_recv++; });
        line("typed loop (Padded, string<7>)", matched(pub) ? us_per_round_trip(M,
            [&] { return pub.send(padded) == rant::SendStatus::Ok; }, pump) : -1.0);
    }
    {
        rant::Publisher<narrow::Chunk> pub(a, "b/chunk");
        rant::Subscriber<narrow::Chunk> sub(b, "b/chunk", [](const narrow::Chunk&) { g_bench_recv++; });
        line("typed tails (Chunk, f32[] string)", matched(pub) ? us_per_round_trip(M,
            [&] { return pub.send(chunk) == rant::SendStatus::Ok; }, pump) : -1.0);
    }
    {
        rant::Publisher<rant::Bytes>  pub(a, "b/dyn", ps);
        rant::Subscriber<rant::Bytes> sub(b, "b/dyn", ps, [&](const rant::MessageView& v) {
            sink += (size_t)(v.get_uint("a") + v.get_uint("b") + v.get_uint("c") + (uint64_t)v.get_f64("d") + v.get_string("tag").size());
            g_bench_recv++;
        });
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
    std::string err;
    auto schema = rant::Schema::compile(SCHEMA, &err);
    if (!schema) { std::printf("FAIL: schema: %s\n", err.c_str()); return 1; }
    std::printf("schema '%.*s' size=%u fields=%u\n",
                (int)schema->name().size(), schema->name().data(), schema->size(), schema->field_count());

    rant::NodeOptions opts;
    opts.domain = 42;
    opts.multicast_interface = "127.0.0.1";   /* single-host discovery */

    auto on_msg = [](const rant::MessageView& m) { decode(m); };
    auto on_evt = [](const char* tag) {
        return [tag](const rant::Event& e) { std::printf("event(%s): %s\n", tag, e.to_string().c_str()); };
    };

    rant::Node a("A", {}, on_evt("A"), opts);
    rant::Node b("B", {}, on_evt("B"), opts);
    if (!a.valid() || !b.valid()) { std::printf("FAIL: node construction\n"); return 1; }

    rant::Publisher<rant::Bytes>  pub(a, "t", &*schema, { rant::Reliability::Reliable });
    rant::Subscriber<rant::Bytes> sub(b, "t", &*schema, on_msg, { rant::Reliability::Reliable });
    if (!pub.valid() || !sub.valid()) { std::printf("FAIL: topic construction\n"); return 1; }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    uint32_t seq = 0;
    while (!g.received && std::chrono::steady_clock::now() < deadline) {
        if (pub.match_count() > 0 && !send_one(pub, *schema, ++seq)) return 1;
        a.poll(1);
        b.poll(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!g.received) { std::printf("FAIL: no message within timeout (match_count=%d)\n", pub.match_count()); return 1; }

    std::printf("B received seq=%u, checking fields:\n", g.seq);
    bool dyn_ok = verify_dynamic();
    std::printf("%s\n", dyn_ok ? "PASS: variable kinds crossed and decoded" : "FAIL: decoded value mismatch");

    /* threaded mode: both nodes on their service threads, no poll() from us */
    if (!a.start() || !b.start()) { std::printf("FAIL: start\n"); return 3; }
    if (a.poll(0) != (int)rant::SendStatus::State) { std::printf("FAIL: poll not refused while started\n"); return 3; }
    g.received = false;
    if (!send_one(pub, *schema, ++seq)) { std::printf("FAIL: threaded send\n"); return 3; }
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!g.received && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!g.received || g.note != NOTE) { std::printf("FAIL: threaded delivery\n"); return 3; }
    a.stop(); b.stop();
    std::printf("PASS: threaded delivery via start() (evicted_unsent=%u)\n", a.stats().evicted_unsent);

    /* patterns + typed codec leg */
    std::printf("patterns leg:\n");
    bool pat_ok = patterns_leg();
    std::printf("%s\n", pat_ok ? "PASS: patterns + typed codec" : "FAIL: patterns leg");

    /* bare-type roots: the type itself is the schema */
    std::printf("value-root leg:\n");
    bool root_ok = value_root_leg();
    std::printf("%s\n", root_ok ? "PASS: bare-type roots" : "FAIL: value-root leg");

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

#if defined(__cpp_exceptions)
    /* a failed constructor throws rant::Error (on_event is required) */
    bool caught = false;
    try { rant::Node bad("bad", {}, {}); }
    catch (const rant::Error&) { caught = true; }
    chk("ctor throws rant::Error without on_event", caught);
#endif

    std::printf("%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 2;
#if defined(__cpp_exceptions)
  } catch (const rant::Error& e) {
    std::printf("FAIL: unexpected rant::Error: %s\n", e.what());
    return 4;
  }
#endif
}
