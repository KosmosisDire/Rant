/* DART C++ wrapper: a small, header-only OOP layer over the C single-header
 * (dart.h). Include this from C++; the raw C API is pulled into a private
 * `dart::detail` namespace so it is not visible at global scope.
 *
 * The shipped dist/dart.hpp is fully self-contained: the C single-header is
 * embedded inside it, so this one file is all a consumer needs.
 *
 * Usage: include "dart.hpp" from your C++ code for the wrapper API, and in
 * exactly ONE translation unit define DART_IMPLEMENTATION before including it
 * to emit the library implementation:
 *
 *     #define DART_IMPLEMENTATION
 *     #include "dart.hpp"          // the one implementation-anchor TU
 *
 * The DART library compiles cleanly as both C and C++, so that anchor may be a
 * .cpp (a pure C++ project needs no C toolchain) or a .c (a .c that just
 * #includes "dart.hpp" is auto-treated as the anchor). The anchor emits only
 * the implementation; use the wrapper from your other TUs.
 *
 *     auto node = dart::Node::open("robot1", { .domain = 7 });   // std::optional
 *     if (!node) return 1;
 *     node->on_message([](const dart::MessageIn& m){
 *         std::printf("%.*s > %.*s\n",
 *             (int)m.sender_name().size(), m.sender_name().data(),
 *             (int)m.text().size(),        m.text().data());
 *     });
 *     auto ch = node->create_channel("chat", dart::Role::PubSub, nullptr,
 *                                    { .reliability = dart::Reliability::Reliable });
 *     for (;;) { node->poll(1); ch.send("hello"); }   // drive it yourself (single-threaded)
 */
#ifndef DART_HPP_INCLUDED
#define DART_HPP_INCLUDED

/* ------------------------------------------------------------------------- *
 *  Implementation dispatch.
 *
 *  dart.h is embedded below EXACTLY ONCE (the packer splices it in for the
 *  shipped dist/dart.hpp; in-tree it resolves through the include path). Where
 *  that single copy lands is chosen at compile time:
 *
 *   - DART_IMPLEMENTATION defined (in one anchor TU, .c OR .cpp): emitted at
 *     global scope, producing the C99 implementation. The library compiles
 *     cleanly as BOTH C and C++, so the anchor may be either -- a pure C++
 *     project needs no C toolchain. dart.h's extern "C" keeps the symbols' C
 *     linkage either way, so the namespaced declarations below link to them.
 *
 *   - a normal C++ consumer TU: emitted inside `namespace dart::detail`,
 *     declarations only, so no raw C symbol reaches global scope; the OOP
 *     wrapper follows. (As with any single-header lib, the one anchor TU emits
 *     only the implementation -- put DART_IMPLEMENTATION in a dedicated TU.)
 *
 *  A .c TU with nothing defined is treated as the implementation anchor. */
#if !defined(DART_IMPLEMENTATION) && !defined(__cplusplus)
#define DART_IMPLEMENTATION
#endif

#if defined(__cplusplus) && !defined(DART_IMPLEMENTATION)

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#if defined(__has_include) && __has_include(<span>)
#include <span>
#endif

/* Pre-include the (only) C std headers dart.h's declaration side pulls, so
 * their include guards are set before the embed and no std name (size_t,
 * uint8_t, ...) gets dragged into `dart::detail`. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(DART_STRING_H) || defined(DART_TRANSPORT_H) || defined(DART_NODE_H)
#error "include dart.hpp instead of dart.h (do not include dart.h before dart.hpp)"
#endif

namespace dart {
namespace detail {

#endif  /* C++ consumer: open the hiding namespace before the embed */

/* ---- embedded C library (dist/dart.h spliced here by tools/pack.cmake) ---- */
#include "dart.h"   /* @DART_EMBED@ */

#if defined(__cplusplus) && !defined(DART_IMPLEMENTATION)
}   /* namespace detail */

/* Enums (1:1 with the C enums by value; asserted below). */
enum class Reliability { BestEffort = 0, Reliable = 1 };
enum class Role        { PubSub = 0, PubOnly = 1, SubOnly = 2, Inactive = 3 };

/* dart_channel_send / create result. Ok is 0; the rest mirror DartResult. */
enum class SendStatus  { Ok = 0, NoChannel = -1, TooBig = -2, BadRole = -3, OutOfMemory = -4 };

