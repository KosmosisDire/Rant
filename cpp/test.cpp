/* Two-node test for dart.hpp: node A publishes a typed message, node B
 * receives and decodes it. Single process, discovery pinned to loopback.
 * Exit 0 = PASS. */
#include "dart.hpp"
#include <cstdio>
#include <chrono>
#include <thread>

static const char SCHEMA[] = "Ping{ seq: u32, textLen: u16, text: u8[64] }";

int main() {
    std::string err;
    auto schema = dart::Schema::compile(SCHEMA, &err);
    if (!schema) { std::printf("FAIL: schema: %s\n", err.c_str()); return 1; }
    std::printf("schema '%.*s' size=%u fields=%u\n",
                (int)schema->name().size(), schema->name().data(), schema->size(), schema->field_count());

    dart::NodeOptions opts;
    opts.domain = 42;
    opts.multicast_interface = "127.0.0.1";   /* single-host discovery */

    auto a = dart::Node::open("A", opts);
    auto b = dart::Node::open("B", opts);
    if (!a || !b) { std::printf("FAIL: open\n"); return 1; }

    std::string got_sender, got_text;
    uint32_t    got_seq = 0;
    bool        received = false;
    b->on_message([&](const dart::MessageIn& m) {
        got_sender = std::string(m.sender_name());
        got_seq    = (uint32_t)m.get_uint("seq");
        auto n     = m.get_uint("textLen");
        auto t     = m.get_array("text");
        if (n > t.size()) n = t.size();
        got_text.assign(reinterpret_cast<const char*>(t.data()), (size_t)n);
        received   = true;
    });

    auto pub = a->create_channel("t", dart::Role::PubOnly, &*schema, { dart::Reliability::Reliable });
    auto sub = b->create_channel("t", dart::Role::SubOnly, &*schema, { dart::Reliability::Reliable });
    (void)sub;

    /* pump both nodes until the match forms and a message lands, or we time out */
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    uint32_t seq = 0;
    while (!received && std::chrono::steady_clock::now() < deadline) {
        if (pub.match_count() > 0) {
            dart::MessageOut s(*schema);
            const char* line = "hello from A";
            s.set_uint("seq", ++seq)
             .set_uint("textLen", 12)
             .set_array("text", dart::Bytes(line, 12));
            if (pub.send(s) != dart::SendStatus::Ok) { std::printf("FAIL: send\n"); return 1; }
        }
        a->poll(1);
        b->poll(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (!received) { std::printf("FAIL: no message within timeout (match_count=%d)\n", pub.match_count()); return 1; }

    std::printf("PASS: B received seq=%u sender='%s' text='%s'\n",
                got_seq, got_sender.c_str(), got_text.c_str());
    bool ok = (got_sender == "A") && (got_text == "hello from A");

    auto peers = b->peers();
    std::printf("B sees %zu peer(s):\n", peers.size());
    for (const auto& p : peers) {
        std::printf("  id=%u name='%s' addr=%s active=%d frag=%u topics=%zu\n",
                    p.id, p.name.c_str(), p.address.c_str(), p.active, p.fragment_size, p.topics.size());
    }
    return ok ? 0 : 2;
}
