/* The web build's benchmark: one program built natively and to wasm, so both run the same
 * Rant code. One side echoes, the other measures.
 *
 *   rant_web_bench echo [--domain N] [--reliable] [--if IP] [--events]
 *   rant_web_bench ping [--domain N] [--reliable] [--size B] [--count N] [--rate HZ] [--window W]
 *   rant_web_bench idle [--domain N] [--seconds S]
 *
 * ping sends count pings of size bytes to bench/ping, at rate per second, or with window
 * set keeping that many unanswered, and prints one JSON line of what came back on
 * bench/pong. idle holds a node on the mesh for seconds and prints what it took. --events
 * prints every node event to stderr. */

#include "rant.hpp"

#ifdef __EMSCRIPTEN__
#include "rant_web.h"
#include <emscripten/emscripten.h>

/* Node.js reports the process's CPU time, a page does not, so a browser measures it outside. */
EM_JS(double, process_cpu_ms, (), {
    if (typeof process === "undefined" || !process.cpuUsage) return -1;
    const u = process.cpuUsage();
    return (u.user + u.system) / 1000;
});
#else
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#endif
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

uint64_t now_us()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* The process's CPU time, for the native build. A page's comes from the browser instead. */
double cpu_ms()
{
#if defined(__EMSCRIPTEN__)
    return process_cpu_ms();
#elif defined(_WIN32)
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto ms = [](FILETIME f) { return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e4; };
    return ms(k) + ms(u);
#else
    rusage r;
    getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec * 1e3 + r.ru_utime.tv_usec / 1e3 + r.ru_stime.tv_sec * 1e3 + r.ru_stime.tv_usec / 1e3;
#endif
}

struct Args {
    std::string mode;
    uint16_t    domain = 160;
    bool        reliable = false;
    size_t      size = 64;
    int         count = 1000;
    int         rate = 100;
    int         window = 0;
    int         seconds = 10;
    std::string iface;          /* multicast_interface, empty for every interface */
    bool        events = false;
};

Args parse(int argc, char** argv)
{
    Args a;
    if (argc > 1) a.mode = argv[1];
    for (int i = 2; i < argc; i++) {
        std::string k = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : "0";
        if (k == "--reliable") a.reliable = true;
        else if (k == "--domain") { a.domain = (uint16_t)std::atoi(v); i++; }
        else if (k == "--size") { a.size = (size_t)std::atoi(v); i++; }
        else if (k == "--count") { a.count = std::atoi(v); i++; }
        else if (k == "--rate") { a.rate = std::atoi(v); i++; }
        else if (k == "--window") { a.window = std::atoi(v); i++; }
        else if (k == "--seconds") { a.seconds = std::atoi(v); i++; }
        else if (k == "--if") { a.iface = v; i++; }
        else if (k == "--events") a.events = true;
    }
    if (a.size < 16) a.size = 16;
    return a;
}

rant::NodeOptions options(const Args& a, const char* role)
{
    rant::NodeOptions o;
    o.threading = rant::Threading::Manual;
    o.domain = a.domain;
    o.max_topics = 16;
    o.match_wait = std::chrono::milliseconds(-1);   /* a page cannot block, so neither side does */
    o.multicast_interface = a.iface;
    o.disable_shm = true;     /* a page has no shared memory, so no side of a comparison uses it */
    o.recv_buffer_bytes = 8u << 20;
    o.send_buffer_bytes = 8u << 20;
    (void)role;
    return o;
}

void watch(rant::Node& node, const Args& a)
{
    if (a.events) node.on_event([](const rant::Event& e) { std::fprintf(stderr, "event: %s\n", e.to_string().c_str()); });
}

rant::Qos qos(const Args& a)
{
    rant::Qos q;
    q.reliability = a.reliable ? rant::Reliability::Reliable : rant::Reliability::BestEffort;
    q.keep_last = 64;     /* deeper than any window, so a burst evicts nothing unacked */
    if (a.size > 1350) q.max_message_bytes = (uint32_t)a.size;
    q.queue_bytes = 8u << 20;
    return q;
}

/* Answers every ping with the same bytes on bench/pong. */
struct Echo {
    rant::Node node;
    rant::Publisher<rant::Bytes> pong;
    rant::Subscriber<rant::Bytes> ping;
    std::vector<std::vector<uint8_t>> backlog;
    uint64_t echoed = 0, deferred = 0, report_us = 0;
    bool finished = false;      /* an echo runs until it is stopped */