enum class EventKind {
    PeerUp = 0, PeerDown, PeerInterest, MessageLost, MessageTooBig, NameCollision,
    QosIncompatible, SchemaMismatch, PeerRefused, InterestOverflow,
    MetaTruncated, PeerMetaTooBig
};

/* Schema field kinds for reflection (Schema::Field); values match the C wire. */
enum class FieldType : uint8_t {
    U8 = 0, U16, U32, U64, I8, I16, I32, I64, F32, F64, Bool, Array, Struct
};

static_assert((int)Reliability::Reliable == detail::DART_RELIABLE, "reliability enum drift");
static_assert((int)Role::Inactive == detail::DART_INACTIVE, "role enum drift");
static_assert((int)SendStatus::OutOfMemory == detail::DART_ERR_OOM, "result enum drift");
static_assert((int)EventKind::PeerMetaTooBig == detail::DART_PEER_META_TOO_BIG, "event enum drift");
static_assert((int)FieldType::Struct == detail::DART_STRUCT, "field-type enum drift");

/* forward decls */
class Node;
class Channel;
class MessageIn;
class Event;

/* Bytes: a non-owning (pointer + length) view of a payload. It is a proper
 * contiguous range (begin/end/operator[], usable in range-for and algorithms)
 * and interops with the standard library both ways: construct it from a
 * string_view / std::string / const char*, or from any contiguous range of
 * byte-sized elements (std::vector / std::array / std::span of uint8_t, char, or
 * std::byte); convert it to std::string_view, std::string, std::vector, or
 * (with C++20) std::span. Multi-byte element types are rejected -- pass their
 * raw bytes explicitly via Bytes(ptr, len). */
class Bytes {
public:
    using value_type     = uint8_t;
    using iterator       = const uint8_t*;
    using const_iterator = const uint8_t*;

    constexpr Bytes() noexcept = default;
    constexpr Bytes(const void* d, size_t n) noexcept
        : data_(static_cast<const uint8_t*>(d)), size_(n) {}
    Bytes(std::string_view s) noexcept
        : data_(reinterpret_cast<const uint8_t*>(s.data())), size_(s.size()) {}
    Bytes(const std::string& s) noexcept : Bytes(std::string_view(s)) {}
    Bytes(const char* s) noexcept : Bytes(std::string_view(s ? s : "")) {}

    /* Any contiguous range of byte-sized elements. Strings are handled by the
     * overloads above (excluded here), so this covers vector/array/span/... of
     * uint8_t / char / std::byte without ambiguity. */
    template <class C,
              class E = std::remove_reference_t<decltype(*std::data(std::declval<const C&>()))>,
              class = std::enable_if_t<
                  !std::is_same<std::decay_t<C>, Bytes>::value &&
                  !std::is_convertible<const C&, std::string_view>::value &&
                  sizeof(E) == 1 && std::is_trivially_copyable<E>::value>>
    Bytes(const C& c) noexcept
        : data_(reinterpret_cast<const uint8_t*>(std::data(c))), size_(std::size(c)) {}

    const uint8_t* data()  const noexcept { return data_; }
    size_t         size()  const noexcept { return size_; }
    bool           empty() const noexcept { return size_ == 0; }
    const uint8_t* begin() const noexcept { return data_; }
    const uint8_t* end()   const noexcept { return data_ + size_; }
    const uint8_t& operator[](size_t i) const noexcept { return data_[i]; }

    std::string_view     str()       const noexcept { return { reinterpret_cast<const char*>(data_), size_ }; }
    std::string          to_string() const { return { reinterpret_cast<const char*>(data_), size_ }; }
    std::vector<uint8_t> to_vector() const { return { data_, data_ + size_ }; }

#if defined(__cpp_lib_span)
    operator std::span<const uint8_t>() const noexcept { return { data_, size_ }; }
    std::span<const uint8_t> span() const noexcept { return { data_, size_ }; }
#endif

private:
    const uint8_t* data_ = nullptr;
    size_t         size_ = 0;
};

