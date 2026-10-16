/* The C++ side of an interop pair: `tick pub <seconds>` publishes the Tick schema every
 * language's tick program shares, `tick sub <seconds> <publisher>` exits 0 once twenty
 * ticks from that node decoded right and 1 on a wrong one or when the seconds run out.
 * Loopback, on RANT_DOMAIN. */
#include "rant.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const char TICK_SCHEMA[] = "Tick { seq: u64, when: Timestamp, value: f64, at: Transform }";
static const int TICK_WANT = 20;

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main(int argc, char** argv) {
    bool sub = argc > 1 && std::strcmp(argv[1], "sub") == 0;
    double seconds = argc > 2 ? std::atof(argv[2]) : 10.0;
    std::string from = argc > 3 ? argv[3] : "";
    int good = 0, bad = 0;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    rant::NodeOptions opts;
    opts.multicast_interface = "127.0.0.1";      /* the domain comes from RANT_DOMAIN */
    opts.threading = rant::Threading::Manual;
    rant::Node node(sub ? "cpp-tick-sub" : "cpp-tick-pub", opts);
    rant::Schema schema = node.schema(TICK_SCHEMA);
    if (!node.valid() || !schema) { std::printf("FAIL: node or schema\n"); return 1; }
    rant::Qos qos;
    qos.reliability = rant::Reliability::Reliable;
    qos.keep_last = 64;

    auto on_tick = [&](const rant::MessageView& m) {
        uint64_t seq = m.get_uint("seq");
        double angle = (double)seq * 0.01;
        bool ok = m.has_schema() && m.publisher_name() == from && m.get_int("when") > 0
               && near(m.get_f64("value"), std::sin(angle))
               && near(m.get_f64("at.pose.position.x"), std::cos(angle))
               && near(m.get_f64("at.pose.position.y"), std::sin(angle));
        if (ok) good++;
        else if (bad++ == 0) std::printf("FAIL: tick %llu decoded wrong\n", (unsigned long long)seq);
    };
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    if (sub) {
        auto topic = node.subscriber<rant::Bytes>("tick", on_tick, qos, schema);
        while (std::chrono::steady_clock::now() < deadline && !bad && good < TICK_WANT)
            node.poll(std::chrono::milliseconds(2));
        std::printf("%s: %d ticks decoded right, %d wrong\n",
                    good >= TICK_WANT && !bad ? "PASS" : "FAIL", good, bad);
        return good >= TICK_WANT && !bad ? 0 : 1;
    }
    auto topic = node.publisher<rant::Bytes>("tick", qos, schema);
    for (uint64_t seq = 0; std::chrono::steady_clock::now() < deadline;) {
        if (topic.match_count() > 0) {
            double angle = (double)seq * 0.01;
            rant::MessageBuilder msg(schema);
            msg.set_uint("seq", seq)
               .set_int("when", rant::types::now().us)
               .set_f64("value", std::sin(angle))
               .set_f64("at.pose.position.x", std::cos(angle))
               .set_f64("at.pose.position.y", std::sin(angle));
            topic.send(msg);
            seq++;
        }
        node.poll(std::chrono::milliseconds(2));
    }
    return 0;
}