    explicit Echo(const Args& a) : node("bench-echo", options(a, "echo"))
    {
        watch(node, a);
        pong = node.publisher<rant::Bytes>("bench/pong", qos(a));
        ping = node.subscriber<rant::Bytes>("bench/ping", [this](const rant::MessageView& m) {
            if (pong.send(m.data()) != rant::SendStatus::Ok) { backlog.push_back(m.data().to_vector()); deferred++; }
            echoed++;
        }, qos(a));
    }

    /* Sends what a callback could not, returns the ms until it wants to run again. */
    int step()
    {
        for (auto it = backlog.begin(); it != backlog.end();) {
            if (pong.send(rant::Bytes(*it)) != rant::SendStatus::Ok) break;
            it = backlog.erase(it);
        }
        uint64_t t = now_us();
        if (t - report_us > 2000000u && echoed) {
            report_us = t;
            std::fprintf(stderr, "echo: %llu echoed, %llu deferred, %zu waiting\n",
                         (unsigned long long)echoed, (unsigned long long)deferred, backlog.size());
        }
        return backlog.empty() ? 1000 : 1;
    }
};

/* Sends pings and times their echoes. */
struct Ping {
    Args a;
    rant::Node node;
    rant::Publisher<rant::Bytes> ping;
    rant::Subscriber<rant::Bytes> pong;
    std::vector<uint8_t> payload;
    std::vector<uint32_t> rtt;
    std::vector<bool> seen;
    uint64_t opened_us = 0, matched_us = 0, start_us = 0, last_send_us = 0, done_us = 0, next_us = 0;
    double cpu_start = 0;
    int sent = 0, got = 0, late = 0;
    bool finished = false;

    explicit Ping(const Args& args) : a(args), node("bench-ping", options(args, "ping")), payload(args.size, 0x5a)
    {
        watch(node, a);
        ping = node.publisher<rant::Bytes>("bench/ping", qos(a));
        pong = node.subscriber<rant::Bytes>("bench/pong", [this](const rant::MessageView& m) { heard(m); }, qos(a));
        rtt.reserve((size_t)a.count);
        seen.assign((size_t)a.count, false);
        opened_us = now_us();
    }

    void heard(const rant::MessageView& m)
    {
        rant::Bytes d = m.data();
        if (d.size() < 16) return;
        uint64_t sent_at;
        std::memcpy(&sent_at, d.data() + 8, 8);
        if (sent_at < start_us) { late++; return; }   /* an echo from before this run */
        rtt.push_back((uint32_t)(now_us() - sent_at));
        uint64_t seq;
        std::memcpy(&seq, d.data(), 8);
        if (seq < seen.size()) seen[seq] = true;
        got++;
    }

    bool send_one(uint64_t t)
    {
        uint64_t seq = (uint64_t)sent;
        std::memcpy(payload.data(), &seq, 8);
        std::memcpy(payload.data() + 8, &t, 8);
        if (ping.send(rant::Bytes(payload)) != rant::SendStatus::Ok) return false;
        sent++;
        last_send_us = t;
        return true;
    }

    /* Sends what is due and returns the ms until it wants to run again, -1 once finished. */
    int step()
    {
        uint64_t t = now_us();
        if (!start_us) {
            /* the echo subscribed, then half a second for its pong side to match us. A
               subscriber's match count counts subscribers, so pong cannot tell */
            if (!matched_us && ping.match_count() > 0) matched_us = t;
            if (!matched_us) return t - opened_us > 20000000 ? finish(t, "no echo node matched in 20 s") : 50;
            if (t - matched_us < 500000) return 50;
            start_us = next_us = t;
            cpu_start = cpu_ms();
            std::printf("{\"phase\":\"start\"}\n");   /* a browser harness starts its CPU count here */
            std::fflush(stdout);
        }
        int before = sent;
        if (a.window > 0) {
            while (sent < a.count && sent - got < a.window && send_one(t)) {}
        } else {
            while (sent < a.count && t >= next_us && send_one(t)) next_us += 1000000u / (uint64_t)std::max(a.rate, 1);
        }
        /* a send between polls waits for the next pass, so one goes now */
        if (sent != before) node.poll(std::chrono::milliseconds(0));
        bool all_sent = sent >= a.count;
        if (all_sent && (got >= a.count || t - last_send_us > 2000000)) return finish(t, nullptr);
        if (all_sent || a.window > 0) return 50;
        return (int)std::max<int64_t>(0, (int64_t)(next_us - t) / 1000);
    }