/* Qos / NodeOptions: plain structs mirroring the C config, all zero = default. */
struct Qos {
    Reliability reliability          = Reliability::BestEffort;
    uint16_t    keep_last            = 0;   /* recent messages retained (late join / repair) */
    uint16_t    catch_up             = 0;   /* recent messages a new subscriber replays */
    uint32_t    max_message_bytes    = 0;   /* 0 = one fragment, or grow-to-fit */
    uint32_t    heartbeat_us         = 0;   /* reliable idle-writer ping (0 = 100ms) */
    uint32_t    repair_delay_us      = 0;   /* reliable reader's resend delay (0 = 20ms) */
    uint32_t    backpressure_wait_us = 0;   /* reliable: send pause for a slow reader (0 = none) */
    uint32_t    shm_max_bytes        = 0;   /* pin channel to one same-host SHM size class */
};

struct NodeOptions {
    uint16_t                 domain               = 0;   /* logical-network selector */
    uint16_t                 max_channels         = 8;   /* how many channels may be created */
    bool                     disable_shm          = false;
    /* networking (all optional) */
    uint16_t                 data_port            = 0;   /* 0 = OS-assigned */
    std::string              discovery_group;            /* empty = "239.255.0.<domain>" default */
    uint16_t                 discovery_port       = 0;   /* 0 = 7400 */
    std::string              multicast_interface;        /* empty = auto; "127.0.0.1" = single-host */
    uint8_t                  multicast_ttl        = 0;   /* 0 = 1 hop */
    std::vector<std::string> seed_peers;                 /* "ip" or "ip:port", unicast announce targets */
    uint16_t                 fragment_size        = 0;   /* UDP payload bytes per fragment */
    /* discovery cadence */
    uint32_t                 announce_interval_us = 0;   /* 0 = 1s */
    uint32_t                 peer_timeout_us      = 0;   /* 0 = 3.5s */
    uint16_t                 max_peers            = 0;   /* 0 = 16 */
    /* Memory. Leave `memory` null for the default dynamic allocator (grows on
     * demand). Set it to a fixed buffer for STATIC allocation: the node draws
     * ALL its memory from there, with no heap and no growth (and the same-host
     * SHM fast path off, since it needs a growable allocator). The buffer must
     * outlive the node and be big enough for the configured max_peers /
     * max_channels plus message buffers; open returns nullopt if it is too
     * small. A typed channel copies its schema into this buffer, so pair it with
     * Schema::compile(text, scratch, size) for a fully heap-free node. */
    void*                    memory      = nullptr;
    size_t                   memory_size = 0;
};

/* Schema: an owned, compiled message schema (see the DSL in schema.h). */
class Schema {
public:
    /* Compile a schema from its text form (see the DSL in schema.h). Returns
     * nullopt on error; if `err` is non-null it receives a short message
     * pointing near the offending text. Uses an internal dynamic (heap)
     * allocator for the compiled schema, freed when the Schema is destroyed. */
    static std::optional<Schema> compile(std::string_view text, std::string* err = nullptr) {
        return compile_with(detail::dart_allocator_dynamic(detail::i_dart_plat_realloc, 0), text, err);
    }
    /* Zero-heap variant: compile into your fixed `scratch` buffer instead of the
     * heap (for a static-memory deployment). The buffer must outlive this Schema;
     * once create_channel has copied the schema into the node, the Schema and its
     * scratch may be dropped or reused. Returns nullopt if the buffer is too small. */
    static std::optional<Schema> compile(std::string_view text, void* scratch, size_t scratch_size,
                                         std::string* err = nullptr) {
        if (!scratch || scratch_size == 0) { if (err) *err = "scratch buffer missing"; return std::nullopt; }
        return compile_with(detail::dart_allocator_static(scratch, scratch_size), text, err);
    }

    Schema(Schema&& o) noexcept : alloc_(o.alloc_), schema_(o.schema_) {
        std::memset(&o.alloc_, 0, sizeof o.alloc_);
        o.schema_ = nullptr;
    }
    Schema& operator=(Schema&& o) noexcept {
        if (this != &o) { reset(); alloc_ = o.alloc_; schema_ = o.schema_;
                          std::memset(&o.alloc_, 0, sizeof o.alloc_); o.schema_ = nullptr; }
        return *this;
    }
    Schema(const Schema&) = delete;
    Schema& operator=(const Schema&) = delete;
    ~Schema() { reset(); }

    std::string_view name() const {
        detail::DartString n = detail::dart_schema_name(schema_);
        return { n.data, n.len };
    }
    uint32_t size() const { return detail::dart_schema_size(schema_); }
    uint64_t hash() const { return detail::dart_schema_hash(schema_); }
    uint16_t field_count() const { return detail::dart_schema_field_count(schema_); }

