/* Minimal C++ node demo over the dart.hpp wrapper. Mirrors examples/example.c
 * but with the OOP API: open a node, declare a typed "chat" topic, run a
 * background poller, and pub/sub short text lines.
 *
 * Run two copies (optionally with a name):  ./example alice   /   ./example bob
 * Type a line to publish it; lines from peers print as they arrive. Ctrl-D/Z quits.
 *
 * Build (pure C++: the implementation anchor is dart_impl.cpp, no C compiler):
 *   Windows (MinGW):
 *     g++ -std=c++17 -Icpp -Idist cpp/example.cpp cpp/dart_impl.cpp \
 *         -o example.exe -lws2_32 -lbcrypt -lwinmm
 *   POSIX:
 *     g++ -std=c++17 -Icpp -Idist cpp/example.cpp cpp/dart_impl.cpp \
 *         -o example -lrt -lpthread
 * (Consumers of the shipped dist/dart.hpp need only -Idist and that one file.)
 */
#include "dart.hpp"

#include <cstdio>
#include <string>

/* A tiny typed schema so deliveries decode through DartMsg.schema. Every node
 * that speaks this topic pastes the identical text. */
static const char CHAT_SCHEMA[] =
    "Chat"
    "{"
    "    ts:      u64,"
    "    seq:     u32,"
    "    textLen: u16,"
    "    text:    u8[240]"
    "}";

/* Print a delivered message: decode it through its schema if typed, else raw. */
static void handle_message(const dart::MessageIn& m) {
    if (m.has_schema()) {
        dart::Bytes text = m.get_array("text");
        uint64_t n = m.get_uint("textLen");
        if (n > text.size()) n = text.size();
        std::printf("[%.*s] %.*s > %.*s  (#%llu)\n",
                    (int)m.publisher_name().size(),  m.publisher_name().data(),
                    (int)m.topic_name().size(), m.topic_name().data(),
                    (int)n, reinterpret_cast<const char*>(text.data()),
                    (unsigned long long)m.get_uint("seq"));
    } else {
        std::printf("[%.*s] %.*s > %.*s\n",
                    (int)m.publisher_name().size(),  m.publisher_name().data(),
                    (int)m.topic_name().size(), m.topic_name().data(),
                    (int)m.text().size(),         m.text().data());
    }
}

/* Print every event on one line. */
static void handle_event(const dart::Event& e) {
    std::printf("  <event> %s\n", e.to_string().c_str());
}

int main(int argc, char** argv) {
    const char* name = (argc > 1) ? argv[1] : nullptr;

    std::string err;
    auto schema = dart::Schema::compile(CHAT_SCHEMA, &err);
    if (!schema) { std::fprintf(stderr, "schema: %s\n", err.c_str()); return 1; }

    auto node = dart::Node::open(name ? name : std::string_view{}, handle_message, handle_event);
    if (!node) { std::fprintf(stderr, "dart_node_open failed\n"); return 1; }

    auto chat = node->create_topic("chat", dart::Role::PubSub, &*schema,
                                     { dart::Reliability::Reliable });
    node->start();   /* background poll thread; sends/creates are now thread-safe */

    std::printf("typed chat on topic 'chat'. type a line to publish; ctrl-d/z to quit.\n");

    char line[256];
    uint32_t seq = 0;
    while (std::fgets(line, sizeof line, stdin)) {
        size_t len = std::strcspn(line, "\n");
        if (!len) continue;
        if (len > 240) len = 240;
        dart::MessageOut msg(*schema);
        msg.set_uint("seq", ++seq)
           .set_uint("textLen", (uint64_t)len)
           .set_array("text", dart::Bytes(line, len));
        if (chat.send(msg) != dart::SendStatus::Ok)
            std::printf("  (send failed)\n");
    }

    return 0;   /* ~Node stops the poller and closes with a BYE */
}