    int finish(uint64_t t, const char* error)
    {
        finished = true;
        done_us = t;
        std::sort(rtt.begin(), rtt.end());
        auto pct = [&](double p) { return rtt.empty() ? 0u : rtt[std::min(rtt.size() - 1, (size_t)(p * rtt.size()))]; };
        double mean = 0;
        for (uint32_t r : rtt) mean += r;
        if (!rtt.empty()) mean /= rtt.size();
        double secs = start_us ? (done_us - start_us) / 1e6 : 0;
        double cpu = cpu_start >= 0 && start_us ? cpu_ms() - cpu_start : -1;
        std::string lost;     /* the first few pings with no echo, to tell a warm up from steady loss */
        for (int i = 0, n = 0; i < sent && n < 12; i++)
            if (!seen[(size_t)i]) { lost += (lost.empty() ? "" : ",") + std::to_string(i); n++; }
        std::printf("{\"mode\":\"ping\",\"reliable\":%s,\"size\":%zu,\"count\":%d,\"rate\":%d,\"window\":%d,"
                    "\"sent\":%d,\"got\":%d,\"late\":%d,\"p50_us\":%u,\"p90_us\":%u,\"p99_us\":%u,\"max_us\":%u,"
                    "\"mean_us\":%.0f,\"seconds\":%.3f,\"msgs_per_s\":%.0f,\"cpu_ms\":%.1f,\"lost\":[%s]%s%s%s}\n",
                    a.reliable ? "true" : "false", a.size, a.count, a.rate, a.window, sent, got, late,
                    pct(0.5), pct(0.9), pct(0.99), rtt.empty() ? 0u : rtt.back(), mean, secs,
                    secs > 0 ? got / secs : 0, cpu, lost.c_str(), error ? ",\"error\":\"" : "", error ? error : "", error ? "\"" : "");
        std::fflush(stdout);
        return -1;
    }
};

/* A node with one subscriber held on the mesh, to see what being a node costs at rest. */
struct Idle {
    Args a;
    rant::Node node;
    rant::Subscriber<rant::Bytes> sub;
    uint64_t start_us;
    double cpu_start;
    bool finished = false;

    explicit Idle(const Args& args) : a(args), node("bench-idle", options(args, "idle")), start_us(now_us()), cpu_start(cpu_ms())
    {
        sub = node.subscriber<rant::Bytes>("bench/idle", [](const rant::MessageView&) {}, qos(a));
        std::printf("{\"phase\":\"start\"}\n");
        std::fflush(stdout);
    }

    int step()
    {
        uint64_t t = now_us();
        if (t - start_us < (uint64_t)a.seconds * 1000000u) return 1000;
        finished = true;
        size_t peers = node.reflection().peers().size();
        double cpu = cpu_start >= 0 ? cpu_ms() - cpu_start : -1;
        std::printf("{\"mode\":\"idle\",\"seconds\":%d,\"peers\":%zu,\"cpu_ms\":%.1f}\n", a.seconds, peers, cpu);
        std::fflush(stdout);
        return -1;
    }
};

template <class B> int run(const Args& a)
{
#ifdef __EMSCRIPTEN__
    /* the page drives: each run polls at once and asks for the next when the bench needs it */
    auto* b = new B(a);
    rant_web_drive([](void* u) {
        auto* b = static_cast<B*>(u);
        if (b->finished) return;
        b->node.poll(std::chrono::milliseconds(60000));
        int ms = b->step();
        if (ms >= 0) rant_web_wake(ms);
    }, b);
    return 0;
#else
    B b(a);
    for (;;) {
        int ms = b.step();
        if (ms < 0) return 0;
        b.node.poll(std::chrono::milliseconds(ms));
    }
#endif
}

} /* namespace */

int main(int argc, char** argv)
{
    Args a = parse(argc, argv);
    if (a.mode == "echo") return run<Echo>(a);
    if (a.mode == "ping") return run<Ping>(a);
    if (a.mode == "idle") return run<Idle>(a);
    std::fprintf(stderr, "usage: rant_web_bench echo|ping|idle [--domain N] [--reliable] [--size B] "
                         "[--count N] [--rate HZ] [--window W] [--seconds S]\n");
    return 2;
}