    struct Field {
        std::string_view name;
        FieldType kind;          /* the field's type */
        FieldType elem;          /* array element type (only when kind == Array) */
        uint16_t  count, depth;
        uint32_t  offset, size;
    };
    bool field_at(uint16_t i, Field& out) const {
        detail::DartSchemaFieldInfo f;
        if (!detail::dart_schema_field_at(schema_, i, &f)) return false;
        out.name   = { f.name.data, f.name.len };
        out.kind   = static_cast<FieldType>(f.kind);
        out.elem   = static_cast<FieldType>(f.elem);
        out.count  = f.count; out.depth = f.depth;
        out.offset = f.offset; out.size = f.size;
        return true;
    }

private:
    Schema() = default;
    static std::optional<Schema> compile_with(detail::DartAllocator a, std::string_view text, std::string* err) {
        Schema s;
        s.alloc_ = a;
        std::string t(text);   /* NUL-terminate for the C API */
        const char* e = nullptr;
        s.schema_ = detail::dart_schema_compile(detail::dart_allocator_alloc, &s.alloc_, t.c_str(), &e);
        if (!s.schema_) {
            if (err) *err = e ? (std::string("schema error near: ") + e) : "schema compile failed";
            detail::dart_allocator_reset(&s.alloc_);
            return std::nullopt;
        }
        return s;
    }
    void reset() { if (alloc_.page_realloc || alloc_.shared) detail::dart_allocator_reset(&alloc_); schema_ = nullptr; }
    detail::DartAllocator alloc_{};
    detail::DartSchema*   schema_ = nullptr;

    friend class Node;
    friend class MessageOut;
    const detail::DartSchema* raw() const { return schema_; }
};

/* MessageOut: a mutable message buffer bound to a Schema, for typed encoding.
 * Set fields by name (nested members by dotted path, e.g. "vel.dx"), then
 * pass it straight to Channel::send (it converts to Bytes). */
class MessageOut {
public:
    explicit MessageOut(const Schema& s) : schema_(s.raw()), buf_(detail::dart_schema_size(s.raw())) {
        detail::dart_schema_message_default(schema_, buf_.data(), buf_.size());
    }
    MessageOut& set_uint (const char* field, uint64_t v) { detail::dart_set_uint (buf_.data(), buf_.size(), schema_, field, v); return *this; }
    MessageOut& set_int  (const char* field, int64_t  v) { detail::dart_set_int  (buf_.data(), buf_.size(), schema_, field, v); return *this; }
    MessageOut& set_f64  (const char* field, double   v) { detail::dart_set_f64  (buf_.data(), buf_.size(), schema_, field, v); return *this; }
    MessageOut& set_f32  (const char* field, float    v) { detail::dart_set_f32  (buf_.data(), buf_.size(), schema_, field, v); return *this; }
    MessageOut& set_bool (const char* field, bool     v) { detail::dart_set_uint (buf_.data(), buf_.size(), schema_, field, v ? 1u : 0u); return *this; }
    MessageOut& set_array(const char* field, Bytes elems) {
        detail::dart_set_array(buf_.data(), buf_.size(), schema_, field, detail::dart_bytes(elems.data(), elems.size()));
        return *this;
    }
    Bytes bytes() const { return { buf_.data(), buf_.size() }; }
    operator Bytes() const { return bytes(); }

private:
    const detail::DartSchema* schema_;
    std::vector<uint8_t>      buf_;
};

/* MessageIn: a delivered message. Non-owning; valid only inside the handler. */
class MessageIn {
public:
    std::string_view sender_name()  const { return { msg_->sender_name.data,  msg_->sender_name.len  }; }
    uint32_t         sender_id()    const { return msg_->sender_id; }
    std::string_view channel_name() const { return { msg_->channel_name.data, msg_->channel_name.len }; }
    uint16_t         channel_id()   const { return msg_->channel_id; }
    Bytes            data()         const { return { msg_->data.data, msg_->data.len }; }
    std::string_view text()         const { return { reinterpret_cast<const char*>(msg_->data.data), msg_->data.len }; }
    bool             has_schema()   const { return msg_->schema != nullptr; }

