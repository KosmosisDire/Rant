/* Two-node test for dart.hpp: node A publishes a typed message, node B
 * receives and decodes it. Exercises the v4 variable kinds (capped + variable
 * string, variable scalar array, and a map). Single process, discovery pinned to
 * loopback. Exit 0 = PASS. */
#include "dart.hpp"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

static const char SCHEMA[] =
    "Sensor{ seq: u32, name: string<16>, note: string, samples: f32[], extras: map }";

static const char NOTE[]    = "a long unbounded note well over sixteen bytes";
static const float SAMPLES[] = { 1.5f, -2.25f, 3.75f };

/* build + send one message; returns false if a setter was refused or the send failed */
static bool send_one(dart::Channel& pub, const dart::Schema& schema, uint32_t seq) {
    dart::MessageOut s(schema);
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

static void decode(const dart::MessageIn& m) {
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

static bool verify() {
    bool ok = true;
    auto chk = [&](const char* n, bool c){ std::printf("  %s  %s\n", c ? "ok " : "FAIL", n); ok &= c; };
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
    return ok;
}

int main() {
    std::string err;
    auto schema = dart::Schema::compile(SCHEMA, &err);
    if (!schema) { std::printf("FAIL: schema: %s\n", err.c_str()); return 1; }
    std::printf("schema '%.*s' size=%u fields=%u\n",
                (int)schema->name().size(), schema->name().data(), schema->size(), schema->field_count());

    dart::NodeOptions opts;
    opts.domain = 42;
    opts.multicast_interface = "127.0.0.1";   /* single-host discovery */

    auto on_msg = [](const dart::MessageIn& m) { decode(m); };
    auto on_evt = [](const char* tag) {
        return [tag](const dart::Event& e) { std::printf("event(%s): %s\n", tag, e.to_string().c_str()); };
    };

    auto a = dart::Node::open("A", nullptr, on_evt("A"), opts);
    auto b = dart::Node::open("B", on_msg, on_evt("B"), opts);
    if (!a || !b) { std::printf("FAIL: open\n"); return 1; }

    auto pub = a->create_channel("t", dart::Role::PubOnly, &*schema, { dart::Reliability::Reliable });
    auto sub = b->create_channel("t", dart::Role::SubOnly, &*schema, { dart::Reliability::Reliable });
    (void)sub;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    uint32_t seq = 0;
    while (!g.received && std::chrono::steady_clock::now() < deadline) {
        if (pub.match_count() > 0 && !send_one(pub, *schema, ++seq)) return 1;
        a->poll(1);
        b->poll(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!g.received) { std::printf("FAIL: no message within timeout (match_count=%d)\n", pub.match_count()); return 1; }

    std::printf("B received seq=%u, checking fields:\n", g.seq);
    bool ok = verify();
    std::printf("%s\n", ok ? "PASS: variable kinds crossed and decoded" : "FAIL: decoded value mismatch");

    /* threaded mode: both nodes on their service threads; send without any poll() from us */
    if (!a->start() || !b->start()) { std::printf("FAIL: start\n"); return 3; }
    if (a->poll(0) != (int)dart::SendStatus::State) { std::printf("FAIL: poll not refused while started\n"); return 3; }
    g.received = false;
    if (!send_one(pub, *schema, ++seq)) { std::printf("FAIL: threaded send\n"); return 3; }
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!g.received && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!g.received || g.note != NOTE) { std::printf("FAIL: threaded delivery\n"); return 3; }
    a->stop(); b->stop();
    std::printf("PASS: threaded delivery via start() (evicted_unsent=%u)\n", a->evicted_unsent());

    return ok ? 0 : 2;
}
