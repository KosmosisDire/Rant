/* Two-node test for dart.hpp. Leg 1: node A publishes a typed message through the
 * dynamic Schema/MessageBuilder API, node B receives and decodes it (v4 variable
 * kinds: capped + variable string, variable scalar array, map). Leg 2: the same
 * over service threads. Leg 3: the patterns layer (typed functions incl. blocking /
 * async / fail / defer, variables, signals, typed pub/sub over DART_SCHEMA with the
 * memcpy, loop, and subset/rebase codec paths, entity reflection). Single process,
 * discovery pinned to loopback on isolated domains (never 0). Exit 0 = PASS. */
#include "dart.hpp"
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
                     dart::Node* pump = nullptr) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        if (pump) pump->poll(2);
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

/* ---- leg 1 + 2: the dynamic schema API (renamed surface) ------------------------- */

static const char SCHEMA[] =
    "Sensor{ seq: u32, name: string<16>, note: string, samples: f32[], extras: map }";

static const char NOTE[]    = "a long unbounded note well over sixteen bytes";
static const float SAMPLES[] = { 1.5f, -2.25f, 3.75f };

/* build + send one message; returns false if a setter was refused or the send failed */
static bool send_one(dart::Topic& pub, const dart::Schema& schema, uint32_t seq) {
    dart::MessageBuilder s(schema);
    dart::MapWriter mw;
    mw.put_uint("battery", 87).put_int("signed", -5)
      .put_string("state", "docked").put_bool("ok", true);
    mw.open_array("temps"); mw.put_f64(nullptr, 36.2).put_f64(nullptr, 34.9); mw.close();
    mw.open_map("meta");    mw.put_string("fw", "1.2.3").put_uint("rev", 7);   mw.close();

    s.set_uint("seq", seq)
     .set_string("name", "lidar")
     .set_string("note", NOTE)
     .set_array("samples", dart::Bytes(SAMPLES, sizeof SAMPLES))
     .set_map("extras", mw);
    if (!s.ok() || !mw.ok()) { std::printf("FAIL: encode (ok=%d mw=%d)\n", s.ok(), mw.ok()); return false; }
    return pub.send(s) == dart::SendStatus::Ok;
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

static void decode(const dart::MessageView& m) {
    g.seq  = (uint32_t)m.get_uint("seq");
    g.name = std::string(m.get_string("name"));
    g.note = std::string(m.get_string("note"));
    auto sm = m.get_array("samples");
    g.n_samples = sm.size() / sizeof(float);
    if (g.n_samples > 3) g.n_samples = 3;
    std::memcpy(g.samples, sm.data(), g.n_samples * sizeof(float));
    auto map = m.get_map("extras");
    dart::MapValue v;
    if (map.get("battery", v)) g.battery = v.as_uint();
    if (map.get("signed",  v)) g.sgn     = v.as_int();
    if (map.get("ok",      v)) g.ok_flag = v.as_bool();
    if (map.get("state",   v)) g.state   = std::string(v.as_string());
    if (map.get("temps",   v)) { g.n_temps = v.array_count(); if (g.n_temps) g.temp0 = v.array_at(0).as_f64(); }
    if (map.get("meta",    v)) {
        auto meta = v.as_map();
        dart::MapValue mv;
        if (meta.get("fw",  mv)) g.meta_fw  = std::string(mv.as_string());
        if (meta.get("rev", mv)) g.meta_rev = mv.as_uint();
    }

    /* std::map readback: decode the whole map into an owning tree that outlives the handler */
    dart::MapDict d = m.get_map("extras").to_map();
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

/* ---- leg 3: typed schemas for the patterns layer --------------------------------- */

struct AddReq { int32_t x; int32_t y; };
DART_SCHEMA(AddReq, x, y);
struct AddRsp { int64_t sum; };
DART_SCHEMA(AddRsp, sum);
struct Speed { int32_t v; };
DART_SCHEMA(Speed, v);
struct Alarm { uint32_t code; };
DART_SCHEMA(Alarm, code);

/* padding-free: offsets == wire offsets, sizeof == wire size -> memcpy path */
struct Flat { uint32_t a; float b; };
DART_SCHEMA(Flat, a, b);
/* padded (compiler inserts gaps; wire is packed) + a capped string -> loop path */
struct Padded {
    uint8_t         a;
    uint32_t        b;
    uint16_t        c;
    double          d;
    dart::String<7> tag;
};
DART_SCHEMA(Padded, a, b, c, d, tag);
static_assert(sizeof(Flat) == 8, "Flat must be padding-free for the memcpy-path leg");
static_assert(sizeof(Padded) > 1 + 4 + 2 + 8 + 9, "Padded must carry padding for the loop-path leg");

/* same wire name ("Telemetry"), different field sets: the subscriber's schema is a
 * SUBSET of the publisher's in a different order, so the hashes differ and the
 * delivery decodes through the subset/rebase path. */
namespace pubside { struct Telemetry { uint32_t seq; float volts; uint16_t flags; }; }
DART_SCHEMA(pubside::Telemetry, seq, volts, flags);
namespace subside { struct Telemetry { uint16_t flags; uint32_t seq; }; }
DART_SCHEMA(subside::Telemetry, flags, seq);

static dart::Deferred<AddRsp> g_deferred;
static std::mutex             g_deferred_mu;
static std::atomic<bool>      g_deferred_ready{ false };

static bool patterns_leg() {
    int fails_at_entry = g_failures;
    dart::NodeOptions opts;
    opts.domain = 43;
    opts.multicast_interface = "127.0.0.1";
    opts.max_topics = 32;
    opts.fetch_details = true;   /* observer behavior: peer entity NAMES resolve (else hash placeholders) */

    auto on_evt = [](const char* tag) {
        return [tag](const dart::Event& e) {
            if (e.is_error())
                std::printf("event(%s): %s\n", tag, e.to_string().c_str());
        };
    };

    dart::Node a("PA", {}, on_evt("PA"), opts);
    dart::Node b("PB", {}, on_evt("PB"), opts);
    chk("patterns: nodes constructed", a.valid() && b.valid());
    if (!a.valid() || !b.valid()) return false;

    /* A runs on its service thread; B is pumped from this thread (so the blocking
     * call/wait forms, which drive the caller's loop themselves, are exercised). */
    chk("patterns: A started", a.start());

    /* ---- functions ---- */
    dart::FunctionDefinition<AddReq, AddRsp> def_add(a, "add",
        [](const AddReq& q) { return AddRsp{ (int64_t)q.x + q.y }; });
    dart::FunctionDefinition<AddReq, AddRsp> def_chk(a, "chk",
        [](const AddReq& q, dart::Request<AddRsp>& rq) {
            if (q.x < 0) rq.fail();
            else         rq.reply(AddRsp{ (int64_t)q.x * q.y });
        });
    dart::FunctionDefinition<AddReq, AddRsp> def_defer(a, "defr",
        [](const AddReq& q, dart::Request<AddRsp>& rq) {
            (void)q;
            std::lock_guard<std::mutex> g(g_deferred_mu);
            g_deferred = rq.defer();
            g_deferred_ready = true;
        });
    chk("patterns: definitions created", def_add.valid() && def_chk.valid() && def_defer.valid());

    dart::RemoteFunction<AddReq, AddRsp> rf_add(b, "add");
    dart::RemoteFunction<AddReq, AddRsp> rf_chk(b, "chk");
    dart::RemoteFunction<AddReq, AddRsp> rf_defer(b, "defr");
    chk("patterns: remotes created", rf_add.valid() && rf_chk.valid() && rf_defer.valid());

    chk("patterns: definition discovered", wait_for(4000,
        [&] { return rf_add.has_definition() && rf_chk.has_definition() && rf_defer.has_definition(); }, &b));
    /* settle B so the rsp lanes (A pub -> B sub) are also formed before the first
     * blocking call; has_definition only proves the req direction */
    chk("patterns: settle", b.settle(4000));

    /* blocking round trip (memcpy-path structs both ways) */
    auto r1 = rf_add.call(AddReq{ 20, 22 }, 3000);
    chk("patterns: blocking call Ok 20+22=42",
        r1.ok() && r1->sum == 42 && r1.provider() != 0);

    /* full-form handler replying */
    auto r2 = rf_chk.call(AddReq{ 6, 7 }, 3000);
    chk("patterns: full-form reply 6*7=42", r2.ok() && r2->sum == 42);

    /* full-form handler failing -> AppError */
    auto r3 = rf_chk.call(AddReq{ -1, 0 }, 3000);
    chk("patterns: fail() -> AppError", !r3.ok() && r3.status() == dart::CallStatus::AppError);

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
        [&](const dart::ResponseView<AddRsp>& rv) {
            if (rv.ok()) async_sum = rv->sum;
            async_done = true;
        });
    chk("patterns: call_async accepted", st == dart::SendStatus::Ok);
    chk("patterns: async 1+2=3", wait_for(3000, [&] { return async_done.load(); }, &b)
        && async_sum == 3);

    chk("patterns: caller_count >= 1", def_add.caller_count() >= 1);

    /* ---- variables ---- */
    dart::VariableOptions<Speed> vo;
    vo.initial = Speed{ 7 };
    vo.allow_force = true;
    dart::VariableDefinition<Speed> vd(a, "speed", vo);
    dart::RemoteVariable<Speed>     rv(b, "speed");
    chk("var: created", vd.valid() && rv.valid());
    chk("var: remote wait() gets a value", rv.wait(4000));
    { auto v = rv.get(); chk("var: initial 7 replicated", v && v->v == 7); }
    chk("var: remote set accepted", rv.set(Speed{ 25 }) == dart::SendStatus::Ok);
    chk("var: set converges at definition", wait_for(3000,
        [&] { auto v = vd.get(); return v && v->v == 25; }, &b));
    chk("var: force accepted", vd.force(Speed{ 99 }) == dart::SendStatus::Ok);
    chk("var: forced value replicated", wait_for(3000,
        [&] { auto v = rv.get(); return rv.forced() && v && v->v == 99; }, &b));
    chk("var: set absorbed while forced", vd.set(Speed{ 50 }) == dart::SendStatus::Ok);
    chk("var: unforce accepted", vd.unforce() == dart::SendStatus::Ok);
    chk("var: unforce restores absorbed set", wait_for(3000,
        [&] { auto v = rv.get(); return !rv.forced() && v && v->v == 50; }, &b));
    chk("var: has_definition/remote_count", rv.has_definition() && vd.remote_count() >= 1);

    /* variable events: on_change replays + dedups, on_write counts every write */
    std::atomic<int> vchg{ 0 }, vwr{ 0 }; std::atomic<int64_t> vlast{ 0 };
    vd.on_change([&](const Speed& s, const dart::VariableUpdate& u) { (void)u; vchg++; vlast = s.v; });
    chk("var: on_change replays current at registration", vchg.load() == 1 && vlast.load() == 50);
    vd.on_write([&](const Speed& s) { (void)s; vwr++; });
    chk("var: on_write does not replay", vwr.load() == 0);
    chk("var: identical re-set accepted", vd.set(Speed{ 50 }) == dart::SendStatus::Ok);
    chk("var: identical re-set is a write, not a change", vchg.load() == 1 && vwr.load() == 1);
    chk("var: new set accepted", vd.set(Speed{ 51 }) == dart::SendStatus::Ok);
    chk("var: change fires inline with the new value",
        vchg.load() == 2 && vlast.load() == 51 && vwr.load() == 2);
    std::atomic<int64_t> rlast{ 0 };
    rv.on_change([&](const Speed& s) { rlast = s.v; });
    chk("var: remote change arrives", wait_for(3000, [&] { return rlast.load() == 51; }, &b));
    vd.on_change(nullptr); vd.on_write(nullptr); rv.on_change(nullptr);

    /* ---- signals ---- */
    std::atomic<uint32_t> alarm_code{ 0 };
    dart::Signal<Alarm> emitter(a, "alarm");
    dart::Signal<Alarm> listener(b, "alarm",
        [&](const Alarm& s) { alarm_code = s.code; });
    chk("sig: created", emitter.valid() && listener.valid());
    chk("sig: listener matched", wait_for(4000,
        [&] { return emitter.listener_count() >= 1; }, &b));
    chk("sig: emit accepted", emitter.emit(Alarm{ 123 }) == dart::SendStatus::Ok);
    chk("sig: typed payload received", wait_for(3000,
        [&] { return alarm_code.load() == 123; }, &b));

    /* payload-less signal through the untyped twin */
    std::atomic<int> pings{ 0 };
    dart::Signal<> ping(a, "ping");
    dart::Signal<> pong(b, "ping", nullptr,
        [&](const dart::MessageView& m) { (void)m; pings++; });
    chk("sig: untyped pair created", ping.valid() && pong.valid());
    chk("sig: untyped listener matched", wait_for(4000,
        [&] { return ping.listener_count() >= 1; }, &b));
    chk("sig: payload-less emit", ping.emit() == dart::SendStatus::Ok);
    chk("sig: payload-less received", wait_for(3000,
        [&] { return pings.load() >= 1; }, &b));

    /* ---- typed pub/sub: memcpy path (padding-free struct, identical schemas) ---- */
    dart::Qos rel; rel.reliability = dart::Reliability::Reliable;
    std::atomic<bool> flat_ok{ false };
    dart::Publisher<Flat>  pf(a, "flat", rel);
    dart::Subscriber<Flat> sf(b, "flat",
        [&](const Flat& f) { if (f.a == 7 && f.b == 2.5f) flat_ok = true; }, rel);
    chk("codec: flat pair created", pf.valid() && sf.valid());
    chk("codec: flat matched", wait_for(4000, [&] { return pf.match_count() > 0 && pf.ready(); }, &b));
    chk("codec: flat send", pf.send(Flat{ 7, 2.5f }) == dart::SendStatus::Ok);
    chk("codec: memcpy-path round trip", wait_for(3000, [&] { return flat_ok.load(); }, &b));

    /* ---- typed pub/sub: loop path (padded struct + string) via typed take() ---- */
    dart::Publisher<Padded>  pp(a, "padded", rel);
    dart::Subscriber<Padded> sp(b, "padded", rel);
    chk("codec: padded pair created", pp.valid() && sp.valid());
    (void)sp.take(0);   /* switch to queued delivery before anything arrives */
    chk("codec: padded matched", wait_for(4000, [&] { return pp.match_count() > 0; }, &b));
    Padded out{};
    out.a = 9; out.b = 0x11223344u; out.c = 777; out.d = -3.5;
    chk("codec: string assign", out.tag.assign("robot"));
    chk("codec: string over-cap refused", !out.tag.assign("well over seven"));
    chk("codec: padded send", pp.send(out) == dart::SendStatus::Ok);
    auto tm = sp.take(3000);
    chk("codec: loop-path take round trip", tm.has_value()
        && tm->value().a == 9 && tm->value().b == 0x11223344u
        && tm->value().c == 777 && tm->value().d == -3.5
        && tm->value().tag.view() == "robot");
    chk("codec: taken envelope", tm.has_value() && tm->publisher_name() == "PA"
        && tm->topic_name() == "padded");

    /* ---- typed pub/sub: schema-hash mismatch (subset subscriber, rebase decode) ---- */
    std::atomic<bool> tele_ok{ false };
    dart::Publisher<pubside::Telemetry>  tp(a, "tele", rel);
    dart::Subscriber<subside::Telemetry> ts(b, "tele",
        [&](const subside::Telemetry& t) { if (t.seq == 31337 && t.flags == 5) tele_ok = true; }, rel);
    chk("codec: telemetry pair created", tp.valid() && ts.valid());
    chk("codec: telemetry matched", wait_for(4000, [&] { return tp.match_count() > 0 && tp.ready(); }, &b));
    chk("codec: telemetry send", tp.send(pubside::Telemetry{ 31337, 12.6f, 5 }) == dart::SendStatus::Ok);
    chk("codec: subset/rebase round trip", wait_for(3000, [&] { return tele_ok.load(); }, &b));

    /* ---- reflection: local + peer entities ---- */
    auto find = [](const std::vector<dart::Entity>& es, dart::EntityKind k, const char* nm)
                -> const dart::Entity* {
        for (const auto& e : es) if (e.kind == k && e.name == nm) return &e;
        return nullptr;
    };
    auto local = a.entities();
    chk("reflect: local function folded",  find(local, dart::EntityKind::Function, "add") != nullptr);
    chk("reflect: local variable folded",  find(local, dart::EntityKind::Variable, "speed") != nullptr);
    chk("reflect: local signal folded",    find(local, dart::EntityKind::Signal, "alarm") != nullptr);
    chk("reflect: local topic passes",     find(local, dart::EntityKind::Topic, "flat") != nullptr);
    const dart::Entity* var_ent = find(local, dart::EntityKind::Variable, "speed");
    chk("reflect: variable writable", var_ent && var_ent->writable);

    bool peer_seen = false, peer_fn = false;
    for (const auto& p : b.peers()) {
        if (p.name != "PA") continue;
        peer_seen = true;
        auto es = b.peer_entities(p.id);
        const dart::Entity* e = find(es, dart::EntityKind::Function, "add");
        peer_fn = e && e->provides;
    }
    chk("reflect: peer PA visible", peer_seen);
    chk("reflect: peer function entity provides", peer_fn);

    a.stop();
    return g_failures == fails_at_entry;
}

int main() {
#if defined(__cpp_exceptions)
  try {
#endif
    std::string err;
    auto schema = dart::Schema::compile(SCHEMA, &err);
    if (!schema) { std::printf("FAIL: schema: %s\n", err.c_str()); return 1; }
    std::printf("schema '%.*s' size=%u fields=%u\n",
                (int)schema->name().size(), schema->name().data(), schema->size(), schema->field_count());

    dart::NodeOptions opts;
    opts.domain = 42;
    opts.multicast_interface = "127.0.0.1";   /* single-host discovery */

    auto on_msg = [](const dart::MessageView& m) { decode(m); };
    auto on_evt = [](const char* tag) {
        return [tag](const dart::Event& e) { std::printf("event(%s): %s\n", tag, e.to_string().c_str()); };
    };

    dart::Node a("A", {}, on_evt("A"), opts);
    dart::Node b("B", on_msg, on_evt("B"), opts);
    if (!a.valid() || !b.valid()) { std::printf("FAIL: node construction\n"); return 1; }

    auto pub = dart::Topic(a, "t", dart::Role::PubOnly, &*schema, { dart::Reliability::Reliable });
    auto sub = b.create_topic("t", dart::Role::SubOnly, &*schema, { dart::Reliability::Reliable });
    (void)sub;

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

    /* threaded mode: both nodes on their service threads; send without any poll() from us */
    if (!a.start() || !b.start()) { std::printf("FAIL: start\n"); return 3; }
    if (a.poll(0) != (int)dart::SendStatus::State) { std::printf("FAIL: poll not refused while started\n"); return 3; }
    g.received = false;
    if (!send_one(pub, *schema, ++seq)) { std::printf("FAIL: threaded send\n"); return 3; }
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!g.received && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!g.received || g.note != NOTE) { std::printf("FAIL: threaded delivery\n"); return 3; }
    a.stop(); b.stop();
    std::printf("PASS: threaded delivery via start() (evicted_unsent=%u)\n", a.evicted_unsent());

    /* patterns + typed codec leg */
    std::printf("patterns leg:\n");
    bool pat_ok = patterns_leg();
    std::printf("%s\n", pat_ok ? "PASS: patterns + typed codec" : "FAIL: patterns leg");

#if defined(__cpp_exceptions)
    /* a failed constructor throws dart::Error (on_event is required) */
    bool caught = false;
    try { dart::Node bad("bad", {}, {}); }
    catch (const dart::Error&) { caught = true; }
    chk("ctor throws dart::Error without on_event", caught);
#endif

    std::printf("%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 2;
#if defined(__cpp_exceptions)
  } catch (const dart::Error& e) {
    std::printf("FAIL: unexpected dart::Error: %s\n", e.what());
    return 4;
  }
#endif
}