    /* Typed field reads (only meaningful when has_schema()); by name / dotted path. */
    uint64_t get_uint (const char* field) const { return detail::dart_get_uint (msg_->data, msg_->schema, field); }
    int64_t  get_int  (const char* field) const { return detail::dart_get_int  (msg_->data, msg_->schema, field); }
    double   get_f64  (const char* field) const { return detail::dart_get_f64  (msg_->data, msg_->schema, field); }
    float    get_f32  (const char* field) const { return detail::dart_get_f32  (msg_->data, msg_->schema, field); }
    bool     get_bool (const char* field) const { return detail::dart_get_uint (msg_->data, msg_->schema, field) != 0; }
    Bytes    get_array(const char* field) const { auto a = detail::dart_get_array(msg_->data, msg_->schema, field); return { a.data, a.len }; }

private:
    explicit MessageIn(const detail::DartMsg* m) : msg_(m) {}
    const detail::DartMsg* msg_;
    friend class Node;
};

/* Event: a peer / loss / QoS notification. */
class Event {
public:
    EventKind        kind()           const { return static_cast<EventKind>(ev_->kind); }
    uint32_t         peer()           const { return ev_->peer; }
    uint16_t         channel()        const { return ev_->channel; }
    std::string_view detail()         const { return ev_->detail ? std::string_view(ev_->detail) : std::string_view{}; }
    uint64_t         lost_first()     const { return ev_->lost_first; }
    uint64_t         lost_count()     const { return ev_->lost_count; }
    uint64_t         too_big_bytes()  const { return ev_->too_big_bytes; }
    uint64_t         identity()       const { return ev_->identity; }
    uint16_t         publish_topics() const { return ev_->publish_topics; }
    uint16_t         receive_topics() const { return ev_->receive_topics; }

    /* A one-line human-readable rendering (uses the C formatter). */
    std::string to_string() const { char b[192]; return detail::dart_event_str(ev_, b, sizeof b); }

private:
    explicit Event(const detail::DartEvent* e) : ev_(e) {}
    const detail::DartEvent* ev_;
    friend class Node;
};

/* Peer / PeerTopic: a copied snapshot of a discovered peer (safe after poll). */
struct PeerTopic {
    std::string name;
    bool        is_publisher;
    bool        reliable;
};
struct Peer {
    uint32_t               id = 0;
    std::string            name;
    std::string            address;         /* "1.2.3.4:port" */
    bool                   active = false;
    uint16_t               fragment_size = 0;
    std::vector<PeerTopic> topics;
};

/* Channel: a lightweight handle (owned by the Node, stable for its life). */
class Channel {
public:
    Channel() = default;
    bool valid() const noexcept { return ch_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    SendStatus send(Bytes data) {
        if (!ch_) return SendStatus::NoChannel;
        return static_cast<SendStatus>(
            detail::dart_channel_send(ch_, detail::dart_bytes(data.data(), data.size())));
    }
    void set_role(Role r) {
        if (!ch_) return;
        detail::dart_channel_set_role(ch_, static_cast<detail::DartRole>(r));
    }
    uint16_t index() const {
        if (!ch_) return 0xffff;
        return detail::dart_channel_index(ch_);
    }
    int match_count() const {
        if (!ch_) return 0;
        return detail::dart_channel_match_count(ch_);
    }
    /* Pump until every reader has acked, or timeout_ms elapses. Call before
     * closing so a final burst is not cut off by the BYE. */
    bool drain(int timeout_ms) {
        if (!ch_) return true;
        return detail::dart_channel_drain(ch_, timeout_ms) == 1;
    }

private:
    explicit Channel(detail::DartChannel* c) : ch_(c) {}
    detail::DartChannel* ch_ = nullptr;
    friend class Node;
};

/* Node: owns the DartNode, its memory, and the user callbacks.
 *
 * Single-threaded: drive poll() yourself in a loop (background threading was
 * removed for now). Call poll, send, create_channel, and the handlers all on one
 * thread; as with the C API, do NOT call back into the node from a handler. */
class Node {
public:
    using MessageHandler = std::function<void(const MessageIn&)>;
    using EventHandler   = std::function<void(const Event&)>;

