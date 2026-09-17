/* A minimal chat node over rant.hpp. Run two copies
 * with a name each and type lines. docs/cpp.md has the build lines. */
#include "rant.hpp"

#include <cstdio>
#include <string>

/* A tiny typed schema pasted by every node on the topic. Timestamp and Color are standard
 * types (docs/stdtypes.md), so their names narrow matching and need no definition. */
static const char CHAT_SCHEMA[] =
    "Chat"
    "{"
    "    ts:      Timestamp,"     /* Unix epoch microseconds, UTC */
    "    seq:     u32,"
    "    tint:    Color,"         /* sRGB RGBA bytes */
    "    textLen: u16,"
    "    text:    u8[240]"
    "}";

/* Print a delivered message: decode it through its schema if typed, else raw. */
static void handle_message(const rant::MessageView& m) {
    if (m.has_schema()) {
        rant::Bytes text = m.get_array("text");
        uint64_t n = m.get_uint("textLen");
        int64_t ts = m.get_int("ts");        /* a Timestamp: Unix epoch microseconds */
        if (n > text.size()) n = text.size();
        /* both clocks count the same microseconds, so this is one way latency plus
           clock skew: meaningful on one host, skew bound across machines */
        std::printf("[%.*s] %.*s > %.*s  (#%llu, +%.2f ms)\n",
                    (int)m.publisher_name().size(),  m.publisher_name().data(),
                    (int)m.topic_name().size(), m.topic_name().data(),
                    (int)n, reinterpret_cast<const char*>(text.data()),
                    (unsigned long long)m.get_uint("seq"),
                    (double)(rant::now().us - ts) / 1000.0);
    } else {
        std::printf("[%.*s] %.*s > %.*s\n",
                    (int)m.publisher_name().size(),  m.publisher_name().data(),
                    (int)m.topic_name().size(), m.topic_name().data(),
                    (int)m.text().size(),         m.text().data());
    }
}

/* Print every event on one line. */
static void handle_event(const rant::Event& e) {
    std::printf("  <event> %s\n", e.to_string().c_str());
}

int main(int argc, char** argv) {
    const char* name = (argc > 1) ? argv[1] : nullptr;

    std::string err;
    auto schema = rant::Schema::compile(CHAT_SCHEMA, &err);
    if (!schema) { std::fprintf(stderr, "schema: %s\n", err.c_str()); return 1; }

#if defined(__cpp_exceptions)
  try {
#endif
    rant::Node node(name ? name : std::string_view{}, {}, handle_event);
    if (!node.valid()) { std::fprintf(stderr, "node: %s\n", node.last_error().c_str()); return 1; }

    /* a publisher and a subscriber on the same name share one topic slot */
    rant::Qos reliable{ rant::Reliability::Reliable };
    rant::Publisher<rant::Bytes>  chat(node, "chat", &*schema, reliable);
    rant::Subscriber<rant::Bytes> incoming(node, "chat", &*schema, handle_message, reliable);
    node.start();   /* background poll thread. Sends and creates are thread safe now */

    std::printf("typed chat on topic 'chat'. type a line to publish; ctrl-d/z to quit.\n");

    char line[256];
    uint32_t seq = 0;
    while (std::fgets(line, sizeof line, stdin)) {
        size_t len = std::strcspn(line, "\n");
        if (!len) continue;
        if (len > 240) len = 240;
        rant::Color tint = rant::color_from_hex(0x3080C0FFu);       /* 0xRRGGBBAA */
        rant::MessageBuilder msg(*schema);
        msg.set_int("ts", rant::now().us)
           .set_uint("seq", ++seq)
           .set_uint("tint.r", tint.r).set_uint("tint.g", tint.g)
           .set_uint("tint.b", tint.b).set_uint("tint.a", tint.a)
           .set_uint("textLen", (uint64_t)len)
           .set_array("text", rant::Bytes(line, len));
        if (chat.send(msg) != rant::SendStatus::Ok)
            std::printf("  (send failed)\n");
    }

    return 0;   /* ~Node stops the poller and closes with a BYE */
#if defined(__cpp_exceptions)
  } catch (const rant::Error& e) {
    std::fprintf(stderr, "rant error: %s\n", e.what());
    return 1;
  }
#endif
}