    /* Open a node. name = a human-readable label synced via discovery (empty =>
     * an auto "node-XXXXXXXX"). Returns nullopt on failure. */
    static std::optional<Node> open(std::string_view name = {}, const NodeOptions& o = {}) {
        std::unique_ptr<Impl> impl(new Impl());
        /* The node retains the net string/seed pointers, so own that storage. */
        impl->disc_group = o.discovery_group;
        impl->mcast_if   = o.multicast_interface;
        for (const std::string& s : o.seed_peers) {
            detail::DartDiscoveryAddr a;
            if (parse_addr(s, a)) impl->seeds.push_back(a);
        }
        std::string nm(name);

        detail::DartNodeOpts co;
        std::memset(&co, 0, sizeof co);
        co.domain       = o.domain;
        co.max_channels = o.max_channels;
        co.disable_shm  = o.disable_shm ? 1 : 0;
        co.user_data    = impl.get();
        co.net.data_port           = o.data_port;
        co.net.discovery_group     = impl->disc_group.empty() ? nullptr : impl->disc_group.c_str();
        co.net.discovery_port      = o.discovery_port;
        co.net.multicast_interface = impl->mcast_if.empty()   ? nullptr : impl->mcast_if.c_str();
        co.net.multicast_ttl       = o.multicast_ttl;
        co.net.seed_peers          = impl->seeds.empty() ? nullptr : impl->seeds.data();
        co.net.n_seed_peers        = static_cast<uint16_t>(impl->seeds.size());
        co.net.fragment_size       = o.fragment_size;
        co.discovery.announce_interval_us = o.announce_interval_us;
        co.discovery.peer_timeout_us      = o.peer_timeout_us;
        co.discovery.max_peers            = o.max_peers;

        detail::DartAllocator mem = (o.memory && o.memory_size)
            ? detail::dart_allocator_static(o.memory, o.memory_size)
            : detail::dart_allocator_dynamic(detail::i_dart_plat_realloc, 0);
        detail::DartNode* n = detail::dart_node_open(
            &mem, nm.empty() ? nullptr : nm.c_str(),
            &Node::on_msg_tramp, &Node::on_evt_tramp, &co);
        if (!n) return std::nullopt;
        impl->node = n;
        return Node(std::move(impl));
    }

    Node(Node&&) noexcept = default;
    Node& operator=(Node&&) noexcept = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    ~Node() = default;   /* teardown lives in Impl::~Impl (so a move-assign tears down correctly too) */

    Node& on_message(MessageHandler h) { impl_->on_msg = std::move(h);   return *this; }
    Node& on_event  (EventHandler   h) { impl_->on_event = std::move(h); return *this; }

    /* Create a topic. schema = an optional typed schema (its bytes are copied
     * into the node, so the Schema need not outlive the channel). */
    Channel create_channel(std::string_view name, Role role = Role::PubSub,
                           const Schema* schema = nullptr, const Qos& qos = {}) {
        std::string nm(name);
        detail::DartChannelOpts co;
        std::memset(&co, 0, sizeof co);
        co.qos = to_c(qos);
        detail::DartChannel* ch = detail::dart_node_create_channel(
            impl_->node, nm.c_str(), static_cast<detail::DartRole>(role),
            schema ? schema->raw() : nullptr, &co);
        return Channel(ch);
    }

    /* Recover an already-created channel handle by its creation index. */
    Channel channel(uint16_t index) const {
        return Channel(detail::dart_node_channel(impl_->node, index));
    }

    /* One loop tick: drives discovery, RX, timers, and flushes queued TX. Blocks
     * up to timeout_ms in the socket wait (wakes early on RX; 0 = non-blocking).
     * Drive this yourself in a loop -- a node is single-threaded. */
    int poll(int timeout_ms = 0) {
        return detail::dart_node_poll(impl_->node, timeout_ms);
    }

    /* A copied snapshot of the live peer table (safe to keep after the poll). */
    std::vector<Peer> peers() const {
        std::vector<Peer> out;
        uint16_t count = 0;
        const detail::DartDiscoveryPeer* ps = detail::dart_node_peers(impl_->node, &count);
        for (uint16_t i = 0; ps && i < count; i++) {
            const detail::DartDiscoveryPeer& p = ps[i];
            Peer peer;
            peer.id            = p.id;
            peer.name          = p.name.data ? std::string(p.name.data, p.name.len) : std::string();
            peer.address       = addr_string(p.addr);
            peer.active        = (p.liveness == detail::DART_PEER_ACTIVE);
            peer.fragment_size = detail::dart_node_peer_frag(&p);
            detail::DartInterestIter it;
            std::memset(&it, 0, sizeof it);
            detail::DartTopic t;
            while (detail::dart_node_peer_interest_next(&p, &it, &t))
                peer.topics.push_back({ std::string(t.name.data, t.name.len), t.is_pub != 0, t.reliable != 0 });
            out.push_back(std::move(peer));
        }
        return out;
    }

    struct MemoryStats { size_t in_use = 0, peak = 0; uint64_t alloc_calls = 0; };
    MemoryStats memory_stats() const {
        MemoryStats s;
        detail::dart_node_mem_stats(impl_->node, &s.in_use, &s.peak, &s.alloc_calls);
        return s;
    }
    struct BackpressureStats { uint64_t waited_us = 0; uint32_t waited_sends = 0; };
    BackpressureStats backpressure_stats() const {
        BackpressureStats b;
        detail::dart_node_backpressure_stats(impl_->node, &b.waited_us, &b.waited_sends);
        return b;
    }

private:
    struct Impl {
        detail::DartNode*        node = nullptr;
        MessageHandler           on_msg;
        EventHandler             on_event;
        std::string              disc_group;
        std::string              mcast_if;
        std::vector<detail::DartDiscoveryAddr> seeds;

        ~Impl() {
            if (node) detail::dart_node_close(node, /*send_bye=*/1);
        }
    };
    std::unique_ptr<Impl> impl_;

    explicit Node(std::unique_ptr<Impl> i) : impl_(std::move(i)) {}

    static void on_msg_tramp(const detail::DartMsg* m) {
        Impl* impl = static_cast<Impl*>(m->user);
        if (impl && impl->on_msg) { MessageIn msg(m); impl->on_msg(msg); }
    }
    static void on_evt_tramp(const detail::DartEvent* e) {
        Impl* impl = static_cast<Impl*>(e->user);
        if (impl && impl->on_event) { Event ev(e); impl->on_event(ev); }
    }

    static detail::DartQos to_c(const Qos& q) {
        detail::DartQos c;
        std::memset(&c, 0, sizeof c);
        c.reliability          = static_cast<detail::DartReliability>(q.reliability);
        c.keep_last            = q.keep_last;
        c.catch_up             = q.catch_up;
        c.max_message_bytes    = q.max_message_bytes;
        c.heartbeat_us         = q.heartbeat_us;
        c.repair_delay_us      = q.repair_delay_us;
        c.backpressure_wait_us = q.backpressure_wait_us;
        c.shm_max_bytes        = q.shm_max_bytes;
        return c;
    }

    static std::string addr_string(const detail::DartDiscoveryAddr& a) {
        char b[64];
        if (a.ip_len == 4)
            std::snprintf(b, sizeof b, "%u.%u.%u.%u:%u", a.ip[0], a.ip[1], a.ip[2], a.ip[3], a.port);
        else
            std::snprintf(b, sizeof b, "[?]:%u", a.port);
        return b;
    }

    /* Parse "ip" or "ip:port" (IPv4) into a locator; port 0 = discovery_port.
     * Hand-rolled so no locale/CRT scanf (and its MSVC deprecation) is needed. */
    static bool parse_addr(const std::string& s, detail::DartDiscoveryAddr& out) {
        unsigned oct[4] = {0}, port = 0;
        size_t i = 0, n = s.size();
        for (int part = 0; part < 4; ++part) {
            if (i >= n || s[i] < '0' || s[i] > '9') return false;
            unsigned v = 0, digits = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + unsigned(s[i++] - '0'); if (++digits > 3 || v > 255) return false; }
            oct[part] = v;
            if (part < 3) { if (i >= n || s[i] != '.') return false; ++i; }
        }
        if (i < n) {                       /* optional ":port" */
            if (s[i] != ':') return false;
            ++i;
            if (i >= n) return false;
            while (i < n && s[i] >= '0' && s[i] <= '9') { port = port * 10 + unsigned(s[i++] - '0'); if (port > 65535) return false; }
            if (i != n) return false;       /* trailing garbage */
        }
        std::memset(&out, 0, sizeof out);
        out.ip[0] = (uint8_t)oct[0]; out.ip[1] = (uint8_t)oct[1];
        out.ip[2] = (uint8_t)oct[2]; out.ip[3] = (uint8_t)oct[3];
        out.ip_len = 4; out.port = (uint16_t)port;
        return true;
    }
};

}   /* namespace dart */

#endif /* C++ consumer (not the implementation anchor) */
#endif /* DART_HPP_INCLUDED */
