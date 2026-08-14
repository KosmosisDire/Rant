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
 *     dart::Node node("robot1",
 *         [](const dart::MessageView& m){
 *             std::printf("%.*s > %.*s\n",
 *                 (int)m.publisher_name().size(), m.publisher_name().data(),
 *                 (int)m.text().size(),           m.text().data());
 *         },
 *         [](const dart::Event& e){ std::fprintf(stderr, "event: %s\n", e.to_string().c_str()); },
 *         { .domain = 7 });                  // throws dart::Error on failure
 *     dart::Topic chat(node, "chat", dart::Role::PubSub, nullptr,
 *                      { .reliability = dart::Reliability::Reliable });
 *     node.start();                          // background service thread owns the loop
 *     for (;;) chat.send("hello");           // thread-safe; or skip start() and poll(1) yourself
 *
 * Typed patterns (functions / variables / signals / pub-sub) over DART_SCHEMA:
 *
 *     struct Pose { double x, y; };
 *     DART_SCHEMA(Pose, x, y);
 *     dart::Publisher<Pose> pub(node, "pose");
 *     pub.send({ 1.0, 2.0 });
 *     dart::FunctionDefinition<Pose, Pose> mirror(node, "mirror",
 *         [](const Pose& p){ return Pose{ -p.x, -p.y }; });
 *
 * Every failed constructor throws dart::Error; built with -fno-exceptions the
 * object is `!valid()` instead and construction never throws. Data-path results
 * stay SendStatus / status enums in both modes, never exceptions.
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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#if defined(__cpp_exceptions)
#include <stdexcept>
#endif
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

/* dart_topic_send / create result. Ok is 0; the rest mirror DartResult. */
enum class SendStatus  { Ok = 0, NoTopic = -1, TooBig = -2, BadRole = -3, OutOfMemory = -4,
                         State = -5, NoSys = -6 };

enum class EventKind {
    PeerUp = 0, PeerDown, PeerInterest, MessageLost, Error
};

/* The specific error carried by an EventKind::Error event (Event::error()); mirrors
   DartErrorKind. Everything that goes wrong is EventKind::Error + one of these. */
enum class ErrorKind {
    None = 0,
    NameCollision, QosIncompatible, KindMismatch, SchemaMismatch, InterestOverflow,
    MetaTruncatedInterest, MetaTruncatedSchema, PeerMetaTooBig, MessageTooBig,
    PeerRefused, EvictedUnsent, UnmatchedSend, DuplicateAuthority,
    Oom, Platform, Socket, Bind, McastJoin, Send, Recv, Poll, Waker, BadAddress
};

/* Schema field kinds for reflection (Schema::Field); values match the C wire.
 * Array/String are FIXED (offset-based); VString/VArray/Map are the VARIABLE kinds
 * that ride the message tail (offset/size report 0). */
enum class FieldType : uint8_t {
    U8 = 0, U16, U32, U64, I8, I16, I32, I64, F32, F64, Bool, Array, Struct, String,
    VString, VArray, Map, Enum, Named
};

/* A function call's outcome (mirrors DartCallStatus). Ok/AppError/NoHandler travel on
 * the wire; Timeout/PeerLost are synthesized client-side when no response arrives;
 * Cancelled is synthesized for calls still pending when the local node closes. */
enum class CallStatus { Ok = 0, AppError = 1, NoHandler = 2, Timeout = 3, PeerLost = 4,
                        Cancelled = 5 };

/* What a network entity is (mirrors DartEntityKind): observers consume ENTITIES, never
 * raw channels; a function's req/rsp pair or a variable's set channel fold into one. */
enum class EntityKind { Topic = 0, Function, Variable, Signal };

/* Severity of a built-in @dart/log line (mirrors DartLogLevel). */
enum class LogLevel { Error = 0, Warn = 1, Info = 2 };

static_assert((int)Reliability::Reliable == detail::DART_RELIABLE, "reliability enum drift");
static_assert((int)Role::Inactive == detail::DART_INACTIVE, "role enum drift");
static_assert((int)SendStatus::NoSys == detail::DART_ERR_NOSYS, "result enum drift");
static_assert((int)EventKind::Error == detail::DART_ERROR, "event enum drift");
static_assert((int)ErrorKind::Waker == detail::DART_E_WAKER, "error enum drift");
static_assert((int)ErrorKind::BadAddress == detail::DART_E_BAD_ADDRESS, "error enum drift");
static_assert((int)FieldType::Struct == detail::DART_STRUCT, "field-type enum drift");
static_assert((int)FieldType::String == detail::DART_STR, "field-type enum drift");
static_assert((int)FieldType::Map == detail::DART_MAP, "field-type enum drift");
static_assert((int)FieldType::Enum == detail::DART_ENUM, "field-type enum drift");
static_assert((int)FieldType::Named == detail::DART_NAMED, "field-type enum drift");
#ifndef DART_NO_PATTERNS
static_assert((int)CallStatus::Ok == detail::DART_CALL_OK, "call-status enum drift");
static_assert((int)CallStatus::PeerLost == detail::DART_CALL_PEER_LOST, "call-status enum drift");
static_assert((int)CallStatus::Cancelled == detail::DART_CALL_CANCELLED, "call-status enum drift");
static_assert((int)EntityKind::Topic == detail::DART_ENTITY_TOPIC, "entity enum drift");
static_assert((int)EntityKind::Signal == detail::DART_ENTITY_SIGNAL, "entity enum drift");
#endif
static_assert((int)LogLevel::Info == detail::DART_LOG_INFO, "log-level enum drift");

/* forward decls */
class Node;
class Topic;
class MessageView;
class Event;
class Schema;
template <class T = void> class Message;
template <class Req = void, class Rsp = void> class FunctionDefinition;
template <class Req = void, class Rsp = void> class RemoteFunction;
template <class T = void> class VariableDefinition;
template <class T = void> class RemoteVariable;
class VariableUpdate;
template <class T = void> class Signal;
template <class T = void> class Publisher;
template <class T = void> class Subscriber;
template <class Rsp = void> class Request;
template <class Rsp = void> class Deferred;
template <class Rsp = void> class Response;
template <class Rsp = void> class ResponseView;

/* Error: the one exception type. Thrown by FAILED CONSTRUCTORS only (Node, Topic, and
 * the pattern handles); data-path results stay SendStatus / status enums. Carries the
 * DartEvent error info: the machine-readable kind, the OS errno for socket faults, and
 * the formatted one-line text as what(). With -fno-exceptions nothing throws and a
 * failed construction leaves the object `!valid()` instead. */
#if defined(__cpp_exceptions)
class Error : public std::runtime_error {
public:
    Error(ErrorKind kind, int os_error, const std::string& text)
        : std::runtime_error(text), kind_(kind), os_(os_error) {}
    ErrorKind kind()     const noexcept { return kind_; }
    int       os_error() const noexcept { return os_; }
private:
    ErrorKind kind_;
    int       os_;
};
#endif

namespace priv {

/* raise helpers: throw dart::Error when exceptions are on, else return (the caller
 * leaves its handle invalid). raise_last formats dart_last_error(n) as the reason. */
inline void raise_last(detail::DartNode* n, const char* what) {
#if defined(__cpp_exceptions)
    detail::DartEvent e = detail::dart_last_error(n);
    char b[192];
    std::string t = what ? std::string(what) + ": " : std::string();
    t += detail::dart_event_str(&e, b, sizeof b);
    throw Error(static_cast<ErrorKind>(e.error), e.os_error, t);
#else
    (void)n; (void)what;
#endif
}
inline void raise_msg(const char* what) {
#if defined(__cpp_exceptions)
    throw Error(ErrorKind::None, 0, what);
#else
    (void)what;
#endif
}

inline uint8_t role_bits(Role r) {   /* pub = 1, sub = 2 */
    switch (r) {
    case Role::PubSub:  return 3;
    case Role::PubOnly: return 1;
    case Role::SubOnly: return 2;
    default:            return 0;
    }
}
inline Role role_from_bits(uint8_t b) {
    return b == 3 ? Role::PubSub : b == 1 ? Role::PubOnly : b == 2 ? Role::SubOnly : Role::Inactive;
}

/* base for the type-erased handler boxes the Node's Impl keeps alive until close */
struct HandlerBox { virtual ~HandlerBox() = default; };

}   /* namespace priv */

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

namespace priv {
inline detail::DartBytes  to_c(Bytes b) { return detail::dart_bytes(b.data(), b.size()); }
}

/* Qos / NodeOptions: plain structs mirroring the C config, all zero = default. */
struct Qos {
    Reliability reliability          = Reliability::BestEffort;
    uint16_t    keep_last            = 0;   /* recent messages retained (late join / repair) */
    uint16_t    catch_up             = 0;   /* recent messages a new subscriber replays */
    uint32_t    max_message_bytes    = 0;   /* 0 = one fragment, or grow-to-fit */
    uint32_t    heartbeat_us         = 0;   /* reliable idle-writer ping (0 = 100ms) */
    uint32_t    repair_delay_us      = 0;   /* reliable reader's resend delay (0 = 20ms) */
    uint32_t    backpressure_wait_us = 0;   /* reliable: send pause for a slow reader (0 = none) */
    uint32_t    shm_max_bytes        = 0;   /* pin topic to one same-host SHM size class */
    uint32_t    queue_bytes          = 0;   /* consumer-queue cap for take()/dispatch(); 0 = the
                                               queue appears lazily on first use, 1 MB cap */
    uint16_t    max_rate_hz          = 0;   /* SUBSCRIBER, best-effort: cap delivery from each
                                               publisher to this many samples/sec (it decimates
                                               to the newest); 0 = unlimited */
    bool        no_timestamp         = false;/* PUBLISHER: send without the per-message source
                                               timestamp, so receivers read written_us() == 0.
                                               Default (false) stamps every message */
};

struct NodeOptions {
    uint16_t                 domain               = 0;   /* logical-network selector */
    uint16_t                 max_topics           = 8;   /* how many topics may be created */
    bool                     disable_shm          = false;
    bool                     fetch_details        = false; /* greedily fetch every peer topic's
                                                              name + schema (observer UIs): fills
                                                              peer_entities() names via the cache */
    int32_t                  match_wait_ms        = 0;   /* send-path match wait: a send that would
                                                            reach ZERO subscribers while a match is
                                                            still resolving blocks up to this long
                                                            for it to form. 0 = default (1s);
                                                            negative = disabled (drop loudly:
                                                            ErrorKind::UnmatchedSend). */
    /* networking (all optional) */
    /* built-in observability (all default on; see Node::log / Node::on_log / Node::meta) */
    bool                     disable_logs         = false; /* strip the @dart/log/{error,warn,info}
                                                              topics (saves their history memory) */
    bool                     disable_meta         = false; /* do not host the @dart/meta endpoint */
    bool                     disable_error_logs   = false; /* suppress the default mirroring of this
                                                              node's errors onto @dart/log/error */
    uint16_t                 data_port            = 0;   /* 0 = OS-assigned */
    std::string              discovery_group;            /* empty = "239.255.0.<domain>" default */
    uint16_t                 discovery_port       = 0;   /* 0 = 7400 */
    std::string              multicast_interface;        /* empty = auto; "127.0.0.1" = single-host */
    uint8_t                  multicast_ttl        = 0;   /* 0 = 1 hop */
    std::vector<std::string> seed_peers;                 /* "ip" or "ip:port", unicast announce targets */
    bool                     unicast_only         = false; /* this node cannot multicast at all: join
                                                   no group, announce only to seed_peers + peers already
                                                   known, and ask whoever hears us to RE-ANNOUNCE us on
                                                   their paths. Seeding ONE reachable node then makes us
                                                   discoverable mesh-wide (data stays unicast either
                                                   way). Pair with seed_peers, or be seeded by a peer. */
    uint16_t                 fragment_size        = 0;   /* UDP payload bytes per fragment */
    /* State our locator outright instead of letting each peer learn it from the datagram
     * source (the default, and right for multihomed hosts). For a STATIC 1:1 mapping (a
     * cloud elastic IP, a container published with -p 7400:7400) or to pin which of our
     * addresses to advertise. One locator goes to EVERY peer, so it suits a 1:1 mapping
     * and not a split inside/outside view; and it creates no inbound path by itself. */
    std::string              self_ip;                    /* "203.0.113.7"; empty = learn per path */
    uint16_t                 advertise_port       = 0;   /* 0 = the port we actually bound */
    /* discovery cadence */
    uint32_t                 announce_interval_us = 0;   /* 0 = 1s */
    uint32_t                 peer_timeout_us      = 0;   /* 0 = 3.5s */
    uint16_t                 max_peers            = 0;   /* 0 = 16 */
    /* Memory. Leave `memory` null for the default dynamic allocator (grows on
     * demand). Set it to a fixed buffer for STATIC allocation: the node draws
     * ALL its memory from there, with no heap and no growth (and the same-host
     * SHM fast path off, since it needs a growable allocator). The buffer must
     * outlive the node and be big enough for the configured max_peers /
     * max_topics plus message buffers; construction fails if it is too
     * small. A typed topic copies its schema into this buffer, so pair it with
     * Schema::compile(text, scratch, size) for a fully heap-free node. */
    void*                    memory      = nullptr;
    size_t                   memory_size = 0;
};

#ifndef DART_NO_PATTERNS
/* Per-pattern options (all zero = defaults). */
struct FunctionOptions {
    uint32_t backpressure_wait_us = 0;   /* 0 = 1s (patterns are low-rate, loss unacceptable) */
    uint32_t timeout_us           = 0;   /* remote call timeout; 0 = 5s */
};
struct SignalOptions {
    uint32_t backpressure_wait_us = 0;   /* 0 = 1s */
};
/* Per-call options (mirrors DartCallOpts). provider directs a call at ONE definition by
 * its peer id (0 = undirected, first answer wins): the way to reach a specific node when
 * many host the same function, e.g. the @dart/meta endpoint (see Node::meta). */
struct CallOptions {
    uint32_t provider = 0;
};
/* VariableOptions<T> for the typed definition (initial is a typed value);
 * VariableOptions<> is the untyped twin (initial is raw Bytes). */
template <class T = void> struct VariableOptions {
    std::optional<T> initial{};              /* the value before any set */
    bool     read_only            = false;   /* no set channel: remote sets get BadRole */
    bool     allow_force          = false;   /* permit force (local + remote) */
    uint16_t catch_up             = 0;       /* value-channel catch_up; 0 = 1 (late remote gets latest) */
    uint32_t backpressure_wait_us = 0;       /* 0 = 1s */
};
template <> struct VariableOptions<void> {
    Bytes    initial{};
    bool     read_only            = false;
    bool     allow_force          = false;
    uint16_t catch_up             = 0;
    uint32_t backpressure_wait_us = 0;
};
#endif /* !DART_NO_PATTERNS */

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
     * once a topic create has copied the schema into the node, the Schema and its
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
    /* Flat index of a field by name; nested members by dotted path ("velocity.dx"),
     * struct-array members by an indexed one ("corners[2].x"). -1 if unknown. */
    int field_index(std::string_view path) const {
        return detail::dart_schema_field_index(schema_, std::string(path).c_str());
    }
    /* Can a reader declaring THIS schema read messages written with `pub`? Type NAMES
     * narrow: an anonymous type reads a named one, never the reverse. */
    bool can_read(const Schema& pub) const {
        return detail::dart_schema_subset(schema_, pub.schema_) != 0;
    }
    /* Spell the schema back as compile-ready DSL text (the inverse of compile), with
     * every named type it uses hoisted to a leading `Name = type` definition. */
    std::string to_dsl() const {
        uint32_t n = detail::dart_schema_print(schema_, nullptr, 0);
        std::string out(n, '\0');
        if (n) detail::dart_schema_print(schema_, &out[0], n + 1);
        return out;
    }

    struct Field {
        std::string_view name;
        std::string_view type_name;  /* the field type's NAME, empty when anonymous */
        std::string_view elem_name;  /* an array ELEMENT type's name, empty when anonymous */
        FieldType kind;          /* the field's type (a named type reports what it wraps) */
        FieldType elem;          /* array element type (Array), or enum backing type (Enum) */
        uint16_t  count, depth;  /* array/enum option count */
        uint16_t  str_cap;       /* string capacity (String fields and String-element arrays) */
        uint16_t  arr_parent;    /* flat index of the enclosing struct ARRAY, 0xFFFF for none */
        uint32_t  offset, size;
        uint32_t  elem_size;     /* bytes of one array element, else 0 */
    };
    bool field_at(uint16_t i, Field& out) const {
        detail::DartSchemaFieldInfo f;
        if (!detail::dart_schema_field_at(schema_, i, &f)) return false;
        out.name      = { f.name.data, f.name.len };
        out.type_name = { f.type_name.data ? f.type_name.data : "", f.type_name.len };
        out.elem_name = { f.elem_name.data ? f.elem_name.data : "", f.elem_name.len };
        out.kind   = static_cast<FieldType>(f.kind);
        out.elem   = static_cast<FieldType>(f.elem);
        out.count  = f.count; out.depth = f.depth;
        out.str_cap = f.str_cap; out.arr_parent = f.arr_parent;
        out.offset = f.offset; out.size = f.size; out.elem_size = f.elem_size;
        return true;
    }

    /* Enum options (by flat field index). One option of an Enum field. */
    struct EnumVariant { int64_t value; std::string_view name; };
    uint16_t enum_count(uint16_t field) const { return detail::dart_schema_enum_count(schema_, field); }
    bool enum_variant(uint16_t field, uint16_t i, EnumVariant& out) const {
        detail::DartString n; int64_t v;
        if (!detail::dart_schema_enum_variant(schema_, field, i, &v, &n)) return false;
        out.value = v; out.name = { n.data, n.len };
        return true;
    }

    /* The raw compiled schema (opaque to the wrapper's consumers; the create calls use it). */
    const detail::DartSchema* raw() const { return schema_; }

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
};

/* ---- map: the self-describing tagged value tree (a `map` field's content) ------
 * Thin OOP over the C map codec (the real DartMapWriter / dart_map_* live in the
 * embedded library, so this wraps them directly -- no reimplementation). Write a
 * body with MapWriter, hand it to MessageBuilder::set_map; read one from
 * MessageView::get_map as a MapReader, or decode a whole map into an owning std::map
 * (MapReader::to_map -> MapDict of MapItem) that outlives the handler. */
class MapReader;
class MapItem;
using MapList = std::vector<MapItem>;             /* a decoded array value's elements */
using MapDict = std::map<std::string, MapItem>;   /* a decoded map: the std::map readback */

/* Builds a map body into a fixed internal buffer (grow via the ctor arg). Key order
 * is yours; array elements are keyless (pass nullptr). finish() returns the body bytes
 * (empty if the buffer overflowed -- check ok()). */
class MapWriter {
public:
    explicit MapWriter(size_t capacity = 512) : buf_(capacity ? capacity : 1) {
        w_ = detail::dart_map_begin(buf_.data(), buf_.size());
    }
    MapWriter(const MapWriter&) = delete;             /* holds a raw buffer pointer */
    MapWriter& operator=(const MapWriter&) = delete;

    MapWriter& put_uint  (const char* key, uint64_t v)         { detail::dart_map_put_uint  (&w_, key, v); return *this; }
    MapWriter& put_int   (const char* key, int64_t  v)         { detail::dart_map_put_int   (&w_, key, v); return *this; }
    MapWriter& put_f64   (const char* key, double   v)         { detail::dart_map_put_f64   (&w_, key, v); return *this; }
    MapWriter& put_f32   (const char* key, float    v)         { detail::dart_map_put_f32   (&w_, key, v); return *this; }
    MapWriter& put_bool  (const char* key, bool     v)         { detail::dart_map_put_bool  (&w_, key, v ? 1 : 0); return *this; }
    MapWriter& put_string(const char* key, std::string_view v) { detail::dart_map_put_string(&w_, key, detail::dart_string(v.data(), v.size())); return *this; }
    /* nested map / array: open, write members (array members pass key = nullptr), close */
    MapWriter& open_map  (const char* key) { detail::dart_map_open_map  (&w_, key); return *this; }
    MapWriter& open_array(const char* key) { detail::dart_map_open_array(&w_, key); return *this; }
    MapWriter& close()                     { detail::dart_map_close     (&w_);      return *this; }

    bool  ok() const { return w_.err == 0; }
    Bytes finish() { uint32_t n = detail::dart_map_finish(&w_); return { buf_.data(), n }; }

private:
    std::vector<uint8_t>  buf_;
    detail::DartMapWriter w_{};
};

/* One value read from a map: a scalar, a string, a nested map, or an array. */
class MapValue {
public:
    FieldType        kind()      const { return static_cast<FieldType>(v_.kind); }
    uint64_t         as_uint()   const { return v_.v.u; }
    int64_t          as_int()    const { return v_.v.i; }
    double           as_f64()    const { return v_.v.f; }
    bool             as_bool()   const { return v_.v.u != 0; }
    std::string_view as_string() const { return { reinterpret_cast<const char*>(v_.bytes.data), v_.bytes.len }; }
    Bytes            raw()       const { return { v_.bytes.data, v_.bytes.len }; }
    MapReader        as_map()    const;   /* kind() == FieldType::Map */
    /* array value (kind() == FieldType::VArray): element count + element by index */
    uint16_t array_count()          const { return detail::dart_map_array_count(v_.bytes); }
    MapValue array_at(uint16_t i)   const { MapValue o; detail::dart_map_array_at(v_.bytes, i, &o.v_); return o; }

private:
    detail::DartValue v_{};
    friend class MapReader;
};

/* Reads a map body: by key, or iterate by index. Every walk is bounds-checked in the
 * C core. The body is a view into the message, so read it inside the handler. */
class MapReader {
public:
    MapReader() = default;
    explicit MapReader(Bytes body) : body_(detail::dart_bytes(body.data(), body.size())) {}
    uint16_t count() const { return detail::dart_map_count(body_); }
    bool get(const char* key, MapValue& out) const { return detail::dart_map_get(body_, key, &out.v_) != 0; }
    bool at(uint16_t i, std::string_view& key, MapValue& out) const {
        detail::DartString k;
        if (!detail::dart_map_at(body_, i, &k, &out.v_)) return false;
        key = { k.data, k.len };
        return true;
    }
    Bytes body() const { return { body_.data, body_.len }; }
    /* Decode the whole map (recursively) into an owning std::map<std::string, MapItem>,
     * copied out of the message so it stays valid after the handler returns. Keys are
     * sorted (std::map). Empty for a non-map / mismatched field. */
    MapDict to_map() const;

private:
    detail::DartBytes body_{};
};

inline MapReader MapValue::as_map() const { return MapReader(Bytes{ v_.bytes.data, v_.bytes.len }); }

/* One node of a decoded map (MapReader::to_map): an owning scalar / string / array
 * (MapList) / nested map (MapDict). Numeric getters coerce between uint/int/double, so a
 * schema-less map -- where an integer arrives as whatever kind fit -- reads back cleanly.
 * All accessors are no-throw: a type mismatch yields the zero value / empty container. */
class MapItem {
public:
    using Value = std::variant<std::monostate, bool, uint64_t, int64_t, double,
                               std::string, MapList, MapDict>;
    Value value;

    bool is_null()   const { return std::holds_alternative<std::monostate>(value); }
    bool is_bool()   const { return std::holds_alternative<bool>(value); }
    bool is_uint()   const { return std::holds_alternative<uint64_t>(value); }
    bool is_int()    const { return std::holds_alternative<int64_t>(value); }
    bool is_double() const { return std::holds_alternative<double>(value); }
    bool is_string() const { return std::holds_alternative<std::string>(value); }
    bool is_array()  const { return std::holds_alternative<MapList>(value); }
    bool is_map()    const { return std::holds_alternative<MapDict>(value); }

    bool     as_bool() const { auto p = std::get_if<bool>(&value); return p && *p; }
    uint64_t as_uint() const {
        if (auto p = std::get_if<uint64_t>(&value)) return *p;
        if (auto p = std::get_if<int64_t>(&value))  return static_cast<uint64_t>(*p);
        if (auto p = std::get_if<double>(&value))   return static_cast<uint64_t>(*p);
        return 0;
    }
    int64_t as_int() const {
        if (auto p = std::get_if<int64_t>(&value))  return *p;
        if (auto p = std::get_if<uint64_t>(&value)) return static_cast<int64_t>(*p);
        if (auto p = std::get_if<double>(&value))   return static_cast<int64_t>(*p);
        return 0;
    }
    double as_f64() const {
        if (auto p = std::get_if<double>(&value))   return *p;
        if (auto p = std::get_if<uint64_t>(&value)) return static_cast<double>(*p);
        if (auto p = std::get_if<int64_t>(&value))  return static_cast<double>(*p);
        return 0.0;
    }
    const std::string& as_string() const {
        if (auto p = std::get_if<std::string>(&value)) return *p;
        static const std::string empty; return empty;
    }
    const MapList& as_array() const {
        if (auto p = std::get_if<MapList>(&value)) return *p;
        static const MapList empty; return empty;
    }
    const MapDict& as_map() const {
        if (auto p = std::get_if<MapDict>(&value)) return *p;
        static const MapDict empty; return empty;
    }

    /* Copy one cursor MapValue (a scalar / string / array / nested map) into an owning node. */
    static MapItem from(const MapValue& v) {
        MapItem m;
        switch (v.kind()) {
        case FieldType::Bool: m.value = v.as_bool(); break;
        case FieldType::U8: case FieldType::U16: case FieldType::U32: case FieldType::U64:
            m.value = v.as_uint(); break;
        case FieldType::I8: case FieldType::I16: case FieldType::I32: case FieldType::I64:
            m.value = v.as_int(); break;
        case FieldType::F32: case FieldType::F64: m.value = v.as_f64(); break;
        case FieldType::VString: m.value = std::string(v.as_string()); break;
        case FieldType::VArray: {
            MapList list;
            uint16_t n = v.array_count();
            list.reserve(n);
            for (uint16_t i = 0; i < n; i++) list.push_back(from(v.array_at(i)));
            m.value = std::move(list);
            break;
        }
        case FieldType::Map: m.value = v.as_map().to_map(); break;
        default: break;   /* leaves monostate (null) */
        }
        return m;
    }
};

inline MapDict MapReader::to_map() const {
    MapDict out;
    uint16_t n = count();
    for (uint16_t i = 0; i < n; i++) {
        std::string_view key;
        MapValue v;
        if (!at(i, key, v)) break;
        out.emplace(std::string(key), MapItem::from(v));
    }
    return out;
}

/* MessageBuilder: a mutable message buffer bound to a Schema, for typed encoding.
 * Set fields by name (nested members by dotted path, e.g. "vel.dx"), then
 * pass it straight to Topic::send (it converts to Bytes). Variable-length fields
 * (`string`, `elem[]`, `map`) grow the buffer as needed; over-cap on a capped field
 * is refused and flips ok() to false (never a silent truncation). */
class MessageBuilder {
public:
    explicit MessageBuilder(const Schema& s) : schema_(s.raw()), buf_(detail::dart_schema_msg_min(s.raw())) {
        detail::dart_schema_message_default(schema_, buf_.data(), buf_.size());
    }
    MessageBuilder& set_uint (const char* field, uint64_t v) { ok_ &= detail::dart_set_uint (buf_.data(), buf_.size(), schema_, field, v) != 0; return *this; }
    MessageBuilder& set_int  (const char* field, int64_t  v) { ok_ &= detail::dart_set_int  (buf_.data(), buf_.size(), schema_, field, v) != 0; return *this; }
    MessageBuilder& set_f64  (const char* field, double   v) { ok_ &= detail::dart_set_f64  (buf_.data(), buf_.size(), schema_, field, v) != 0; return *this; }
    MessageBuilder& set_f32  (const char* field, float    v) { ok_ &= detail::dart_set_f32  (buf_.data(), buf_.size(), schema_, field, v) != 0; return *this; }
    MessageBuilder& set_bool (const char* field, bool     v) { ok_ &= detail::dart_set_uint (buf_.data(), buf_.size(), schema_, field, v ? 1u : 0u) != 0; return *this; }
    /* a capped OR variable string (dart_set_string handles both) */
    MessageBuilder& set_string(const char* field, std::string_view v) {
        grow_for(v.size());
        ok_ &= detail::dart_set_string(buf_.data(), buf_.size(), schema_, field, detail::dart_string(v.data(), v.size())) != 0;
        return *this;
    }
    /* one element of a string array (a variable string array must be grown first with
       a set_array of empty slots, per the C API) */
    MessageBuilder& set_string_at(const char* field, uint16_t index, std::string_view v) {
        grow_for(v.size() + 2);
        ok_ &= detail::dart_set_string_at(buf_.data(), buf_.size(), schema_, field, index, detail::dart_string(v.data(), v.size())) != 0;
        return *this;
    }
    /* a fixed OR variable array (raw element bytes; for a string array, whole
       [u16 len][cap] slots) */
    MessageBuilder& set_array(const char* field, Bytes elems) {
        grow_for(elems.size());
        ok_ &= detail::dart_set_array(buf_.data(), buf_.size(), schema_, field, detail::dart_bytes(elems.data(), elems.size())) != 0;
        return *this;
    }
    /* a `map` field, from a finished map body */
    MessageBuilder& set_map(const char* field, Bytes map_body) {
        grow_for(map_body.size());
        ok_ &= detail::dart_set_map(buf_.data(), buf_.size(), schema_, field, detail::dart_bytes(map_body.data(), map_body.size())) != 0;
        return *this;
    }
    MessageBuilder& set_map(const char* field, MapWriter& w) { return set_map(field, w.finish()); }
    /* an `enum` field by its number, or by option name (unknown name is refused -> ok() false) */
    MessageBuilder& set_enum(const char* field, int64_t value) {
        ok_ &= detail::dart_set_int(buf_.data(), buf_.size(), schema_, field, value) != 0;
        return *this;
    }
    MessageBuilder& set_enum(const char* field, std::string_view name) {
        std::string n(name);
        ok_ &= detail::dart_set_enum(buf_.data(), buf_.size(), schema_, field, n.c_str()) != 0;
        return *this;
    }

    /* false if any setter was refused (over-cap string, unknown field, short buffer) */
    bool ok() const { return ok_; }
    /* the live message bytes (fixed section + its variable tail) */
    Bytes bytes() const { return { buf_.data(), detail::dart_schema_msg_len(schema_, buf_.data(), buf_.size()) }; }
    operator Bytes() const { return bytes(); }

private:
    /* ensure room for a variable frame to grow: the new content can add at most its
       own length past the current live length. resize preserves the live prefix. */
    void grow_for(size_t extra) {
        size_t used = detail::dart_schema_msg_len(schema_, buf_.data(), buf_.size());
        if (buf_.size() < used + extra) buf_.resize(used + extra);
    }
    const detail::DartSchema* schema_;
    std::vector<uint8_t>      buf_;
    bool                      ok_ = true;
};

/* FieldView: the shared read surface over (payload bytes, schema). Every delivered
 * form derives it -- MessageView (handler view), Message<> (taken message), Request<>
 * (function request), ResponseView<> (async response) -- so typed field reads look the
 * same everywhere. Reads are only meaningful when has_schema(). */
class FieldView {
public:
    Bytes            data() const { return { d_.data, d_.len }; }
    std::string_view text() const { return { reinterpret_cast<const char*>(d_.data), d_.len }; }
    bool             has_schema() const { return s_ != nullptr; }
    /* the raw compiled schema the payload decodes with (internal; feeds the typed codec) */
    const detail::DartSchema* raw_schema() const { return s_; }

    /* Typed field reads (only meaningful when has_schema()); by name / dotted path. */
    uint64_t get_uint (const char* field) const { return detail::dart_get_uint (d_, s_, field); }
    int64_t  get_int  (const char* field) const { return detail::dart_get_int  (d_, s_, field); }
    double   get_f64  (const char* field) const { return detail::dart_get_f64  (d_, s_, field); }
    float    get_f32  (const char* field) const { return detail::dart_get_f32  (d_, s_, field); }
    bool     get_bool (const char* field) const { return detail::dart_get_uint (d_, s_, field) != 0; }
    Bytes    get_array(const char* field) const { auto a = detail::dart_get_array(d_, s_, field); return { a.data, a.len }; }
    /* capped OR variable string; empty view on a mismatch */
    std::string_view get_string(const char* field) const { auto s = detail::dart_get_string(d_, s_, field); return { s.data, s.len }; }
    std::string_view get_string_at(const char* field, uint16_t index) const { auto s = detail::dart_get_string_at(d_, s_, field, index); return { s.data, s.len }; }
    /* a `map` field, as a reader over its body (valid while the view is) */
    MapReader get_map(const char* field) const { auto b = detail::dart_get_map(d_, s_, field); return MapReader(Bytes{ b.data, b.len }); }
    /* an `enum` field: its number is get_int/get_uint; this is the current value's option
     * name ({} if the stored number has no option, i.e. an unknown/newer value) */
    std::string_view get_enum_name(const char* field) const { auto s = detail::dart_get_enum(d_, s_, field); return { s.data, s.len }; }

protected:
    FieldView() = default;
    FieldView(detail::DartBytes d, const detail::DartSchema* s) : d_(d), s_(s) {}
    detail::DartBytes         d_{};
    const detail::DartSchema* s_ = nullptr;
};

/* MessageView: a delivered message. Non-owning; valid only inside the handler. */
class MessageView : public FieldView {
public:
    std::string_view publisher_name() const { return { msg_->publisher_name.data,  msg_->publisher_name.len  }; }
    uint32_t         publisher_id()   const { return msg_->publisher_id; }
    std::string_view topic_name()     const { return { msg_->topic_name.data, msg_->topic_name.len }; }
    uint16_t         topic_index()    const { return msg_->topic_index; }
    /* the pattern-header bytes in front of the payload ({NULL,0} on a plain topic) */
    Bytes            header()         const { return { msg_->header.data, msg_->header.len }; }
    /* node monotonic us when the poll RECEIVED it (queued: at enqueue), so paced
     * consumers measure true arrival times, never their own cadence */
    uint64_t         recv_us()        const { return msg_->recv_us; }
    /* the WRITER's wall clock (UTC us) at the moment it wrote the message: a source stamp,
     * kept across repair and replay. 0 = the publisher opted out (Qos::no_timestamp).
     * Never mix it with the monotonic recv_us. */
    uint64_t         written_us()        const { return msg_->written_us; }

private:
    explicit MessageView(const detail::DartMsg* m) : FieldView(m->data, m->schema), msg_(m) {}
    const detail::DartMsg* msg_;
    friend class Node;
    template <class A> friend class Signal;
};

/* Event: a peer / message-loss / error notification. Everything that goes wrong arrives
   as kind() == EventKind::Error with error() set; to_string() formats any of them. */
class Event {
public:
    EventKind        kind()           const { return static_cast<EventKind>(ev_->kind); }
    ErrorKind        error()          const { return static_cast<ErrorKind>(ev_->error); }
    bool             is_error()       const { return ev_->kind == detail::DART_ERROR; }
    uint32_t         peer()           const { return ev_->peer; }
    /* the peer's human-readable node name for peer-scoped events (empty when unknown);
       prefer it over peer() in messages, an id means nothing to a human */
    std::string_view peer_name()      const { return ev_->peer_name ? std::string_view(ev_->peer_name) : std::string_view{}; }
    uint16_t         topic()          const { return ev_->topic; }
    /* our topic name for topic-scoped events, else empty */
    std::string_view topic_name()     const { return ev_->topic_name ? std::string_view(ev_->topic_name) : std::string_view{}; }
    int              os_error()       const { return ev_->os_error; }
    uint64_t         lost_first()     const { return ev_->lost_first; }
    uint64_t         lost_count()     const { return ev_->lost_count; }
    uint64_t         too_big_bytes()  const { return ev_->too_big_bytes; }
    uint64_t         identity()       const { return ev_->identity; }
    uint16_t         publish_topics() const { return ev_->publish_topics; }
    uint16_t         receive_topics() const { return ev_->receive_topics; }
    /* SchemaMismatch: what exactly was incompatible (empty when unknown). */
    std::string      schema_detail()  const { return ev_->schema_detail ? ev_->schema_detail : ""; }

    /* A one-line human-readable rendering (uses the C formatter). */
    std::string to_string() const { char b[192]; return detail::dart_event_str(ev_, b, sizeof b); }

private:
    explicit Event(const detail::DartEvent* e) : ev_(e) {}
    const detail::DartEvent* ev_;
    friend class Node;
};

/* SchemaField: one field of a reflected schema, an OWNED copy (safe to keep). Mirrors
 * Schema::Field plus, for an enum, its option table. The flat depth-first order matches
 * Schema::field_at -- a struct's members directly follow it one depth deeper, and offsets
 * are message-absolute (0 for the variable-tail kinds vstring/varr/map). */
struct SchemaField {
    std::string name;
    FieldType   kind    = FieldType::U8;
    FieldType   elem    = FieldType::U8;   /* Array element type, or Enum backing type */
    uint16_t    count   = 0;               /* Array element / Enum option count */
    uint16_t    depth   = 0;               /* 0 = top level; n = member of the struct n levels up */
    uint16_t    str_cap = 0;               /* String capacity (String fields and String-element arrays) */
    uint32_t    offset  = 0, size = 0;
    struct Option { std::string name; int64_t value = 0; };
    std::vector<Option> variants;          /* Enum only: its named options, declaration order */
};

/* SchemaInfo: a reflected schema as an OWNED snapshot (safe to keep) -- its root name,
 * hash and fixed size, plus the flat field table. `empty()` when the entity is untyped
 * or its details are not yet fetched. This is what a mesh debugger renders per entity,
 * and what lets it decode live messages of a topic it merely discovered. */
struct SchemaInfo {
    std::string              name;
    uint64_t                 hash = 0;   /* the schema identity (matches Entity::schema_hash) */
    uint32_t                 size = 0;   /* fixed-section length; where the variable tail begins */
    std::vector<SchemaField> fields;
    bool empty() const { return fields.empty() && hash == 0; }
};

/* Entity: one network entity, as advertised by a peer or hosted locally. Pattern
 * channels are folded (a function's req/rsp pair is ONE function entity; a variable's
 * set channel merges into its value entity as `writable`); plain topics pass through.
 * A copied snapshot, safe to keep (including the schema field tables). `name` is the
 * base name with any @-mangling stripped; until the peer's details are fetched it is the
 * "0x????????" hash placeholder (details arrive within an RTT; NodeOptions::fetch_details
 * covers topics this node does not share, so a full mesh debugger sets it). */
struct Entity {
    EntityKind  kind = EntityKind::Topic;
    std::string name;
    bool        provides   = false;   /* they are the source side: publisher / definition / owner / emitter */
    bool        consumes   = false;   /* they are the sink side: subscriber / caller / accessor / listener */
    bool        reliable   = false;   /* the primary channel's advertised reliability */
    bool        writable   = false;   /* VARIABLE: a set channel is advertised alongside the value */
    bool        forceable  = false;   /* VARIABLE: the owner permits force/unforce (allow_force) */
    bool        incomplete = false;   /* a pattern half-pair: surfaced, never silently dropped */
    uint16_t    index = 0;            /* the primary channel's index at the peer */
    uint32_t    hash  = 0;            /* the primary channel's low-32 name hash (the placeholder) */
    uint64_t    schema_hash     = 0;  /* value/request/payload schema identity (0 = untyped/unfetched) */
    uint64_t    rsp_schema_hash = 0;  /* FUNCTION only: the response schema identity */
    SchemaInfo  schema;               /* value/request/payload schema field table (empty = untyped/unfetched) */
    SchemaInfo  rsp_schema;           /* FUNCTION only: the response schema field table */
};

/* Peer: a copied snapshot of a discovered peer (safe to keep after the poll). Peer
 * facts only: what a peer advertises is a separate, explicit Node::peer_entities(id)
 * call, so a peers() in a hot path (an event handler, a UI tick) never pays the
 * entity fold or its allocations. */
struct Peer {
    uint32_t            id = 0;
    std::string         name;
    std::string         address;         /* "1.2.3.4:port" */
    bool                active = false;
    uint16_t            fragment_size = 0;
};

/* LogLine: one decoded @dart/log line handed to a Node::on_log handler. The `node` and
 * `text` views are valid for the callback only (copy them to keep them). wall_us is epoch
 * micros (comparable across nodes); mono_us is the publisher's monotonic clock (orders
 * within one node); recv_us is this node's clock when the poll received it; written_us is the
 * transport's source stamp for the carrying message (see MessageView::written_us). */
struct LogLine {
    LogLevel         level = LogLevel::Info;
    std::string_view node;         /* the publishing node's name */
    uint32_t         node_id = 0;  /* the publishing peer id */
    uint64_t         wall_us = 0;
    uint64_t         mono_us = 0;
    uint64_t         recv_us = 0;
    uint64_t         written_us = 0;
    std::string_view text;
};

/* Message<T> / Message<>: a message popped from a topic's consumer queue.
 * Message<> (untyped, from Topic::take / Subscriber<>::take) owns the envelope struct
 * by value; its data/name views point into the topic's ring and stay valid until the
 * NEXT take/dispatch on that topic. Message<T> (typed, from Subscriber<T>::take) owns
 * the decoded T plus copied envelope strings, so it is valid indefinitely. */
template <> class Message<void> : public FieldView {
public:
    Message() = default;
    bool valid() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    std::string_view publisher_name() const { return { m_.publisher_name.data,  m_.publisher_name.len  }; }
    uint32_t         publisher_id()   const { return m_.publisher_id; }
    std::string_view topic_name()     const { return { m_.topic_name.data, m_.topic_name.len }; }
    uint16_t         topic_index()    const { return m_.topic_index; }
    Bytes            header()         const { return { m_.header.data, m_.header.len }; }
    uint64_t         recv_us()        const { return m_.recv_us; }   /* arrival stamp (see MessageView) */
    uint64_t         written_us()        const { return m_.written_us; }   /* source stamp (see MessageView) */
private:
    void bind() { d_ = m_.data; s_ = m_.schema; ok_ = true; }
    detail::DartMsg m_{};
    bool ok_ = false;
    friend class Topic;
};

/* =========================================================================== *
 *  DART_SCHEMA reflection + the typed codec.
 *
 *  DART_SCHEMA(T, fields...) (at global scope, after the struct) specializes
 *  dart::reflect<T> with the field list. On first use the codec synthesizes the
 *  schema DSL text from it, compiles it through the C compiler (which stays the
 *  single source of wire truth), and builds a flat copy table. When the struct
 *  is padding-free (every member lands exactly at its wire offset on a
 *  little-endian host) encode/decode are a single memcpy; otherwise a per-field
 *  loop runs. Deliveries whose schema hash differs from ours (a compatible
 *  publisher with more/other fields) rebuild the wire offsets from the incoming
 *  schema and cache the result, so the subset/rebase read stays cheap.
 *
 *  Wire types: sized integers, float/double, bool, T[N] / std::array<U,N> of
 *  those, dart::String<N> (the capped-string wire slot), and nested DART_SCHEMA
 *  structs. std::string / std::vector / pointers / maps are refused at compile
 *  time (variable-length fields belong to the dynamic Schema/MessageBuilder API).
 *
 *  A BARE TYPE needs no DART_SCHEMA: any of those wire types used DIRECTLY as the
 *  handle's T (Publisher<bool>, RemoteVariable<float>, Signal<std::array<float,3>>,
 *  Subscriber<std::string>) is the whole schema, anonymous, so `bool` from any
 *  language is the same wire bytes and the same hash. std::string is allowed HERE
 *  (as a root it is the unbounded `string` type, one tail frame, not a fixed slot).
 * =========================================================================== */

template <class T> struct reflect;              /* specialized by DART_SCHEMA */
template <class E> struct reflect_enum;         /* specialized by DART_ENUM */
template <class U> struct field_tag {};         /* visitor dispatch tag */

/* String<N>: exactly the capped-string wire slot, [u16 live-length][N bytes].
 * assign() refuses (returns false) over capacity, never truncates; view() clamps a
 * hostile length so it can never over-read. */
template <uint16_t N> struct String {
    uint16_t len = 0;
    char     data[N] = {};

    String() = default;
    template <size_t M> String(const char (&lit)[M]) {
        static_assert(M - 1 <= N, "string literal exceeds dart::String capacity");
        len = static_cast<uint16_t>(M - 1);
        std::memcpy(data, lit, M - 1);
    }
    bool assign(std::string_view s) {
        if (s.size() > N) return false;
        len = static_cast<uint16_t>(s.size());
        if (s.size()) std::memcpy(data, s.data(), s.size());
        if (s.size() < N) std::memset(data + s.size(), 0, N - s.size());
        return true;
    }
    std::string_view view() const { return { data, len <= N ? len : N }; }
    bool operator==(std::string_view s) const { return view() == s; }
};

/* ===========================================================================
 *  STANDARD TYPES (see docs/stdtypes.md and src/serialize/stdtypes.h)
 *
 *  A wire type may carry a NAME, which rides the schema (never a message byte) and
 *  NARROWS matching: a `Celsius` field never binds to a `Fahrenheit` one, while a
 *  reader declaring the bare shape still reads either. The library below pre-names the
 *  types applications keep re-inventing; the names are always in scope in the DSL.
 *
 *      struct Track { dart::Pose at; dart::Uuid id; dart::Timestamp when; };
 *      DART_SCHEMA(Track, at, id, when);          // -> "Track { at: Pose, id: Uuid, ... }"
 *
 *  Each mirror is standard-layout and identical to the wire on a little-endian target,
 *  so the usual memcpy fast path still fires. Give one of YOUR types a wire name the
 *  same way the library does, by specializing std_type (see the two macros below).
 *  Image / VideoFrame carry a variable member, so they have no fixed C++ mirror: reach
 *  them through Schema + MessageOut/MessageIn, or name them in a DSL string.
 * =========================================================================== */

/* The naming hook: `name` is the wire type name, and `repr` is void for a type whose
 * shape is a struct (its members are reflected) or the representation type for an ALIAS
 * (`Uuid = u8[16]` reprs as uint8_t[16]). Unspecialized = an ordinary anonymous type. */
template <class T> struct std_type { static constexpr const char* name = nullptr; using repr = void; };

struct Float2  { float x, y; };
struct Float3  { float x, y, z; };
struct Float4  { float x, y, z, w; };
struct Double2 { double x, y; };
struct Double3 { double x, y, z; };
struct Double4 { double x, y, z, w; };
struct Int2    { int32_t x, y; };
struct Int3    { int32_t x, y, z; };
struct Int4    { int32_t x, y, z, w; };
struct Quaternion { double x, y, z, w; };            /* stored x, y, z, w */
struct Color   { uint8_t r, g, b, a; };              /* sRGB, straight alpha */
struct Rect    { float x, y, w, h; };
struct RectI   { int32_t x, y, w, h; };
struct Pose    { Double3 position; Quaternion orientation; };   /* meters, radians */
struct Twist   { Double3 linear, angular; };                    /* m/s, rad/s */
struct GeoPoint{ double lat, lon, alt; };                       /* degrees, degrees, meters */
struct Uuid    { uint8_t bytes[16]; };                          /* RFC 4122 byte order */
struct Matrix3x3 { float m[9];  };                              /* row-major */
struct Matrix4x4 { float m[16]; };                              /* row-major */
struct Uri     { String<256> value; };
/* microseconds since the Unix epoch, UTC -- the clock Message::written_us is stamped from */
struct Timestamp { int64_t us = 0; };
struct Duration  { int64_t us = 0; };

inline bool operator==(Timestamp a, Timestamp b) { return a.us == b.us; }
inline bool operator<(Timestamp a, Timestamp b) { return a.us < b.us; }
inline Duration  operator-(Timestamp a, Timestamp b) { return Duration{ a.us - b.us }; }
inline Timestamp operator+(Timestamp t, Duration d) { return Timestamp{ t.us + d.us }; }
inline bool operator==(Duration a, Duration b) { return a.us == b.us; }
inline Duration milliseconds(int64_t ms) { return Duration{ ms * 1000 }; }
inline Duration seconds(double s) { return Duration{ (int64_t)(s * 1000000.0) }; }
inline double seconds_of(Duration d) { return (double)d.us / 1000000.0; }
/* 0xRRGGBBAA both ways, the order you read in CSS */
inline Color color_from_hex(uint32_t rgba) {
    return Color{ (uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8), (uint8_t)rgba };
}
inline uint32_t color_to_hex(Color c) {
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | c.a;
}
inline Double3 operator+(Double3 a, Double3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Double3 operator-(Double3 a, Double3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Double3 operator*(Double3 a, double k)  { return { a.x * k, a.y * k, a.z * k }; }
inline double  dot(Double3 a, Double3 b)  { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Double3 cross(Double3 a, Double3 b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline double  length(Double3 a) { return detail::dart_double3_length({ a.x, a.y, a.z }); }
inline Double3 normalize(Double3 a) {
    detail::DartDouble3 n = detail::dart_double3_normalize({ a.x, a.y, a.z });
    return { n.x, n.y, n.z };
}
inline Quaternion identity_rotation() { return { 0.0, 0.0, 0.0, 1.0 }; }
inline Quaternion conjugate(Quaternion q) { return { -q.x, -q.y, -q.z, q.w }; }
/* a then b: the rotation b applied after a */
inline Quaternion operator*(Quaternion a, Quaternion b) {
    return { b.w * a.x + b.x * a.w + b.y * a.z - b.z * a.y,
             b.w * a.y - b.x * a.z + b.y * a.w + b.z * a.x,
             b.w * a.z + b.x * a.y - b.y * a.x + b.z * a.w,
             b.w * a.w - b.x * a.x - b.y * a.y - b.z * a.z };
}
inline Double3 rotate(Quaternion q, Double3 v) {
    detail::DartDouble3 r = detail::dart_quaternion_rotate({ q.x, q.y, q.z, q.w }, { v.x, v.y, v.z });
    return { r.x, r.y, r.z };
}
inline Pose identity_pose() { return { { 0.0, 0.0, 0.0 }, identity_rotation() }; }
inline bool is_nil(const Uuid& u) {
    for (int i = 0; i < 16; i++) if (u.bytes[i]) return false;
    return true;
}
/* The two values that need a platform (the node runtime provides them). */
inline Timestamp now() { return Timestamp{ detail::dart_timestamp_now() }; }
inline Uuid      new_uuid() { Uuid u; detail::dart_uuid_new((detail::DartUuid*)&u); return u; }

/* Give a struct-shaped type a wire name: reflect its members, then name it. */
#define DART_STD_STRUCT(T, ...) \
    template <> struct dart::reflect<dart::T> { \
        using is_dart_schema = void; \
        static constexpr const char* type_name = #T; \
        template <class V> static void visit(V&& v) { DART_STD_MEMBERS_##T(v) } \
    }; \
    template <> struct dart::std_type<dart::T> { \
        static constexpr const char* name = #T; using repr = void; }
/* Give an ALIAS-shaped type a wire name: it copies as R (`Uuid = u8[16]`, R = uint8_t[16]). */
#define DART_STD_ALIAS(T, R) \
    template <> struct dart::std_type<dart::T> { \
        static constexpr const char* name = #T; using repr = R; }

#define DART_STD_F(T, f) v(::dart::field_tag<decltype(::dart::T::f)>{}, #f, offsetof(::dart::T, f));
#define DART_STD_MEMBERS_Float2(v)  DART_STD_F(Float2,x)  DART_STD_F(Float2,y)
#define DART_STD_MEMBERS_Float3(v)  DART_STD_F(Float3,x)  DART_STD_F(Float3,y)  DART_STD_F(Float3,z)
#define DART_STD_MEMBERS_Float4(v)  DART_STD_F(Float4,x)  DART_STD_F(Float4,y)  DART_STD_F(Float4,z)  DART_STD_F(Float4,w)
#define DART_STD_MEMBERS_Double2(v) DART_STD_F(Double2,x) DART_STD_F(Double2,y)
#define DART_STD_MEMBERS_Double3(v) DART_STD_F(Double3,x) DART_STD_F(Double3,y) DART_STD_F(Double3,z)
#define DART_STD_MEMBERS_Double4(v) DART_STD_F(Double4,x) DART_STD_F(Double4,y) DART_STD_F(Double4,z) DART_STD_F(Double4,w)
#define DART_STD_MEMBERS_Int2(v)    DART_STD_F(Int2,x)    DART_STD_F(Int2,y)
#define DART_STD_MEMBERS_Int3(v)    DART_STD_F(Int3,x)    DART_STD_F(Int3,y)    DART_STD_F(Int3,z)
#define DART_STD_MEMBERS_Int4(v)    DART_STD_F(Int4,x)    DART_STD_F(Int4,y)    DART_STD_F(Int4,z)    DART_STD_F(Int4,w)
#define DART_STD_MEMBERS_Quaternion(v) DART_STD_F(Quaternion,x) DART_STD_F(Quaternion,y) DART_STD_F(Quaternion,z) DART_STD_F(Quaternion,w)
#define DART_STD_MEMBERS_Color(v)   DART_STD_F(Color,r)   DART_STD_F(Color,g)   DART_STD_F(Color,b)   DART_STD_F(Color,a)
#define DART_STD_MEMBERS_Rect(v)    DART_STD_F(Rect,x)    DART_STD_F(Rect,y)    DART_STD_F(Rect,w)    DART_STD_F(Rect,h)
#define DART_STD_MEMBERS_RectI(v)   DART_STD_F(RectI,x)   DART_STD_F(RectI,y)   DART_STD_F(RectI,w)   DART_STD_F(RectI,h)
#define DART_STD_MEMBERS_Pose(v)    v(::dart::field_tag<::dart::Double3>{}, "position", offsetof(::dart::Pose, position)); \
                                    v(::dart::field_tag<::dart::Quaternion>{}, "orientation", offsetof(::dart::Pose, orientation));
#define DART_STD_MEMBERS_Twist(v)   v(::dart::field_tag<::dart::Double3>{}, "linear", offsetof(::dart::Twist, linear)); \
                                    v(::dart::field_tag<::dart::Double3>{}, "angular", offsetof(::dart::Twist, angular));
#define DART_STD_MEMBERS_GeoPoint(v) DART_STD_F(GeoPoint,lat) DART_STD_F(GeoPoint,lon) DART_STD_F(GeoPoint,alt)

namespace priv {

template <class> inline constexpr bool always_false = false;

template <class U> struct is_dart_string : std::false_type {};
template <uint16_t N> struct is_dart_string<String<N>> : std::true_type {
    static constexpr uint16_t cap = N;
};
template <class U> struct is_std_array : std::false_type {};
template <class E, size_t N> struct is_std_array<std::array<E, N>> : std::true_type {
    using elem = E;
    static constexpr size_t count = N;
};
template <class U, class = void> struct is_reflected : std::false_type {};
template <class U> struct is_reflected<U, std::void_t<typename reflect<U>::is_dart_schema>>
    : std::true_type {};
/* an enum type registered with DART_ENUM (else a plain enum ships as its backing integer) */
template <class U, class = void> struct is_reg_enum : std::false_type {};
template <class U> struct is_reg_enum<U, std::void_t<typename reflect_enum<U>::is_dart_enum>>
    : std::true_type {};
/* std::string as a handle's T: the unbounded `string` root (a tail frame, not a slot) */
template <class U> struct is_var_string : std::is_same<U, std::string> {};

/* map a C++ scalar type onto the wire kind; -1 = not a wire scalar */
template <class U> constexpr int scalar_kind_of() {
    if constexpr (std::is_same_v<U, bool>) return (int)detail::DART_BOOL;
    else if constexpr (std::is_integral_v<U>) {
        if constexpr (sizeof(U) == 1) return std::is_signed_v<U> ? (int)detail::DART_I8  : (int)detail::DART_U8;
        else if constexpr (sizeof(U) == 2) return std::is_signed_v<U> ? (int)detail::DART_I16 : (int)detail::DART_U16;
        else if constexpr (sizeof(U) == 4) return std::is_signed_v<U> ? (int)detail::DART_I32 : (int)detail::DART_U32;
        else if constexpr (sizeof(U) == 8) return std::is_signed_v<U> ? (int)detail::DART_I64 : (int)detail::DART_U64;
        else return -1;
    }
    else if constexpr (std::is_floating_point_v<U>) {
        if constexpr (sizeof(U) == 4) return (int)detail::DART_F32;
        else if constexpr (sizeof(U) == 8) return (int)detail::DART_F64;
        else return -1;
    }
    else return -1;
}

inline const char* scalar_kind_name(int k) {
    static const char* names[] = { "u8","u16","u32","u64","i8","i16","i32","i64","f32","f64","bool" };
    return (k >= 0 && k <= (int)detail::DART_BOOL) ? names[k] : "?";
}
inline uint32_t scalar_wire_size(int k) {
    static const uint8_t sz[] = { 1,2,4,8,1,2,4,8,4,8,1 };
    return (k >= 0 && k <= (int)detail::DART_BOOL) ? sz[k] : 0;
}
inline bool host_le() {
    const uint16_t probe = 1;
    return *reinterpret_cast<const uint8_t*>(&probe) == 1;
}

/* one leaf field of the flattened struct: where it lives in the struct, where on the
 * wire, and how to copy it. Arrays are one leaf with count > 1 (per-element strides). */
struct Leaf {
    uint32_t    struct_off = 0, wire_off = 0;
    uint8_t     kind = 0;                       /* scalar kind, DART_STR, or (enum) the backing kind */
    uint8_t     is_array = 0;
    uint8_t     is_enum = 0;                     /* wire kind is ENUM; copies as its backing integer (kind) */
    uint16_t    count = 1, cap = 0;             /* elements; string capacity */
    uint32_t    s_stride = 0, w_stride = 0;     /* per-element byte strides */
    std::string path;                           /* dotted path for by-name offset lookup */
};

struct SchemaBuilder {
    std::string       dsl;
    std::vector<Leaf> leaves;
    uint32_t          base = 0;
    std::string       prefix;
    bool              first = true;
    bool              value_root = false;   /* the schema IS one bare type: no name, no braces */
    int               quiet = 0;            /* > 0: emit leaves only (inside a NAMED type,
                                               whose spelling is just its name) */

    void put(const char* s)        { if (!quiet) dsl += s; }
    void put(const std::string& s) { if (!quiet) dsl += s; }
    void sep(const char* name) {
        if (value_root || quiet) return;    /* a bare type spells only itself */
        if (!first) dsl += ", ";
        first = false;
        dsl += name;
        dsl += ": ";
    }
    template <class E> void add_scalar(const char* name, size_t off, size_t count, bool arr) {
        constexpr int k = scalar_kind_of<E>();
        static_assert(k >= 0, "DART_SCHEMA: field/element type is not a wire scalar");
        static_assert(!std::is_same_v<E, bool> || sizeof(bool) == 1, "bool must be one byte");
        sep(name);
        put(scalar_kind_name(k));
        if (arr) { put("["); put(std::to_string(count)); put("]"); }
        Leaf l;
        l.struct_off = base + (uint32_t)off;
        l.kind = (uint8_t)k; l.is_array = arr ? 1 : 0;
        l.count = (uint16_t)count;
        l.s_stride = (uint32_t)sizeof(E);
        l.w_stride = scalar_wire_size(k);
        l.path = prefix + name;
        leaves.push_back(std::move(l));
    }
    template <class E> void add_string(const char* name, size_t off, size_t count, bool arr) {
        constexpr uint16_t cap = is_dart_string<E>::cap;
        sep(name);
        put("string<"); put(std::to_string(cap)); put(">");
        if (arr) { put("["); put(std::to_string(count)); put("]"); }
        Leaf l;
        l.struct_off = base + (uint32_t)off;
        l.kind = (uint8_t)detail::DART_STR; l.is_array = arr ? 1 : 0;
        l.count = (uint16_t)count; l.cap = cap;
        l.s_stride = (uint32_t)sizeof(E);
        l.w_stride = 2u + cap;
        l.path = prefix + name;
        leaves.push_back(std::move(l));
    }
    /* a DART_ENUM-registered enum: emit `enum<uN> { A=v, ... }`, copy as the backing int */
    template <class E> void add_enum(const char* name, size_t off) {
        using Backing = std::underlying_type_t<E>;
        constexpr int k = scalar_kind_of<Backing>();
        static_assert(k >= 0 && k <= (int)detail::DART_I64,
                      "DART_ENUM: backing must be an integer type (u8..i64)");
        sep(name);
        put("enum<"); put(scalar_kind_name(k)); put("> { ");
        bool firstv = true;
        reflect_enum<E>::visit([&](int64_t value, const char* vn) {
            if (!firstv) put(", ");
            firstv = false;
            put(vn); put("="); put(std::to_string(value));
        });
        put(" }");
        Leaf l;
        l.struct_off = base + (uint32_t)off;
        l.kind = (uint8_t)k; l.is_enum = 1;
        l.s_stride = (uint32_t)sizeof(E);
        l.w_stride = scalar_wire_size(k);
        l.path = prefix + name;
        leaves.push_back(std::move(l));
    }
    template <class U> void add(const char* name, size_t off);
};

struct SchemaVisit {
    SchemaBuilder* b;
    template <class U> void operator()(field_tag<U>, const char* name, size_t off) const {
        b->add<U>(name, off);
    }
};

template <class U> void SchemaBuilder::add(const char* name, size_t off) {
    if constexpr (scalar_kind_of<U>() >= 0) {
        add_scalar<U>(name, off, 1, false);
    } else if constexpr (std::is_enum_v<U>) {
        if constexpr (is_reg_enum<U>::value) add_enum<U>(name, off);   /* named options on the wire */
        else add_scalar<std::underlying_type_t<U>>(name, off, 1, false); /* plain int (no DART_ENUM) */
    } else if constexpr (is_dart_string<U>::value) {
        add_string<U>(name, off, 1, false);
    } else if constexpr (std::is_array_v<U>) {
        using E = std::remove_extent_t<U>;
        constexpr size_t n = std::extent_v<U>;
        if constexpr (is_dart_string<E>::value) add_string<E>(name, off, n, true);
        else                                    add_scalar<E>(name, off, n, true);
    } else if constexpr (is_std_array<U>::value) {
        using E = typename is_std_array<U>::elem;
        constexpr size_t n = is_std_array<U>::count;
        if constexpr (is_dart_string<E>::value) add_string<E>(name, off, n, true);
        else                                    add_scalar<E>(name, off, n, true);
    } else if constexpr (std_type<U>::name != nullptr) {
        /* a NAMED type: it spells as its name alone, but still contributes the leaves of
           whatever it wraps, so the copy loops and the memcpy fast path are unchanged */
        sep(name);
        put(std_type<U>::name);
        uint32_t    saved_base   = base;
        std::string saved_prefix = prefix;
        base = base + (uint32_t)off;
        ++quiet;
        if constexpr (std::is_void_v<typename std_type<U>::repr>) {
            prefix = prefix + name + ".";          /* struct-shaped: its members are fields */
            reflect<U>::visit(SchemaVisit{ this });
        } else {
            base = saved_base;                     /* alias-shaped: one leaf, at this offset */
            add<typename std_type<U>::repr>(name, off);
        }
        --quiet;
        base   = saved_base;
        prefix = std::move(saved_prefix);
    } else if constexpr (is_reflected<U>::value) {
        /* nested reflected struct: inline its fields one level deeper */
        if (!first) dsl += ", ";
        first = false;
        dsl += name;
        dsl += ": { ";
        uint32_t    saved_base   = base;
        std::string saved_prefix = prefix;
        base   = base + (uint32_t)off;
        prefix = prefix + name + ".";
        first  = true;
        reflect<U>::visit(SchemaVisit{ this });
        dsl += " }";
        base   = saved_base;
        prefix = std::move(saved_prefix);
        first  = false;
    } else {
        static_assert(always_false<U>,
            "DART_SCHEMA: unsupported field type (no std::string/std::vector/pointer/map; "
            "use dart::String<N>, arrays, scalars, or a nested DART_SCHEMA struct)");
    }
}

/* resolve every leaf's wire offset by dotted-path lookup in a compiled schema,
 * verifying the kinds match. Used on our own schema at registration and on an
 * incoming (publisher-layout) schema for the subset/rebase decode. */
inline bool fill_offsets(const detail::DartSchema* s, std::vector<Leaf>& lv) {
    for (Leaf& l : lv) {
        int idx = detail::dart_schema_field_index(s, l.path.c_str());
        if (idx < 0) return false;
        detail::DartSchemaFieldInfo fi;
        if (!detail::dart_schema_field_at(s, (uint16_t)idx, &fi)) return false;
        if (l.is_enum) {
            if (fi.kind != (uint8_t)detail::DART_ENUM || fi.elem != l.kind) return false;  /* same backing width */
        } else if (l.is_array) {
            if (fi.kind != (uint8_t)detail::DART_ARR || fi.elem != l.kind || fi.count != l.count) return false;
            if (l.kind == (uint8_t)detail::DART_STR && fi.str_cap != l.cap) return false;
        } else if (l.kind == (uint8_t)detail::DART_STR) {
            if (fi.kind != (uint8_t)detail::DART_STR || fi.str_cap != l.cap) return false;
        } else {
            if (fi.kind != l.kind) return false;
        }
        l.wire_off = fi.offset;
    }
    return true;
}

inline void copy_swapped(uint8_t* dst, const uint8_t* src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[n - 1 - i];
}

inline void leaf_to_wire(const Leaf& l, const uint8_t* sbase, uint8_t* wire, bool le) {
    const uint8_t* sp = sbase + l.struct_off;
    uint8_t*       wp = wire + l.wire_off;
    for (uint16_t i = 0; i < l.count; i++, sp += l.s_stride, wp += l.w_stride) {
        if (l.kind == (uint8_t)detail::DART_STR) {
            uint16_t len;
            std::memcpy(&len, sp, 2);
            if (len > l.cap) len = l.cap;
            if (le) std::memcpy(wp, &len, 2); else copy_swapped(wp, reinterpret_cast<uint8_t*>(&len), 2);
            std::memcpy(wp + 2, sp + 2, l.cap);
        } else if (le || l.w_stride == 1) {
            std::memcpy(wp, sp, l.w_stride);
        } else {
            copy_swapped(wp, sp, l.w_stride);
        }
    }
}
inline void leaf_from_wire(const Leaf& l, uint8_t* sbase, const uint8_t* wire, bool le) {
    uint8_t*       sp = sbase + l.struct_off;
    const uint8_t* wp = wire + l.wire_off;
    for (uint16_t i = 0; i < l.count; i++, sp += l.s_stride, wp += l.w_stride) {
        if (l.kind == (uint8_t)detail::DART_STR) {
            uint16_t len;
            if (le) std::memcpy(&len, wp, 2); else copy_swapped(reinterpret_cast<uint8_t*>(&len), wp, 2);
            if (len > l.cap) len = l.cap;   /* clamp a hostile length prefix */
            std::memcpy(sp, &len, 2);
            std::memcpy(sp + 2, wp + 2, l.cap);
        } else if (le || l.w_stride == 1) {
            std::memcpy(sp, wp, l.w_stride);
        } else {
            copy_swapped(sp, wp, l.w_stride);
        }
    }
}

/* T is a BARE WIRE TYPE: usable as a handle's whole schema with no DART_SCHEMA, since a
 * bare type's identity is its shape. std::string is the unbounded `string` root, handled
 * on its own path (a tail frame has no fixed leaf). */
template <class U> constexpr bool is_value_type() {
    return scalar_kind_of<U>() >= 0 || std::is_enum_v<U> || is_dart_string<U>::value
        || is_std_array<U>::value || is_var_string<U>::value;
}

/* the per-T registration: schema + copy table + rebase cache, built once, immortal */
struct TypeCodec {
    bool                      ok = false;
    bool                      memcpy_ok = false;      /* our own layout coincides with our wire */
    bool                      memcpy_capable = false; /* trivially copyable + little-endian host */
    bool                      var_string = false;     /* T is std::string: the whole message is one frame */
    std::optional<Schema>     schema;
    const detail::DartSchema* raw = nullptr;
    uint64_t                  hash = 0;
    uint32_t                  wire_size = 0;
    uint32_t                  struct_size = 0;
    std::vector<Leaf>         leaves;

    /* one incoming schema pointer's resolved offsets. A rebased schema keeps the
     * SUBSCRIBER's wire (hence its hash) with the PUBLISHER's offsets, so the hash can
     * never discriminate layouts: offsets are always re-resolved per schema pointer,
     * and the memcpy fast path fires per pointer when they coincide with the struct. */
    struct Rebased { std::vector<Leaf> leaves; uint32_t size = 0; bool ok = false, coincide = false; };
    std::mutex                                       mu;
    std::map<const detail::DartSchema*, Rebased>     rebased;

    const Rebased* rebased_for(const detail::DartSchema* s) {
        std::lock_guard<std::mutex> g(mu);
        auto it = rebased.find(s);
        if (it != rebased.end()) return &it->second;
        Rebased r;
        r.leaves = leaves;                    /* keep struct offsets, refill wire offsets */
        r.ok     = fill_offsets(s, r.leaves);
        r.size   = detail::dart_schema_size(s);
        if (r.ok && memcpy_capable && r.size >= struct_size) {
            bool co = true;
            for (const Leaf& l : r.leaves)
                co = co && l.struct_off == l.wire_off && l.s_stride == l.w_stride;
            r.coincide = co;
        }
        return &rebased.emplace(s, std::move(r)).first->second;
    }
};

inline std::string strip_namespaces(const char* type_name) {
    std::string n(type_name);
    size_t p = n.rfind(':');
    return p == std::string::npos ? n : n.substr(p + 1);
}

template <class T> TypeCodec* build_codec() {
    static_assert(std_type<T>::name != nullptr || is_reflected<T>::value || is_value_type<T>(),
                  "type has no DART_SCHEMA(T, fields...) declaration and is not a bare wire type");
    auto* c = new TypeCodec();
    SchemaBuilder b;
    if constexpr (std_type<T>::name != nullptr) {
        b.dsl = std_type<T>::name;              /* the standard type IS the whole schema */
        ++b.quiet;
        if constexpr (std::is_void_v<typename std_type<T>::repr>)
            reflect<T>::visit(SchemaVisit{ &b });          /* fields at the root: "position.x" */
        else
            b.add<typename std_type<T>::repr>("", 0);      /* an alias root: the empty path */
        --b.quiet;
    } else if constexpr (is_reflected<T>::value) {
        b.dsl = strip_namespaces(reflect<T>::type_name);
        b.dsl += " { ";
        reflect<T>::visit(SchemaVisit{ &b });
        b.dsl += " }";
    } else if constexpr (is_var_string<T>::value) {
        b.dsl = "string";                       /* one tail frame: no fixed leaf to copy */
        c->var_string = true;
    } else {
        b.value_root = true;                    /* a bare type: the schema is its spelling alone */
        b.add<T>("", 0);
    }
    c->leaves = std::move(b.leaves);
    c->schema = Schema::compile(b.dsl);
    if (!c->schema) return c;
    c->raw         = c->schema->raw();
    c->hash        = c->schema->hash();
    c->wire_size   = detail::dart_schema_size(c->raw);
    c->struct_size = (uint32_t)sizeof(T);
    if (!fill_offsets(c->raw, c->leaves)) return c;
    c->memcpy_capable = std::is_trivially_copyable<T>::value && host_le();
    bool coincide = c->memcpy_capable && sizeof(T) == c->wire_size;
    for (const Leaf& l : c->leaves)
        coincide = coincide && l.struct_off == l.wire_off && l.s_stride == l.w_stride;
    c->memcpy_ok = coincide;
    c->ok = true;
    return c;
}

template <class T> TypeCodec& type_codec() {
    static TypeCodec* c = build_codec<T>();   /* immortal: schemas are referenced by topics */
    return *c;
}

/* the compiled Schema for T (registers on first use); nullptr if the synthesized
 * DSL failed to compile (a codec bug, surfaced by the handle constructors) */
template <class T> const Schema* schema_of() {
    TypeCodec& c = type_codec<T>();
    return c.ok ? &*c.schema : nullptr;
}

/* struct -> wire. The memcpy fast path returns a view of the struct itself (zero
 * copy); otherwise the field loop packs into scratch. Empty Bytes = codec invalid. */
template <class T> Bytes encode(const T& v, std::vector<uint8_t>& scratch) {
    TypeCodec& c = type_codec<T>();
    if (!c.ok) return Bytes();
    if constexpr (is_var_string<T>::value) {              /* `string` root: one [u32 len] frame */
        scratch.assign((size_t)detail::dart_schema_msg_min(c.raw) + v.size(), 0);
        detail::dart_schema_message_default(c.raw, scratch.data(), scratch.size());
        detail::DartString sv; sv.data = v.data(); sv.len = v.size();
        if (!detail::dart_set_string(scratch.data(), scratch.size(), c.raw, "", sv)) return Bytes();
        return Bytes(scratch.data(),
                     detail::dart_schema_msg_len(c.raw, scratch.data(), scratch.size()));
    } else {
        if (c.memcpy_ok) return Bytes(&v, sizeof(T));
        scratch.assign(c.wire_size, 0);
        const bool le = host_le();
        const uint8_t* base = reinterpret_cast<const uint8_t*>(&v);
        for (const Leaf& l : c.leaves) leaf_to_wire(l, base, scratch.data(), le);
        return Bytes(scratch.data(), scratch.size());
    }
}

/* wire -> struct. Offsets always come from the delivered schema (deliveries arrive in
 * the PUBLISHER's layout via the subset/rebase reader, and a rebased schema keeps our
 * hash, so the hash can never pick the layout); resolutions are cached per schema
 * pointer, with the memcpy fast path enabled per pointer when that layout coincides
 * with the struct. schema == nullptr assumes our own layout. */
template <class T> bool decode(T& out, Bytes data, const detail::DartSchema* schema) {
    TypeCodec& c = type_codec<T>();
    if (!c.ok) return false;
    if constexpr (is_var_string<T>::value) {              /* `string` root: read the frame */
        detail::DartBytes mb; mb.data = data.data(); mb.len = data.size();
        detail::DartString sv = detail::dart_get_string(mb, schema ? schema : c.raw, "");
        if (!sv.data) { out.clear(); return false; }
        out.assign(sv.data, sv.len);
        return true;
    } else {
        const std::vector<Leaf>* lv = &c.leaves;
        uint32_t need = c.wire_size;
        bool fast = c.memcpy_ok;
        if (schema && schema != c.raw) {
            const TypeCodec::Rebased* rb = c.rebased_for(schema);
            if (!rb->ok) return false;
            lv = &rb->leaves;
            need = rb->size;
            fast = rb->coincide;
        }
        if (fast) {
            if (data.size() < sizeof(T)) return false;
            std::memcpy(&out, data.data(), sizeof(T));
            return true;
        }
        if (data.size() < need) return false;
        const bool le = host_le();
        uint8_t* base = reinterpret_cast<uint8_t*>(&out);
        for (const Leaf& l : *lv) leaf_from_wire(l, base, data.data(), le);
        return true;
    }
}

}   /* namespace priv */

/* Topic: the untyped dynamic topic (explicit role, Bytes payloads). The typed
 * Publisher<T>/Subscriber<T> sugar is built over it. Handles are thin and non-owning:
 * the topic itself lives in the Node until close. Same-name constructions on one node
 * share the underlying topic slot and the role widens (the node keys topics by name). */
class Topic {
public:
    Topic() = default;
    /* Create (or share) a topic on `node`. Throws dart::Error on failure (with
     * -fno-exceptions: check valid()). schema is copied into the node. */
    Topic(Node& node, std::string_view name, Role role = Role::PubSub,
          const Schema* schema = nullptr, const Qos& qos = {});

    bool valid() const noexcept { return ch_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    SendStatus send(Bytes data) {
        if (!ch_) return SendStatus::NoTopic;
        return static_cast<SendStatus>(
            detail::dart_topic_send(ch_, detail::dart_bytes(data.data(), data.size())));
    }
    SendStatus set_role(Role r) {
        if (!ch_) return SendStatus::NoTopic;
        return static_cast<SendStatus>(
            detail::dart_topic_set_role(ch_, static_cast<detail::DartRole>(r)));
    }
    /* RETIRE the topic: the lifecycle verb for re-creating a name with a different
     * schema (a retype). The topic leaves the announce, every lane tears down, and its
     * slot parks for reuse by a later create, so retire/create cycles never grow the
     * node. This handle (and every other handle sharing the slot) is INVALID after
     * SendStatus::Ok; the wrapper's name cache forgets the name, so the next
     * Topic(node, name, ...) creates fresh (with a new schema if desired). Refused with
     * SendStatus::State from a callback, for builtin topics, and mid-dispatch. */
    SendStatus retire();
    uint16_t index() const {
        if (!ch_) return 0xffff;
        return detail::dart_topic_index(ch_);
    }
    int match_count() const {
        if (!ch_) return 0;
        return detail::dart_topic_match_count(ch_);
    }
    /* Unresolved candidate matches right now (peers nominated by hash whose verdicts
     * are still in flight); 0 = matching has converged for everyone currently known. */
    int pending_count() const {
        if (!ch_) return 0;
        return detail::dart_topic_pending_count(ch_);
    }
    /* 1 when a send would not wait: a subscriber is matched, or matching has converged
     * so there is nobody to wait for. The async form of the send-path match wait. */
    bool ready() const {
        if (!ch_) return false;
        return detail::dart_topic_ready(ch_) == 1;
    }
    /* Pump until every reader has acked, or timeout_ms elapses. Call before
     * closing so a final burst is not cut off by the BYE. */
    bool drain(int timeout_ms) {
        if (!ch_) return true;
        return detail::dart_topic_drain(ch_, timeout_ms) == 1;
    }

    /* ---- consumer queue (take / dispatch) ------------------------------------------
     * The first take()/dispatch() switches this topic to QUEUED delivery: its
     * messages then queue instead of firing the node handler on the poll thread, and
     * exactly one thread of your choosing consumes them here (per topic). The queue
     * grows on demand to Qos::queue_bytes (0 = 1 MB); at the cap a best-effort topic
     * overwrites oldest (Event MsgLost), a reliable one backpressures the publisher.
     * Waiting works everywhere: alongside start()/a poller it sleeps, otherwise it
     * drives the poll loop itself. */

    /* Pop the next queued message. timeout_ms: 0 = just check, >0 = wait up to that
     * long, negative = wait indefinitely. Empty optional = nothing arrived in time.
     *     while (auto msg = scan.take()) render(msg->data()); */
    std::optional<Message<>> take(int timeout_ms = 0) {
        Message<> t;
        if (!ch_ || detail::dart_topic_take(ch_, &t.m_, timeout_ms) != 1) return std::nullopt;
        t.bind();
        return t;
    }
    /* Drain the queue by running the node's message handler on the CALLING thread,
     * oldest first: up to max_msgs of those queued at entry (0 = all), waiting up to
     * timeout_ms for the first like take. Returns messages dispatched. Unlike
     * poll-thread handlers, these run without the node lock and may use the full API. */
    int dispatch(int max_msgs = 0, int timeout_ms = 0) {
        if (!ch_) return 0;
        return detail::dart_topic_dispatch(ch_, max_msgs, timeout_ms);
    }
    struct QueueStats { uint32_t messages = 0, bytes = 0, capacity = 0, dropped = 0; };
    QueueStats queue_stats() const {
        QueueStats s;
        if (ch_) detail::dart_topic_queue_stats(ch_, &s.messages, &s.bytes, &s.capacity, &s.dropped);
        return s;
    }

    /* Cumulative traffic counters (always on): messages/bytes this node committed to the
     * topic (tx) and delivered from it (rx). Also carried in the @dart/meta snapshot. */
    struct Counts { uint64_t tx_msgs = 0, tx_bytes = 0, rx_msgs = 0, rx_bytes = 0; };
    Counts counts() const {
        Counts c;
        if (ch_) detail::dart_topic_counts(ch_, &c.tx_msgs, &c.tx_bytes, &c.rx_msgs, &c.rx_bytes);
        return c;
    }

private:
    explicit Topic(detail::DartTopic* c, void* impl = nullptr) : ch_(c), impl_(impl) {}
    detail::DartTopic* ch_ = nullptr;
    void* impl_ = nullptr;   /* the owning Node::Impl (Node is incomplete here), so
                                retire() can forget the wrapper's name-cache entry */
    friend class Node;
};

#ifndef DART_NO_PATTERNS

/* Deferred<Rsp> / Deferred<>: a parked function reply (from Request::defer). Movable,
 * single-shot; complete()/fail() may be called from any thread. Dropping it without
 * completing leaves the caller to its timeout. */
template <> class Deferred<void> {
public:
    Deferred() = default;
    Deferred(Deferred&& o) noexcept : fn_(o.fn_), token_(o.token_) { o.fn_ = nullptr; o.token_ = 0; }
    Deferred& operator=(Deferred&& o) noexcept {
        if (this != &o) { fn_ = o.fn_; token_ = o.token_; o.fn_ = nullptr; o.token_ = 0; }
        return *this;
    }
    Deferred(const Deferred&) = delete;
    Deferred& operator=(const Deferred&) = delete;

    bool valid() const noexcept { return fn_ != nullptr && token_ != 0; }
    explicit operator bool() const noexcept { return valid(); }

    /* message: optional human-readable outcome text (ResponseView::message on the caller;
     * truncated at DART_CALL_MSG_MAX). On fail it is what a generic consumer displays. */
    bool complete(Bytes rsp = Bytes(), std::string_view message = {}) {
        return finish(detail::DART_CALL_OK, message, rsp);
    }
    bool fail(std::string_view message = {}, Bytes rsp = Bytes()) {
        return finish(detail::DART_CALL_APP_ERROR, message, rsp);
    }

private:
    Deferred(detail::DartFunction* fn, uint64_t token) : fn_(fn), token_(token) {}
    bool finish(int status, std::string_view message, Bytes rsp) {
        if (!valid()) return false;
        std::string m(message);   /* the C API takes a NUL-terminated string */
        int r = detail::dart_function_complete(fn_, token_,
                    static_cast<detail::DartCallStatus>(status),
                    m.empty() ? nullptr : m.c_str(), priv::to_c(rsp));
        fn_ = nullptr; token_ = 0;
        return r == 0;
    }
    detail::DartFunction* fn_ = nullptr;
    uint64_t              token_ = 0;
    template <class R> friend class Request;
};

/* Request<> (untyped): the request as seen by a function definition's handler. Wraps
 * the exact DartRequest the callback received; views are valid for the callback only.
 * Reply exactly once (reply/fail), or defer() and complete later; returning without
 * replying auto-acks CallStatus::Ok with an empty payload. */
template <> class Request<void> : public FieldView {
public:
    std::string_view function_name() const { return { rq_->function_name.data, rq_->function_name.len }; }
    uint32_t         caller()        const { return rq_->caller; }
    std::string_view caller_name()   const { return { rq_->caller_name.data, rq_->caller_name.len }; }
    uint64_t         recv_us()       const { return rq_->recv_us; }
    uint64_t         written_us()       const { return rq_->written_us; }   /* the caller's write stamp */

    void reply(Bytes rsp)       { detail::dart_request_reply(rq_, priv::to_c(rsp)); }
    /* message: the human-readable failure reason (ResponseView::message on the caller,
     * truncated at DART_CALL_MSG_MAX; empty = the default "app error"). rsp may still
     * carry structured failure data beside it. */
    void fail(std::string_view message = {}, Bytes rsp = {}) {
        std::string m(message);
        detail::dart_request_fail(rq_, m.empty() ? nullptr : m.c_str(), priv::to_c(rsp));
    }
    /* Park the reply: suppresses the auto-ack and lets the handler return now; the
     * returned Deferred completes the call later, from any thread. */
    Deferred<> defer() { return Deferred<>(fn_, detail::dart_request_defer(rq_)); }

private:
    Request(detail::DartRequest* rq, detail::DartFunction* fn)
        : FieldView(rq->data, rq->schema), rq_(rq), fn_(fn) {}
    detail::DartRequest*  rq_;
    detail::DartFunction* fn_;
    template <class A, class B> friend class FunctionDefinition;
};

/* Response<> (untyped): an OWNING function-call outcome (from RemoteFunction<>::call);
 * the payload is copied out, so it outlives the call. send_status() carries a
 * synchronous refusal (a negative DartResult) when the call never launched. */
template <> class Response<void> {
public:
    Response() = default;
    CallStatus status()      const { return st_; }
    bool       ok()          const { return st_ == CallStatus::Ok; }
    uint32_t   provider()    const { return provider_; }
    SendStatus send_status() const { return ss_; }
    uint64_t   written_us()     const { return written_; }   /* the provider's write stamp (0 = synthesized) */
    Bytes      data()        const { return { data_.data(), data_.size() }; }
    /* human-readable outcome text (owned): the provider's message, or default status text
     * ("timeout", ...) on any answered/synthesized non-OK outcome. Empty on OK with no
     * message, and on a synchronous send refusal (send_status() carries that). */
    std::string_view message() const { return message_; }
    /* the schema data decodes with (interned in the node, valid until node close) */
    const detail::DartSchema* raw_schema() const { return schema_; }

private:
    CallStatus                st_ = CallStatus::Timeout;
    SendStatus                ss_ = SendStatus::Ok;
    uint32_t                  provider_ = 0;
    uint64_t                  written_ = 0;
    std::vector<uint8_t>      data_;
    std::string               message_;
    const detail::DartSchema* schema_ = nullptr;
    template <class A, class B> friend class RemoteFunction;
};

/* ResponseView<> (untyped): the async outcome, valid for the callback only. */
template <> class ResponseView<void> : public FieldView {
public:
    CallStatus status()   const { return static_cast<CallStatus>(r_->status); }
    bool       ok()       const { return r_->status == detail::DART_CALL_OK; }
    uint32_t   provider() const { return r_->provider; }
    uint64_t   written_us()  const { return r_->written_us; }   /* the provider's write stamp */
    /* human-readable outcome text (a view, callback lifetime): the provider's message, or
     * default status text on any non-OK outcome; empty only on OK with no message. */
    std::string_view message() const { return { r_->message.data, r_->message.len }; }

private:
    explicit ResponseView(const detail::DartResponse* r)
        : FieldView(r->data, r->schema), r_(r) {}
    const detail::DartResponse* r_;
    template <class A, class B> friend class RemoteFunction;
};

namespace priv {
/* one in-flight async call's callback; owned by the registry until the outcome fires */
struct AsyncBox {
    std::function<void(const ResponseView<>&)> cb;
    std::mutex*                mu;     /* the node Impl's registry lock */
    std::unordered_set<void*>* live;   /* the node Impl's outstanding-box set */
};
}

/* Section-mask bits for a @dart/meta request: OR them into Node::meta_request's
 * `sections` (0 = every section). */
enum MetaSection : uint32_t {
    MetaNode   = 0x1u,   /* uptime, memory, backpressure, peer/topic counts, last error */
    MetaProc   = 0x2u,   /* per-process cpu/rss (absent where the platform can't measure) */
    MetaTopics = 0x4u,   /* per-topic array: names, roles, match counts, traffic counters */
    MetaPeers  = 0x8u    /* per-peer array: id, name, address, match counts */
};

/* MetaSnapshot: a decoded @dart/meta reply, owned so it outlives the callback. The common
 * "node" and "proc" scalars are pulled out; the full self-describing body (including the
 * topics[] and peers[] arrays) stays in `info` as a MapDict for anything else. Absent
 * sections leave their fields zero (proc.have stays false where unmeasured). */
struct MetaSnapshot {
    bool       valid    = false;                 /* a CallStatus::Ok reply decoded */
    CallStatus status   = CallStatus::Timeout;
    uint32_t   provider = 0;                      /* the peer that answered */
    MapDict    info;                              /* the whole decoded body */

    struct NodeInfo {
        std::string name;
        uint64_t uptime_us = 0, wall_us = 0;
        uint64_t mem_in_use = 0, mem_peak = 0, alloc_calls = 0;
        uint64_t evicted_unsent = 0, bp_waited_us = 0, bp_waits = 0;
        uint64_t peers = 0, max_peers = 0, topics = 0, max_topics = 0;
        uint64_t shm_tx = 0, shm_rx = 0, last_error = 0;
        std::string last_error_text;
    } node;
    struct ProcInfo {
        bool have = false;
        bool have_cpu = false;
        uint64_t pid = 0, cpu_us = 0, rss = 0, peak_rss = 0;
        uint64_t heap_total = 0, heap_free = 0, heap_min_free = 0, heap_largest_free_block = 0;
    } proc;

    /* Decode from a raw response body + schema (the wrapper below feeds Response/
     * ResponseView). status/provider are carried through unchanged. */
    static MetaSnapshot decode(CallStatus st, uint32_t provider,
                               Bytes data, const detail::DartSchema* schema) {
        MetaSnapshot s;
        s.status = st; s.provider = provider;
        if (st != CallStatus::Ok || !schema) return s;
        detail::DartBytes info = detail::dart_get_map(detail::dart_bytes(data.data(), data.size()),
                                                      schema, "info");
        if (!info.data) return s;
        s.info = MapReader(Bytes{ info.data, info.len }).to_map();
        s.valid = true;
        auto it = s.info.find("node");
        if (it != s.info.end() && it->second.is_map()) {
            const MapDict& m = it->second.as_map();
            auto u = [&](const char* k){ auto j = m.find(k); return j == m.end() ? uint64_t(0) : j->second.as_uint(); };
            auto str = [&](const char* k){ auto j = m.find(k); return j == m.end() ? std::string() : j->second.as_string(); };
            s.node.name           = str("name");
            s.node.uptime_us      = u("uptime_us");
            s.node.wall_us        = u("wall_us");
            s.node.mem_in_use     = u("mem_in_use");
            s.node.mem_peak       = u("mem_peak");
            s.node.alloc_calls    = u("alloc_calls");
            s.node.evicted_unsent = u("evicted_unsent");
            s.node.bp_waited_us   = u("bp_waited_us");
            s.node.bp_waits       = u("bp_waits");
            s.node.peers          = u("peers");
            s.node.max_peers      = u("max_peers");
            s.node.topics         = u("topics");
            s.node.max_topics     = u("max_topics");
            s.node.shm_tx         = u("shm_tx");
            s.node.shm_rx         = u("shm_rx");
            s.node.last_error     = u("last_error");
            s.node.last_error_text= str("last_error_text");
        }
        it = s.info.find("proc");
        if (it != s.info.end() && it->second.is_map()) {
            const MapDict& m = it->second.as_map();
            auto u = [&](const char* k){ auto j = m.find(k); return j == m.end() ? uint64_t(0) : j->second.as_uint(); };
            s.proc.have     = true;
            s.proc.have_cpu = m.find("cpu_us") != m.end();
            s.proc.pid      = u("pid");
            s.proc.cpu_us   = u("cpu_us");
            s.proc.rss      = u("rss");
            s.proc.peak_rss = u("peak_rss");
            s.proc.heap_total = u("heap_total"); s.proc.heap_free = u("heap_free");
            s.proc.heap_min_free = u("heap_min_free");
            s.proc.heap_largest_free_block = u("heap_largest_free_block");
        }
        return s;
    }
    static MetaSnapshot decode(const Response<>& r) {
        return decode(r.status(), r.provider(), r.data(), r.raw_schema());
    }
    static MetaSnapshot decode(const ResponseView<>& r) {
        return decode(r.status(), r.provider(), r.data(), r.raw_schema());
    }
};

#endif /* !DART_NO_PATTERNS */

/* Node: owns the DartNode, its memory, and the user callbacks.
 *
 * Thread-safe: every call is serialized by a node-level lock inside the C core.
 * Drive it either by calling poll() from your own loop, or via start(): a C-level
 * background service thread owns the loop and handlers then fire on it (never two
 * at once for one node). From inside a handler, send() and read-only queries are
 * allowed; poll/topic create/set_role/drain/stop are refused (SendStatus::State
 * / no-op), never corrupting. */
class Node {
public:
    using MessageHandler = std::function<void(const MessageView&)>;
    using EventHandler   = std::function<void(const Event&)>;

    /* Open a node. name = a human-readable label synced via discovery (empty =>
     * an auto "node-XXXXXXXX"). on_message may be empty ({}: only per-subscriber
     * handlers and take/dispatch deliver); on_event is REQUIRED so no early
     * peer/error event is ever missed. Throws dart::Error on failure (with
     * -fno-exceptions: check valid() and last_open_error()). */
    Node(std::string_view name, MessageHandler on_message,
         EventHandler on_event, const NodeOptions& o = {}) {
        if (!on_event) { priv::raise_msg("dart::Node: an on_event handler is required"); return; }
        std::unique_ptr<Impl> impl(new Impl());
        impl->on_msg   = std::move(on_message);
        impl->on_event = std::move(on_event);
        /* The node retains the net string/seed pointers, so own that storage. */
        impl->disc_group = o.discovery_group;
        impl->mcast_if   = o.multicast_interface;
        impl->self_ip    = o.self_ip;
        for (const std::string& s : o.seed_peers) {
            detail::DartDiscoveryAddr a;
            if (parse_addr(s, a)) impl->seeds.push_back(a);
        }
        std::string nm(name);

        detail::DartNodeOpts co;
        std::memset(&co, 0, sizeof co);
        co.domain        = o.domain;
        co.max_topics    = o.max_topics;
        co.disable_shm   = o.disable_shm ? 1 : 0;
        co.fetch_details = o.fetch_details ? 1 : 0;
        co.match_wait_ms = o.match_wait_ms;
        co.disable_logs  = o.disable_logs ? 1 : 0;
        co.disable_meta  = o.disable_meta ? 1 : 0;
        co.disable_error_logs = o.disable_error_logs ? 1 : 0;
        co.user_data     = impl.get();
        co.net.data_port           = o.data_port;
        co.net.discovery_group     = impl->disc_group.empty() ? nullptr : impl->disc_group.c_str();
        co.net.discovery_port      = o.discovery_port;
        co.net.multicast_interface = impl->mcast_if.empty()   ? nullptr : impl->mcast_if.c_str();
        co.net.multicast_ttl       = o.multicast_ttl;
        co.net.seed_peers          = impl->seeds.empty() ? nullptr : impl->seeds.data();
        co.net.n_seed_peers        = static_cast<uint16_t>(impl->seeds.size());
        co.net.unicast_only        = o.unicast_only ? 1 : 0;
        co.net.self_ip             = impl->self_ip.empty() ? nullptr : impl->self_ip.c_str();
        co.net.advertise_port      = o.advertise_port;
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
        if (!n) { priv::raise_last(nullptr, "dart::Node open"); return; }
        impl->node = n;
        impl_ = std::move(impl);
    }

    bool valid() const noexcept { return impl_ != nullptr && impl_->node != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /* Why the most recent construction failed (also what a thrown dart::Error carries):
       the formatted one-line reason, and its machine-readable ErrorKind. */
    static std::string last_open_error() {
        detail::DartEvent e = detail::dart_last_error(nullptr);
        char b[192]; return detail::dart_event_str(&e, b, sizeof b);
    }
    static ErrorKind last_open_error_kind() {
        return static_cast<ErrorKind>(detail::dart_last_error(nullptr).error);
    }

    Node(Node&&) noexcept = default;
    Node& operator=(Node&&) noexcept = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    ~Node() = default;   /* teardown lives in Impl::~Impl (so a move-assign tears down correctly too);
                            it stops the service thread and closes with a BYE */

    /* Rebind a handler set at construction. Rarely needed. */
    Node& on_message(MessageHandler h) { if (impl_) impl_->on_msg = std::move(h);   return *this; }
    Node& on_event  (EventHandler   h) { if (impl_ && h) impl_->on_event = std::move(h); return *this; }

    /* Create (or share) a topic; equivalent to the Topic constructor. */
    Topic create_topic(std::string_view name, Role role = Role::PubSub,
                       const Schema* schema = nullptr, const Qos& qos = {});

    /* Recover an already-created topic handle by its creation index. */
    Topic topic(uint16_t index) const {
        if (!valid()) return Topic();
        return Topic(detail::dart_node_topic(impl_->node, index), impl_.get());
    }

    /* One loop tick: drives discovery, RX, timers, and flushes queued TX. Blocks
     * up to timeout_ms in the socket wait (wakes early on RX or a send from another
     * thread; 0 = non-blocking). Not needed (and refused) while start() runs. */
    int poll(int timeout_ms = 0) {
        if (!valid()) return (int)SendStatus::State;
        return detail::dart_node_poll(impl_->node, timeout_ms);
    }

    /* Run the C-level background service thread: it owns the loop and fires the
     * handlers; every Node/Topic call stays safe from any thread, and a send is
     * flushed immediately (a waker cuts the service's socket wait short). */
    bool start() { return valid() && detail::dart_node_start(impl_->node) == 0; }
    /* Stop and join the service thread (idempotent; implied by node teardown). */
    void stop()  { if (valid()) detail::dart_node_stop(impl_->node); }
    bool is_started() const { return valid() && detail::dart_node_is_started(impl_->node) == 1; }

    /* Block until discovery + matching settle: everything now sent reaches everyone
     * already on the network. Call AFTER creating your topics. Most apps never need
     * it (the per-send match wait covers the same window lazily). timeout_ms < 0 =
     * 3 announce intervals. Returns true when settled, false on timeout. */
    bool settle(int timeout_ms = -1) {
        return valid() && detail::dart_node_settle(impl_->node, timeout_ms) == 1;
    }

    /* Dispatch every already-queued topic on the calling thread (see Topic::take/
     * dispatch): the one-liner for a frame-paced consumer that owns all the queues.
     * Waits up to timeout_ms for any queued topic to hold data. */
    int dispatch(int max_msgs = 0, int timeout_ms = 0) {
        if (!valid()) return 0;
        return detail::dart_node_dispatch(impl_->node, max_msgs, timeout_ms);
    }

    /* The number of known peers, allocation free (safe from any callback). */
    uint16_t peer_count() const {
        if (!valid()) return 0;
        uint16_t count = 0;
        LockGuard guard(impl_->node);
        (void)detail::dart_node_peers(impl_->node, &count);
        return count;
    }

    /* A copied snapshot of the live peer table (safe to keep after the poll): peer
     * facts only. Use peer_entities(id) for what a peer advertises. */
    std::vector<Peer> peers() const {
        std::vector<Peer> out;
        if (!valid()) return out;
        uint16_t count = 0;
        /* the C view is zero-copy: hold the node lock across the copy so a poller
           on another thread cannot mutate it mid-read (no-op if we are that thread).
           RAII so a bad_alloc mid-copy cannot leak the lock and stall the node. */
        LockGuard guard(impl_->node);
        const detail::DartDiscoveryPeer* ps = detail::dart_node_peers(impl_->node, &count);
        for (uint16_t i = 0; ps && i < count; i++) {
            const detail::DartDiscoveryPeer& p = ps[i];
            Peer peer;
            peer.id            = p.id;
            peer.name          = p.name.data ? std::string(p.name.data, p.name.len) : std::string();
            peer.address       = addr_string(p.addr);
            peer.active        = (p.liveness == detail::DART_PEER_ACTIVE);
            peer.fragment_size = detail::dart_node_peer_frag(&p);
            out.push_back(std::move(peer));
        }
        return out;
    }

#ifndef DART_NO_PATTERNS
    /* What one peer advertises, folded into entities (a function's req/rsp pair is one
     * entity, a variable's set channel merges as `writable`). A copied snapshot. This
     * walks the peer's whole interest list and allocates per entity: an explicit,
     * observer-grade call, deliberately not part of peers(). A DROPPED (silent,
     * resumable) peer yields an empty vector by default: its cached entities are its
     * dead incarnation's, and after a restart they would stand beside the live
     * incarnation's. include_dropped = true serves that last-known view anyway (a
     * ghost display); gate on Peer::active yourself then. */
    std::vector<Entity> peer_entities(uint32_t peer_id, bool include_dropped = false) const {
        std::vector<Entity> out;
        if (!valid()) return out;
        LockGuard guard(impl_->node);
        detail::DartEntityIter it;
        std::memset(&it, 0, sizeof it);
        it.include_dropped = include_dropped ? 1 : 0;
        detail::DartEntityInfo ei;
        while (detail::dart_node_peer_entity_next(impl_->node, peer_id, &it, &ei))
            out.push_back(entity_from(ei));
        return out;
    }
#endif

#ifndef DART_NO_PATTERNS
    /* The entities THIS node hosts (its functions/variables/signals, then its plain
     * topics), the same folded shape as peer_entities(). A copied snapshot. */
    std::vector<Entity> entities() const {
        std::vector<Entity> out;
        if (!valid()) return out;
        LockGuard guard(impl_->node);
        detail::DartEntityIter it;
        std::memset(&it, 0, sizeof it);
        detail::DartEntityInfo ei;
        while (detail::dart_node_entity_next(impl_->node, &it, &ei))
            out.push_back(entity_from(ei));
        return out;
    }
#endif

    struct MemoryStats { size_t in_use = 0, peak = 0; uint64_t alloc_calls = 0; };
    MemoryStats memory_stats() const {
        MemoryStats s;
        if (valid()) detail::dart_node_mem_stats(impl_->node, &s.in_use, &s.peak, &s.alloc_calls);
        return s;
    }
    struct BackpressureStats { uint64_t waited_us = 0; uint32_t waited_sends = 0; };
    BackpressureStats backpressure_stats() const {
        BackpressureStats b;
        if (valid()) detail::dart_node_backpressure_stats(impl_->node, &b.waited_us, &b.waited_sends);
        return b;
    }
    /* Sends that evicted never-sent history after the bounded wait (the
     * ErrorKind::EvictedUnsent count): the send-burst/overload indicator. */
    uint32_t evicted_unsent() const { return valid() ? detail::dart_node_evicted_unsent(impl_->node) : 0; }

    /* The most recent error this node reported (also delivered via on_event): the
     * formatted one-line message, and its machine-readable ErrorKind. */
    std::string last_error() const {
        detail::DartEvent e = detail::dart_last_error(valid() ? impl_->node : nullptr);
        char b[192]; return detail::dart_event_str(&e, b, sizeof b);
    }
    ErrorKind last_error_kind() const {
        return static_cast<ErrorKind>(detail::dart_last_error(valid() ? impl_->node : nullptr).error);
    }

    /* ---- built-in logs (the @dart/log/{error,warn,info} topics) ---------------------
     * Publish a line on a level's topic. Already-formatted text (format in your own
     * code); truncated at DART_LOG_MAX. SendStatus::NoSys when logs are disabled. */
    SendStatus log(LogLevel level, std::string_view text) {
        if (!valid()) return SendStatus::State;
        return static_cast<SendStatus>(detail::dart_node_log_text(
            impl_->node, static_cast<detail::DartLogLevel>(level),
            text.data(), static_cast<int>(text.size())));
    }
    SendStatus log_error(std::string_view t) { return log(LogLevel::Error, t); }
    SendStatus log_warn (std::string_view t) { return log(LogLevel::Warn,  t); }
    SendStatus log_info (std::string_view t) { return log(LogLevel::Info,  t); }

    /* printf-style overloads. They preserve the C log API's bounded formatting:
     * output is truncated at DART_LOG_MAX, and a formatting failure publishes an empty
     * line. A plain string literal may use either overload and has the same result. */
    template <class... Args>
    SendStatus log(LogLevel level, const char* fmt, Args&&... args) {
        if (!valid()) return SendStatus::State;
        if (!fmt) return SendStatus::NoTopic;
        char text[DART_LOG_MAX];
        int len = std::snprintf(text, sizeof text, fmt, std::forward<Args>(args)...);
        if (len < 0) len = 0;
        if (len >= static_cast<int>(sizeof text)) len = static_cast<int>(sizeof text) - 1;
        return log(level, std::string_view(text, static_cast<size_t>(len)));
    }
    template <class... Args>
    SendStatus log_error(const char* fmt, Args&&... args) {
        return log(LogLevel::Error, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    SendStatus log_warn(const char* fmt, Args&&... args) {
        return log(LogLevel::Warn, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    SendStatus log_info(const char* fmt, Args&&... args) {
        return log(LogLevel::Info, fmt, std::forward<Args>(args)...);
    }

    /* This node's own handle for a level's log topic (invalid Topic when disabled):
     * widen its role and read it like any topic, or use on_log below. */
    Topic log_topic(LogLevel level) {
        if (!valid()) return Topic();
        return Topic(detail::dart_node_log_topic(impl_->node,
                     static_cast<detail::DartLogLevel>(level)), impl_.get());
    }

    /* Subscribe to a level's mesh-wide log stream: widens this node's own log handle to
     * PubSub and delivers every OTHER node's lines at that level (never your own),
     * decoded to a LogLine. Late-join history (keep_last per writer) replays on match.
     * The handler fires on the polling thread like any subscription. Returns false when
     * logs are disabled. Call it once per level from your setup (not from a callback). */
    bool on_log(LogLevel level, std::function<void(const LogLine&)> cb) {
        if (!valid() || !cb) return false;
        detail::DartTopic* ch = detail::dart_node_log_topic(
            impl_->node, static_cast<detail::DartLogLevel>(level));
        if (!ch) return false;
        if (detail::dart_topic_set_role(ch, detail::DART_PUBSUB) != 0) return false;
        uint16_t idx = detail::dart_topic_index(ch);
        MessageHandler h = [level, cb = std::move(cb)](const MessageView& m) {
            LogLine ln;
            ln.level   = level;
            ln.node    = m.publisher_name();
            ln.node_id = m.publisher_id();
            ln.wall_us = m.get_uint("wall_us");
            ln.mono_us = m.get_uint("mono_us");
            ln.recv_us = m.recv_us();
            ln.written_us = m.written_us();
            ln.text    = m.get_string("text");
            cb(ln);
        };
        std::lock_guard<std::mutex> g(impl_->reg_mu);   /* leaf lock: never call C while held */
        auto& slot = impl_->sub_handlers[idx];
        auto nv = slot ? std::make_shared<std::vector<MessageHandler>>(*slot)
                       : std::make_shared<std::vector<MessageHandler>>();
        nv->push_back(std::move(h));
        slot = std::move(nv);
        return true;
    }

#ifndef DART_NO_PATTERNS
    /* ---- @dart/meta introspection --------------------------------------------------
     * The local @dart/meta caller handle. Call it directed at a peer id to fetch that
     * peer's snapshot, e.g. node.meta().call_async(req, cb, {peer_id}). Invalid when
     * meta is disabled. Most callers want meta_request. (Defined out-of-line below:
     * RemoteFunction<> is completed after Node.) */
    RemoteFunction<> meta();
    /* Fetch a peer's snapshot: directs a @dart/meta call at `peer` and decodes the reply
     * into an owning MetaSnapshot. Async, so it works under start() (unlike a blocking
     * call). cb fires once, on the polling thread. sections = OR of MetaSection (0 = all).
     * Returns the send status of the request. */
    SendStatus meta_request(uint32_t peer, std::function<void(const MetaSnapshot&)> cb,
                            uint32_t sections = 0);
#endif

private:
    struct TopicRec { detail::DartTopic* ch; uint8_t bits; uint64_t schema_hash; };

    struct Impl {
        detail::DartNode*        node = nullptr;
        MessageHandler           on_msg;
        EventHandler             on_event;
        std::string              disc_group;
        std::string              mcast_if;
        std::string              self_ip;
        std::vector<detail::DartDiscoveryAddr> seeds;

        /* wrapper-level registries. create_mu serializes wrapper-side creates (held
         * across C create calls; NEVER taken from a callback). reg_mu is a leaf lock
         * (never call into C while holding it; safe from callbacks). */
        std::mutex               create_mu;
        std::mutex               reg_mu;
        std::map<std::string, TopicRec, std::less<>> topics;   /* name -> shared slot */
        std::unordered_map<uint16_t,
            std::shared_ptr<const std::vector<MessageHandler>>> sub_handlers;
        std::vector<std::unique_ptr<priv::HandlerBox>> boxes;  /* pattern handler boxes */
#ifndef DART_NO_PATTERNS
        std::unordered_set<void*> async_live;                  /* outstanding AsyncBox* */
#endif

        ~Impl() {
            if (node) detail::dart_node_close(node, /*send_bye=*/1);
#ifndef DART_NO_PATTERNS
            /* the C never fires pending async callbacks at close, so reap the boxes */
            for (void* b : async_live) delete static_cast<priv::AsyncBox*>(b);
#endif
        }
    };
    std::unique_ptr<Impl> impl_;

    struct LockGuard {
        detail::DartNode* n;
        explicit LockGuard(detail::DartNode* node) : n(node) { detail::dart_node_lock(n); }
        ~LockGuard() { detail::dart_node_unlock(n); }
    };

    static void on_msg_tramp(const detail::DartMsg* m) {
        Impl* impl = static_cast<Impl*>(m->user);
        if (!impl) return;
        std::shared_ptr<const std::vector<MessageHandler>> hs;
        {
            std::lock_guard<std::mutex> g(impl->reg_mu);
            auto it = impl->sub_handlers.find(m->topic_index);
            if (it != impl->sub_handlers.end()) hs = it->second;
        }
        MessageView msg(m);
        if (hs) {
            for (const MessageHandler& h : *hs) {
#if defined(__cpp_exceptions)
                try { h(msg); } catch (...) { report_handler_exception(impl); }
#else
                h(msg);
#endif
            }
            return;
        }
        if (impl->on_msg) {
#if defined(__cpp_exceptions)
            try { impl->on_msg(msg); } catch (...) { report_handler_exception(impl); }
#else
            impl->on_msg(msg);
#endif
        }
    }
    static void on_evt_tramp(const detail::DartEvent* e) {
        Impl* impl = static_cast<Impl*>(e->user);
        if (impl && impl->on_event) {
            Event ev(e);
#if defined(__cpp_exceptions)
            try { impl->on_event(ev); } catch (...) {}   /* never let a throw reach C */
#else
            impl->on_event(ev);
#endif
        }
    }
    /* an app handler threw: surface it as an EventKind::Error (kind ErrorKind::None)
     * rather than swallowing it silently or letting it unwind into the C poll */
    static void report_handler_exception(Impl* impl) {
#if defined(__cpp_exceptions)
        if (!impl->on_event) return;
        detail::DartEvent e;
        std::memset(&e, 0, sizeof e);
        e.kind = detail::DART_ERROR;
        e.error = detail::DART_E_NONE;
        e.user = impl;
        Event ev(&e);
        try { impl->on_event(ev); } catch (...) {}
#else
        (void)impl;
#endif
    }

#ifndef DART_NO_PATTERNS
    /* Decode a node-owned schema (valid under the node lock the reflection call holds)
     * into an owned SchemaInfo, so the returned Entity stays safe to keep. */
    static SchemaInfo schema_info_from(const detail::DartSchema* s) {
        SchemaInfo si;
        if (!s) return si;
        detail::DartString nm = detail::dart_schema_name(s);
        if (nm.data) si.name.assign(nm.data, nm.len);
        si.hash = detail::dart_schema_hash(s);
        si.size = detail::dart_schema_size(s);
        uint16_t n = detail::dart_schema_field_count(s);
        si.fields.reserve(n);
        for (uint16_t i = 0; i < n; i++) {
            detail::DartSchemaFieldInfo f;
            if (!detail::dart_schema_field_at(s, i, &f)) break;
            SchemaField sf;
            if (f.name.data) sf.name.assign(f.name.data, f.name.len);
            sf.kind = static_cast<FieldType>(f.kind);
            sf.elem = static_cast<FieldType>(f.elem);
            sf.count = f.count; sf.depth = f.depth; sf.str_cap = f.str_cap;
            sf.offset = f.offset; sf.size = f.size;
            if (sf.kind == FieldType::Enum) {
                uint16_t vc = detail::dart_schema_enum_count(s, i);
                sf.variants.reserve(vc);
                for (uint16_t k = 0; k < vc; k++) {
                    int64_t val; detail::DartString vn;
                    if (!detail::dart_schema_enum_variant(s, i, k, &val, &vn)) continue;
                    SchemaField::Option o;
                    o.value = val;
                    if (vn.data) o.name.assign(vn.data, vn.len);
                    sf.variants.push_back(std::move(o));
                }
            }
            si.fields.push_back(std::move(sf));
        }
        return si;
    }

    static Entity entity_from(const detail::DartEntityInfo& ei) {
        Entity e;
        e.kind = static_cast<EntityKind>(ei.kind);
        if (ei.name.data) e.name.assign(ei.name.data, ei.name.len);
        else {
            char hx[16];
            std::snprintf(hx, sizeof hx, "0x%08x", (unsigned)ei.hash);
            e.name = hx;
        }
        e.provides   = ei.provides != 0;
        e.consumes   = ei.consumes != 0;
        e.reliable   = ei.reliable != 0;
        e.writable   = ei.writable != 0;
        e.forceable  = ei.forceable != 0;
        e.incomplete = ei.incomplete != 0;
        e.index = ei.index;
        e.hash  = ei.hash;
        e.schema_hash     = ei.schema_hash;
        e.rsp_schema_hash = ei.rsp_schema_hash;
        e.schema     = schema_info_from(ei.schema);
        e.rsp_schema = schema_info_from(ei.rsp_schema);
        return e;
    }
#endif

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
        c.queue_bytes          = q.queue_bytes;
        c.max_rate_hz          = q.max_rate_hz;
        c.no_timestamp         = q.no_timestamp ? 1 : 0;
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

    friend class Topic;
    template <class A, class B> friend class FunctionDefinition;
    template <class A, class B> friend class RemoteFunction;
    template <class A> friend class VariableDefinition;
    template <class A> friend class RemoteVariable;
    template <class A> friend class Signal;
    template <class A> friend class Publisher;
    template <class A> friend class Subscriber;
};

/* Topic constructor: create, or share the same-name slot with a widened role. A LIVE
 * same-name topic with a different schema still refuses (two modules of one process
 * disagreeing about a name is a bug worth surfacing); retire() the old one first to
 * retype a name. */
inline Topic::Topic(Node& node, std::string_view name, Role role,
                    const Schema* schema, const Qos& qos) {
    if (!node.valid()) { priv::raise_msg("dart::Topic: node is not valid"); return; }
    Node::Impl* impl = node.impl_.get();
    std::lock_guard<std::mutex> g(impl->create_mu);
    std::string nm(name);
    uint64_t sh = schema ? schema->hash() : 0;
    impl_ = impl;
    auto it = impl->topics.find(nm);
    if (it != impl->topics.end()) {
        if (sh && it->second.schema_hash && sh != it->second.schema_hash) {
            priv::raise_msg("dart::Topic: same-name topic already exists with a different schema"
                            " (retire() it to retype the name)");
            return;
        }
        uint8_t bits = (uint8_t)(it->second.bits | priv::role_bits(role));
        if (bits != it->second.bits) {
            detail::dart_topic_set_role(it->second.ch, static_cast<detail::DartRole>(priv::role_from_bits(bits)));
            it->second.bits = bits;
        }
        ch_ = it->second.ch;
        return;
    }
    detail::DartTopicOpts co;
    std::memset(&co, 0, sizeof co);
    co.qos = Node::to_c(qos);
    ch_ = detail::dart_node_create_topic(impl->node, nm.c_str(),
              static_cast<detail::DartRole>(role), schema ? schema->raw() : nullptr, &co);
    if (!ch_) { priv::raise_last(impl->node, "dart::Topic create"); return; }
    impl->topics.emplace(std::move(nm), Node::TopicRec{ ch_, priv::role_bits(role), sh });
}

/* Topic retire (see the declaration): on success the C handle is freed, the name-cache
 * entry and this topic's wrapper subscriptions are forgotten, and this handle (plus any
 * same-name handle sharing the slot) goes invalid. */
inline SendStatus Topic::retire() {
    if (!ch_) return SendStatus::NoTopic;
    Node::Impl* impl = static_cast<Node::Impl*>(impl_);
    if (!impl) {   /* no registry back-pointer (default-constructed edge): C-level only */
        int bare = detail::dart_topic_retire(ch_);
        if (bare == 0) ch_ = nullptr;
        return static_cast<SendStatus>(bare);
    }
    std::lock_guard<std::mutex> g(impl->create_mu);
    uint16_t idx = detail::dart_topic_index(ch_);
    int rc = detail::dart_topic_retire(ch_);
    if (rc != 0) return static_cast<SendStatus>(rc);
    for (auto it = impl->topics.begin(); it != impl->topics.end(); ++it)
        if (it->second.ch == ch_) { impl->topics.erase(it); break; }
    {   /* drop this index's message handlers: the slot may be reused by a different
           topic, and stale handlers must never fire for the successor */
        std::lock_guard<std::mutex> g2(impl->reg_mu);
        impl->sub_handlers.erase(idx);
    }
    ch_ = nullptr;
    return SendStatus::Ok;
}

inline Topic Node::create_topic(std::string_view name, Role role,
                                const Schema* schema, const Qos& qos) {
    return Topic(*this, name, role, schema, qos);
}

#ifndef DART_NO_PATTERNS

/* ====================== FUNCTIONS (untyped cores) =========================== */

/* FunctionDefinition<> (untyped): the implementation side of a request/response
 * function. Exactly one reply per call; ONE definition per name on the network
 * (a rival fires ErrorKind::DuplicateAuthority on both). Handles are thin and
 * non-owning; the function lives in the Node until close. */
template <> class FunctionDefinition<void, void> {
public:
    using Handler = std::function<void(Request<>&)>;

    FunctionDefinition() = default;
    /* handler == {} answers every call CallStatus::NoHandler (a declared stub). */
    FunctionDefinition(Node& n, std::string_view name,
                       const Schema* req_schema, const Schema* rsp_schema,
                       Handler handler, const FunctionOptions& o = {}) {
        if (!n.valid()) { priv::raise_msg("dart::FunctionDefinition: node is not valid"); return; }
        std::string nm(name);
        detail::DartFunctionOpts co;
        std::memset(&co, 0, sizeof co);
        co.backpressure_wait_us = o.backpressure_wait_us;
        co.timeout_us           = o.timeout_us;
        Box* box = nullptr;
        if (handler) { box = new Box(); box->h = std::move(handler); }
        fn_ = detail::dart_node_create_function_definition(n.impl_->node, nm.c_str(),
                  req_schema ? req_schema->raw() : nullptr,
                  rsp_schema ? rsp_schema->raw() : nullptr,
                  box ? &FunctionDefinition::tramp : nullptr, box, &co);
        if (!fn_) {
            delete box;
            priv::raise_last(n.impl_->node, "dart::FunctionDefinition create");
            return;
        }
        if (box) {
            box->fn.store(fn_);
            std::lock_guard<std::mutex> g(n.impl_->reg_mu);
            n.impl_->boxes.emplace_back(box);
        }
    }

    bool valid() const noexcept { return fn_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }
    /* callers currently matched to this definition */
    int caller_count() const { return fn_ ? detail::dart_function_match_count(fn_) : 0; }
    /* Retire the definition: park its channels and release the name so a successor can
     * bind (see dart_function_retire). The handle is empty after; refused (State) from
     * inside a callback, and the handle then stays valid. */
    SendStatus retire() {
        if (!fn_) return SendStatus::NoTopic;
        int rc = detail::dart_function_retire(fn_);
        if (rc == 0) fn_ = nullptr;
        return static_cast<SendStatus>(rc);
    }

private:
    struct Box : priv::HandlerBox {
        Handler h;
        std::atomic<detail::DartFunction*> fn{ nullptr };
    };
    static void tramp(detail::DartRequest* rq, void* user) {
        Box* b = static_cast<Box*>(user);
        Request<> r(rq, b->fn.load());
#if defined(__cpp_exceptions)
        try { b->h(r); }
        catch (...) { detail::dart_request_fail(rq, "handler threw", detail::dart_bytes(nullptr, 0)); }
#else
        b->h(r);
#endif
    }
    detail::DartFunction* fn_ = nullptr;
    template <class A, class B> friend class FunctionDefinition;
};

/* RemoteFunction<> (untyped): a reference to a definition on another node. */
template <> class RemoteFunction<void, void> {
public:
    RemoteFunction() = default;
    RemoteFunction(Node& n, std::string_view name,
                   const Schema* req_schema = nullptr, const Schema* rsp_schema = nullptr,
                   const FunctionOptions& o = {}) {
        if (!n.valid()) { priv::raise_msg("dart::RemoteFunction: node is not valid"); return; }
        std::string nm(name);
        detail::DartFunctionOpts co;
        std::memset(&co, 0, sizeof co);
        co.backpressure_wait_us = o.backpressure_wait_us;
        co.timeout_us           = o.timeout_us;
        fn_ = detail::dart_node_create_remote_function(n.impl_->node, nm.c_str(),
                  req_schema ? req_schema->raw() : nullptr,
                  rsp_schema ? rsp_schema->raw() : nullptr, &co);
        if (!fn_) { priv::raise_last(n.impl_->node, "dart::RemoteFunction create"); return; }
        impl_ = n.impl_.get();
    }

    bool valid() const noexcept { return fn_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /* BLOCKING call: drives the node loop until the response arrives or timeout_ms
     * elapses (negative = the function's default timeout). Refused (send_status()
     * == SendStatus::State) from inside a callback or while a service thread owns
     * this node's loop; use call_async there. */
    Response<> call(Bytes req, int timeout_ms = -1, const CallOptions& opts = {}) {
        Response<> r;
        if (!fn_) { r.ss_ = SendStatus::NoTopic; return r; }
        detail::DartResponse out;
        std::memset(&out, 0, sizeof out);
        detail::DartCallOpts co; std::memset(&co, 0, sizeof co); co.provider = opts.provider;
        int rc = detail::dart_function_call(fn_, priv::to_c(req), &out, timeout_ms, &co);
        if (rc == 1) {
            r.st_       = static_cast<CallStatus>(out.status);
            r.provider_ = out.provider;
            r.written_  = out.written_us;
            r.schema_   = out.schema;
            if (out.data.len) r.data_.assign(out.data.data, out.data.data + out.data.len);
        } else if (rc < 0) {
            r.ss_ = static_cast<SendStatus>(rc);   /* status() stays Timeout: not answered */
        }
        if (rc >= 0 && out.message.len)   /* answered or timed out: copy the outcome text */
            r.message_.assign(out.message.data, out.message.len);
        return r;
    }
    /* Async form: returns as soon as the request is committed; on_response fires once
     * with the outcome (on the polling thread). */
    SendStatus call_async(Bytes req, std::function<void(const ResponseView<>&)> on_response,
                          const CallOptions& opts = {}) {
        if (!fn_) return SendStatus::NoTopic;
        priv::AsyncBox* box = new priv::AsyncBox{ std::move(on_response),
                                                  &impl_->reg_mu, &impl_->async_live };
        {
            std::lock_guard<std::mutex> g(impl_->reg_mu);
            impl_->async_live.insert(box);
        }
        detail::DartCallOpts co; std::memset(&co, 0, sizeof co); co.provider = opts.provider;
        int rc = detail::dart_function_call_async(fn_, priv::to_c(req),
                                                  &RemoteFunction::async_tramp, box, &co);
        if (rc != 0) {
            std::lock_guard<std::mutex> g(impl_->reg_mu);
            impl_->async_live.erase(box);
            delete box;
        }
        return static_cast<SendStatus>(rc);
    }

    int  match_count()    const { return fn_ ? detail::dart_function_match_count(fn_) : 0; }
    bool has_definition() const { return match_count() > 0; }
    /* Retire the remote: park its channels and release the name so a successor can bind
     * (see dart_function_retire); every outstanding call completes with
     * CallStatus::Cancelled. The handle is empty after; refused (State) from inside a
     * callback, and the handle then stays valid. */
    SendStatus retire() {
        if (!fn_) return SendStatus::NoTopic;
        int rc = detail::dart_function_retire(fn_);
        if (rc == 0) fn_ = nullptr;
        return static_cast<SendStatus>(rc);
    }

private:
    /* wrap a node-owned function handle (the @dart/meta endpoint): callable, never
     * destroyed by us (functions are never torn down before the node). */
    RemoteFunction(detail::DartFunction* fn, Node::Impl* impl) : fn_(fn), impl_(impl) {}
    static void async_tramp(const detail::DartResponse* r) {
        priv::AsyncBox* box = static_cast<priv::AsyncBox*>(r->user);
        {
            std::lock_guard<std::mutex> g(*box->mu);
            box->live->erase(box);
        }
        if (box->cb) {
            ResponseView<> rv(r);
#if defined(__cpp_exceptions)
            try { box->cb(rv); } catch (...) {}   /* never unwind into the C poll */
#else
            box->cb(rv);
#endif
        }
        delete box;
    }
    detail::DartFunction* fn_ = nullptr;
    Node::Impl*           impl_ = nullptr;
    template <class A, class B> friend class RemoteFunction;
    friend class Node;
};

/* Node::meta / meta_request: out-of-line so RemoteFunction<> is a complete type here. */
inline RemoteFunction<> Node::meta() {
    if (!valid()) return RemoteFunction<>();
    return RemoteFunction<>(detail::dart_node_meta_function(impl_->node), impl_.get());
}
inline SendStatus Node::meta_request(uint32_t peer,
                                     std::function<void(const MetaSnapshot&)> cb, uint32_t sections) {
    RemoteFunction<> m = meta();
    if (!m.valid()) return SendStatus::NoTopic;
    uint8_t buf[4]; Bytes req;   /* the mask lives on the stack: call_async commits it now */
    if (sections) {
        buf[0] = (uint8_t)(sections);       buf[1] = (uint8_t)(sections >> 8);
        buf[2] = (uint8_t)(sections >> 16); buf[3] = (uint8_t)(sections >> 24);
        req = Bytes(buf, 4);
    }
    return m.call_async(req, [cb = std::move(cb)](const ResponseView<>& r) {
        cb(MetaSnapshot::decode(r));
    }, CallOptions{ peer });
}

/* ====================== VARIABLES (untyped cores) =========================== */

/* VariableUpdate: the state just applied to a variable, as handed to on_change /
 * on_write handlers. Views are valid for the callback only. */
class VariableUpdate {
public:
    Bytes            value()     const { return { u_->value.data, u_->value.len }; }
    std::string_view name()      const { return { u_->name.data, u_->name.len }; }
    bool             forced()    const { return u_->forced != 0; }
    uint32_t         write_seq() const { return u_->write_seq; }
    /* peer id the write arrived from (0 = a local call on this node) */
    uint32_t         source()    const { return u_->source; }
    /* node monotonic us when the write applied */
    uint64_t         recv_us()   const { return u_->recv_us; }
    /* the writer's wall clock for this write (this node's own for a local write) */
    uint64_t         written_us()   const { return u_->written_us; }
    const detail::DartSchema* raw_schema() const { return u_->schema; }

private:
    friend class VariableDefinition<void>;
    explicit VariableUpdate(const detail::DartVariableUpdate* u) : u_(u) {}
    const detail::DartVariableUpdate* u_;
};

/* VariableDefinition<> (untyped): this node holds the authoritative value. */
template <> class VariableDefinition<void> {
public:
    VariableDefinition() = default;
    VariableDefinition(Node& n, std::string_view name, const Schema* schema,
                       const VariableOptions<>& o = {}) {
        if (!n.valid()) { priv::raise_msg("dart::VariableDefinition: node is not valid"); return; }
        create(n, name, schema, o, /*definition=*/true);
    }

    bool valid() const noexcept { return var_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /* the current value, copied out under the node lock (nullopt = none yet) */
    std::optional<std::vector<uint8_t>> get() const {
        if (!var_) return std::nullopt;
        detail::dart_node_lock(node_);
        detail::DartBytes b;
        std::optional<std::vector<uint8_t>> out;
        if (detail::dart_variable_get(var_, &b) == 1) {
            out.emplace();
            if (b.len) out->assign(b.data, b.data + b.len);
        }
        detail::dart_node_unlock(node_);
        return out;
    }
    SendStatus set(Bytes value) {
        if (!var_) return SendStatus::NoTopic;
        return static_cast<SendStatus>(detail::dart_variable_set(var_, priv::to_c(value)));
    }
    /* Force the value: writes are absorbed into the shadow source until unforce, which
     * restores the latest absorbed set. Requires VariableOptions::allow_force. */
    SendStatus force(Bytes value) {
        if (!var_) return SendStatus::NoTopic;
        return static_cast<SendStatus>(detail::dart_variable_force(var_, priv::to_c(value)));
    }
    SendStatus unforce() {
        if (!var_) return SendStatus::NoTopic;
        return static_cast<SendStatus>(detail::dart_variable_unforce(var_));
    }
    bool forced() const { return var_ && detail::dart_variable_forced(var_) == 1; }
    /* remotes currently matched to this definition */
    int remote_count() const { return var_ ? detail::dart_variable_match_count(var_) : 0; }

    /* Observe this variable. on_change fires only when the observed state actually
     * changes (the first value, different bytes, or a forced flip), and replays the
     * current value once at registration so it can never be missed; on_write fires on
     * every applied write, byte-identical or not. Handlers run inline on the thread
     * that applied the write (the poll / service thread for anything off the wire),
     * like a subscriber handler. One handler each; pass {} to clear. */
    void on_change(std::function<void(const VariableUpdate&)> h) { observe(std::move(h), true); }
    void on_write (std::function<void(const VariableUpdate&)> h) { observe(std::move(h), false); }

    /* Retire the handle: park its channels and release the name so a successor can bind
     * (see dart_variable_retire; without it a re-created same-name handle is silently
     * shadowed by the live twin). The handle is empty after; refused (State) from inside
     * a callback, and the handle then stays valid. */
    SendStatus retire() {
        if (!var_) return SendStatus::NoTopic;
        int rc = detail::dart_variable_retire(var_);
        if (rc == 0) var_ = nullptr;
        return static_cast<SendStatus>(rc);
    }

protected:
    struct UBox : priv::HandlerBox {
        std::function<void(const VariableUpdate&)> h;
        Node::Impl* impl = nullptr;
    };
    static void utramp(const detail::DartVariableUpdate* u, void* user) {
        UBox* b = static_cast<UBox*>(user);
        VariableUpdate up(u);
#if defined(__cpp_exceptions)
        try { b->h(up); } catch (...) { Node::report_handler_exception(b->impl); }
#else
        b->h(up);
#endif
    }
    void observe(std::function<void(const VariableUpdate&)> h, bool change) {
        if (!var_) return;
        UBox* box = nullptr;
        if (h) { box = new UBox(); box->h = std::move(h); box->impl = impl_; }
        (void)(change ? detail::dart_variable_on_change(var_, box ? &VariableDefinition::utramp : nullptr, box)
                      : detail::dart_variable_on_write (var_, box ? &VariableDefinition::utramp : nullptr, box));
        if (box) {   /* kept alive until node close, like every handler box */
            std::lock_guard<std::mutex> g(impl_->reg_mu);
            impl_->boxes.emplace_back(box);
        }
    }
    void create(Node& n, std::string_view name, const Schema* schema,
                const VariableOptions<>& o, bool definition) {
        std::string nm(name);
        detail::DartVariableOpts co;
        std::memset(&co, 0, sizeof co);
        co.initial     = priv::to_c(o.initial);
        co.access      = o.read_only ? detail::DART_VAR_READONLY : detail::DART_VAR_READWRITE;
        co.allow_force = o.allow_force ? 1 : 0;
        co.catch_up    = o.catch_up;
        co.backpressure_wait_us = o.backpressure_wait_us;
        node_ = n.impl_->node;
        impl_ = n.impl_.get();
        var_ = definition
            ? detail::dart_node_create_variable_definition(node_, nm.c_str(),
                  schema ? schema->raw() : nullptr, &co)
            : detail::dart_node_create_remote_variable(node_, nm.c_str(),
                  schema ? schema->raw() : nullptr, &co);
        if (!var_) priv::raise_last(node_, definition ? "dart::VariableDefinition create"
                                                      : "dart::RemoteVariable create");
    }
    detail::DartVariable* var_ = nullptr;
    detail::DartNode*     node_ = nullptr;
    Node::Impl*           impl_ = nullptr;
    template <class A> friend class VariableDefinition;
};

/* RemoteVariable<> (untyped): the value lives on another node; reads see the cached
 * latest, writes go over the set channel (dumb writes, no response). */
template <> class RemoteVariable<void> : public VariableDefinition<void> {
public:
    RemoteVariable() = default;
    RemoteVariable(Node& n, std::string_view name, const Schema* schema,
                   const VariableOptions<>& o = {}) {
        if (!n.valid()) { priv::raise_msg("dart::RemoteVariable: node is not valid"); return; }
        create(n, name, schema, o, /*definition=*/false);
    }
    /* Block (driving the node loop) until a value exists or timeout_ms elapses. */
    bool wait(int timeout_ms) { return var_ && detail::dart_variable_wait(var_, timeout_ms) == 1; }
    bool has_definition() const { return remote_count() > 0; }
    int  match_count()    const { return remote_count(); }
};

/* ====================== SIGNALS (untyped core) ============================== */

/* Signal<> (untyped): a reliable fire-and-forget event, N emitters / N listeners,
 * never latched. Passing a handler IS the subscription; every handle may emit. */
template <> class Signal<void> {
public:
    Signal() = default;
    /* emit-only handle (no subscription) */
    Signal(Node& n, std::string_view name, const Schema* schema = nullptr,
           const SignalOptions& o = {}) {
        init(n, name, schema, nullptr, o);
    }
    /* listening handle: on_signal fires for each signal from ANOTHER node */
    Signal(Node& n, std::string_view name, const Schema* schema,
           std::function<void(const MessageView&)> on_signal, const SignalOptions& o = {}) {
        init(n, name, schema, std::move(on_signal), o);
    }

    bool valid() const noexcept { return sig_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /* emit to every matched listener (payload may be empty) */
    SendStatus emit(Bytes payload = Bytes()) {
        if (!sig_) return SendStatus::NoTopic;
        return static_cast<SendStatus>(detail::dart_signal_emit(sig_, priv::to_c(payload)));
    }
    int listener_count() const { return sig_ ? detail::dart_signal_listener_count(sig_) : 0; }
    /* Retire the handle: park its channel and release the name so a successor can bind
     * (see dart_signal_retire). The handle is empty after; refused (State) from inside a
     * callback, and the handle then stays valid. */
    SendStatus retire() {
        if (!sig_) return SendStatus::NoTopic;
        int rc = detail::dart_signal_retire(sig_);
        if (rc == 0) sig_ = nullptr;
        return static_cast<SendStatus>(rc);
    }

private:
    struct Box : priv::HandlerBox {
        std::function<void(const MessageView&)> h;
        Node::Impl* impl = nullptr;
    };
    static void tramp(const detail::DartMsg* m, void* user) {
        Box* b = static_cast<Box*>(user);
        MessageView msg(m);
#if defined(__cpp_exceptions)
        try { b->h(msg); } catch (...) { Node::report_handler_exception(b->impl); }
#else
        b->h(msg);
#endif
    }
    void init(Node& n, std::string_view name, const Schema* schema,
              std::function<void(const MessageView&)> h, const SignalOptions& o) {
        if (!n.valid()) { priv::raise_msg("dart::Signal: node is not valid"); return; }
        std::string nm(name);
        detail::DartSignalOpts co;
        std::memset(&co, 0, sizeof co);
        co.backpressure_wait_us = o.backpressure_wait_us;
        Box* box = nullptr;
        if (h) { box = new Box(); box->h = std::move(h); box->impl = n.impl_.get(); }
        sig_ = detail::dart_node_create_signal(n.impl_->node, nm.c_str(),
                   schema ? schema->raw() : nullptr,
                   box ? &Signal::tramp : nullptr, box, &co);
        if (!sig_) {
            delete box;
            priv::raise_last(n.impl_->node, "dart::Signal create");
            return;
        }
        if (box) {
            std::lock_guard<std::mutex> g(n.impl_->reg_mu);
            n.impl_->boxes.emplace_back(box);
        }
    }
    detail::DartSignal* sig_ = nullptr;
    template <class A> friend class Signal;
};

#endif /* !DART_NO_PATTERNS */

/* ====================== PUB/SUB (untyped cores) ============================= */

/* Publisher<> (untyped): the publish-side handle over a (possibly shared) topic. */
template <> class Publisher<void> {
public:
    Publisher() = default;
    Publisher(Node& n, std::string_view name, const Schema* schema = nullptr,
              const Qos& qos = {})
        : t_(n, name, Role::PubOnly, schema, qos) {}

    bool valid() const noexcept { return t_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    SendStatus send(Bytes data)   { return t_.send(data); }
    int  match_count()      const { return t_.match_count(); }
    int  pending_count()    const { return t_.pending_count(); }
    bool ready()            const { return t_.ready(); }
    SendStatus retire()           { return t_.retire(); }   /* see Topic::retire */
    Topic& topic()                { return t_; }

private:
    Topic t_;
};

/* Subscriber<> (untyped): the subscribe-side handle. A handler fires per message on
 * the polling thread; or consume via take()/dispatch() on a thread of your choosing. */
template <> class Subscriber<void> {
public:
    Subscriber() = default;
    Subscriber(Node& n, std::string_view name, const Schema* schema = nullptr,
               const Qos& qos = {})
        : t_(n, name, Role::SubOnly, schema, qos) {}
    Subscriber(Node& n, std::string_view name, const Schema* schema,
               Node::MessageHandler on_message, const Qos& qos = {})
        : t_(n, name, Role::SubOnly, schema, qos) {
        if (t_.valid() && on_message) add_handler(n, t_, std::move(on_message));
    }

    bool valid() const noexcept { return t_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    std::optional<Message<>> take(int timeout_ms = 0) { return t_.take(timeout_ms); }
    int dispatch(int max_msgs = 0, int timeout_ms = 0) { return t_.dispatch(max_msgs, timeout_ms); }
    SendStatus retire() { return t_.retire(); }   /* see Topic::retire */
    Topic& topic() { return t_; }

private:
    static void add_handler(Node& n, Topic& t, Node::MessageHandler h) {
        Node::Impl* impl = n.impl_.get();
        std::lock_guard<std::mutex> g(impl->reg_mu);
        auto& slot = impl->sub_handlers[t.index()];
        auto nv = slot ? std::make_shared<std::vector<Node::MessageHandler>>(*slot)
                       : std::make_shared<std::vector<Node::MessageHandler>>();
        nv->push_back(std::move(h));
        slot = std::move(nv);
    }
    Topic t_;
    template <class A> friend class Subscriber;
};

/* =========================================================================== *
 *  Typed sugar: thin template layers over the untyped cores, using the
 *  DART_SCHEMA codec for the payloads. All handles stay thin and non-owning.
 * =========================================================================== */

/* Message<T>: an owning taken message (decoded value + copied envelope). */
template <class T> class Message {
public:
    Message() = default;
    const T& value()      const { return v_; }
    const T& operator*()  const { return v_; }
    const T* operator->() const { return &v_; }
    std::string_view publisher_name() const { return pub_; }
    uint32_t         publisher_id()   const { return pid_; }
    std::string_view topic_name()     const { return topic_; }
    uint16_t         topic_index()    const { return idx_; }
    uint64_t         recv_us()        const { return recv_; }
    uint64_t         written_us()        const { return written_; }
private:
    T           v_{};
    std::string pub_, topic_;
    uint32_t    pid_ = 0;
    uint16_t    idx_ = 0;
    uint64_t    recv_ = 0, written_ = 0;
    template <class U> friend class Subscriber;
};

#ifndef DART_NO_PATTERNS

/* Request<Rsp>: the typed view of a request inside a full-form function handler.
 * Wraps the untyped Request<> (reference; callback lifetime) and adds the typed reply. */
template <class Rsp> class Request {
public:
    explicit Request(Request<>& core) : core_(core) {}
    Bytes            data()          const { return core_.data(); }
    uint32_t         caller()        const { return core_.caller(); }
    std::string_view caller_name()   const { return core_.caller_name(); }
    std::string_view function_name() const { return core_.function_name(); }
    uint64_t         recv_us()       const { return core_.recv_us(); }
    uint64_t         written_us()       const { return core_.written_us(); }

    void reply(const Rsp& v) {
        std::vector<uint8_t> s;
        core_.reply(priv::encode(v, s));
    }
    void reply(Bytes raw)     { core_.reply(raw); }
    void fail(std::string_view message = {}, Bytes raw = {}) { core_.fail(message, raw); }
    Deferred<Rsp> defer()     { return Deferred<Rsp>(core_.defer()); }

private:
    Request<>& core_;
};

/* Deferred<Rsp>: the typed parked reply. */
template <class Rsp> class Deferred {
public:
    Deferred() = default;
    Deferred(Deferred<>&& core) : core_(std::move(core)) {}
    Deferred(Deferred&&) noexcept = default;
    Deferred& operator=(Deferred&&) noexcept = default;
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }
    bool complete(const Rsp& v, std::string_view message = {}) {
        std::vector<uint8_t> s;
        return core_.complete(priv::encode(v, s), message);
    }
    bool fail(std::string_view message = {}) { return core_.fail(message); }
private:
    Deferred<> core_;
};

/* Response<Rsp>: the owning typed call outcome. value() is meaningful when ok(). */
template <class Rsp> class Response {
public:
    Response() = default;
    CallStatus status()      const { return st_; }
    bool       ok()          const { return st_ == CallStatus::Ok; }
    uint32_t   provider()    const { return provider_; }
    SendStatus send_status() const { return ss_; }
    uint64_t   written_us()     const { return written_; }
    /* human-readable outcome text (owned): see Response<>::message */
    std::string_view message() const { return message_; }
    const Rsp& value()       const { return v_; }
    const Rsp& operator*()   const { return v_; }
    const Rsp* operator->()  const { return &v_; }
private:
    CallStatus  st_ = CallStatus::Timeout;
    SendStatus  ss_ = SendStatus::Ok;
    uint32_t    provider_ = 0;
    uint64_t    written_ = 0;
    std::string message_;
    Rsp         v_{};
    template <class A, class B> friend class RemoteFunction;
};

/* ResponseView<Rsp>: the typed async outcome; valid for the callback. */
template <class Rsp> class ResponseView {
public:
    CallStatus status()     const { return st_; }
    bool       ok()         const { return st_ == CallStatus::Ok; }
    uint32_t   provider()   const { return provider_; }
    uint64_t   written_us()    const { return written_; }
    /* human-readable outcome text (a view, callback lifetime): see ResponseView<>::message */
    std::string_view message() const { return message_; }
    const Rsp& value()      const { return v_; }
    const Rsp& operator*()  const { return v_; }
    const Rsp* operator->() const { return &v_; }
private:
    ResponseView() = default;
    CallStatus       st_ = CallStatus::Timeout;
    uint32_t         provider_ = 0;
    uint64_t         written_ = 0;
    std::string_view message_;
    Rsp              v_{};
    template <class A, class B> friend class RemoteFunction;
};

/* FunctionDefinition<Req,Rsp>: the typed implementation side. Two handler forms:
 *   Rsp(const Req&)                       simple: the return value is the reply
 *   void(const Req&, Request<Rsp>&)       full: reply/fail/defer explicitly
 * A handler that throws answers CallStatus::AppError (never unwinds into the C). */
template <class Req, class Rsp> class FunctionDefinition {
public:
    FunctionDefinition() = default;
    template <class H>
    FunctionDefinition(Node& n, std::string_view name, H&& handler,
                       const FunctionOptions& o = {}) {
        const Schema* rq = priv::schema_of<Req>();
        const Schema* rs = priv::schema_of<Rsp>();
        if (!rq || !rs) { priv::raise_msg("dart::FunctionDefinition: DART_SCHEMA compile failed"); return; }
        core_ = FunctionDefinition<>(n, name, rq, rs, adapt(std::forward<H>(handler)), o);
    }
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }
    int caller_count() const { return core_.caller_count(); }
    SendStatus retire() { return core_.retire(); }

private:
    template <class H>
    static typename FunctionDefinition<>::Handler adapt(H&& h) {
        if constexpr (std::is_invocable_v<std::decay_t<H>&, const Req&, Request<Rsp>&>) {
            return [f = std::forward<H>(h)](Request<>& u) mutable {
                Req q{};
                if (!priv::decode(q, u.data(), u.raw_schema())) { u.fail("request decode failed"); return; }
                Request<Rsp> tr(u);
                f(q, tr);
            };
        } else if constexpr (std::is_invocable_v<std::decay_t<H>&, const Req&>) {
            static_assert(std::is_convertible_v<
                              std::invoke_result_t<std::decay_t<H>&, const Req&>, Rsp>,
                          "simple function handler must return Rsp");
            return [f = std::forward<H>(h)](Request<>& u) mutable {
                Req q{};
                if (!priv::decode(q, u.data(), u.raw_schema())) { u.fail("request decode failed"); return; }
                Rsp r = f(q);
                std::vector<uint8_t> s;
                u.reply(priv::encode(r, s));
            };
        } else {
            static_assert(priv::always_false<H>,
                "function handler must be Rsp(const Req&) or void(const Req&, dart::Request<Rsp>&)");
            return {};
        }
    }
    FunctionDefinition<> core_;
};

/* RemoteFunction<Req,Rsp>: the typed caller side. */
template <class Req, class Rsp> class RemoteFunction {
public:
    RemoteFunction() = default;
    RemoteFunction(Node& n, std::string_view name, const FunctionOptions& o = {}) {
        const Schema* rq = priv::schema_of<Req>();
        const Schema* rs = priv::schema_of<Rsp>();
        if (!rq || !rs) { priv::raise_msg("dart::RemoteFunction: DART_SCHEMA compile failed"); return; }
        core_ = RemoteFunction<>(n, name, rq, rs, o);
    }
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /* BLOCKING call (see RemoteFunction<>::call); decodes into the owning Response. */
    Response<Rsp> call(const Req& req, int timeout_ms = -1, const CallOptions& opts = {}) {
        std::vector<uint8_t> s;
        Response<> ur = core_.call(priv::encode(req, s), timeout_ms, opts);
        Response<Rsp> r;
        r.st_ = ur.status(); r.ss_ = ur.send_status(); r.provider_ = ur.provider();
        r.written_ = ur.written_us();
        r.message_.assign(ur.message());   /* own it: ur dies with this frame */
        if (ur.ok()) (void)priv::decode(r.v_, ur.data(), ur.raw_schema());
        return r;
    }
    SendStatus call_async(const Req& req, std::function<void(const ResponseView<Rsp>&)> cb,
                          const CallOptions& opts = {}) {
        std::vector<uint8_t> s;
        return core_.call_async(priv::encode(req, s),
            [cb = std::move(cb)](const ResponseView<>& uv) {
                ResponseView<Rsp> tv;
                tv.st_ = uv.status(); tv.provider_ = uv.provider(); tv.written_ = uv.written_us();
                tv.message_ = uv.message();
                if (uv.ok()) (void)priv::decode(tv.v_, uv.data(), uv.raw_schema());
                cb(tv);
            }, opts);
    }

    int  match_count()    const { return core_.match_count(); }
    bool has_definition() const { return core_.has_definition(); }
    SendStatus retire() { return core_.retire(); }

private:
    RemoteFunction<> core_;
};

/* VariableDefinition<T>: the typed authoritative value. */
template <class T> class VariableDefinition {
public:
    VariableDefinition() = default;
    VariableDefinition(Node& n, std::string_view name, const VariableOptions<T>& o = {}) {
        const Schema* sc = priv::schema_of<T>();
        if (!sc) { priv::raise_msg("dart::VariableDefinition: DART_SCHEMA compile failed"); return; }
        std::vector<uint8_t> scratch;
        VariableOptions<> uo;
        if (o.initial) uo.initial = priv::encode(*o.initial, scratch);
        uo.read_only = o.read_only; uo.allow_force = o.allow_force;
        uo.catch_up = o.catch_up; uo.backpressure_wait_us = o.backpressure_wait_us;
        core_ = VariableDefinition<>(n, name, sc, uo);
    }
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /* the current value BY VALUE, decoded under the node lock (nullopt = none yet) */
    std::optional<T> get() const {
        auto b = core_.get();
        if (!b) return std::nullopt;
        T v{};
        if (!priv::decode(v, Bytes(b->data(), b->size()), nullptr)) return std::nullopt;
        return v;
    }
    SendStatus set(const T& v)   { std::vector<uint8_t> s; return core_.set(priv::encode(v, s)); }
    SendStatus force(const T& v) { std::vector<uint8_t> s; return core_.force(priv::encode(v, s)); }
    SendStatus unforce()         { return core_.unforce(); }
    bool forced()          const { return core_.forced(); }
    int  remote_count()    const { return core_.remote_count(); }

    /* Observe (see the untyped core for the change/write contract). Handler forms:
     * void(const T&) or void(const T&, const VariableUpdate&); nullptr clears. */
    template <class H, class = std::enable_if_t<
        std::is_invocable_v<std::decay_t<H>&, const T&> ||
        std::is_invocable_v<std::decay_t<H>&, const T&, const VariableUpdate&>>>
    void on_change(H&& h) { core_.on_change(adapt(std::forward<H>(h))); }
    template <class H, class = std::enable_if_t<
        std::is_invocable_v<std::decay_t<H>&, const T&> ||
        std::is_invocable_v<std::decay_t<H>&, const T&, const VariableUpdate&>>>
    void on_write(H&& h)  { core_.on_write(adapt(std::forward<H>(h))); }
    void on_change(std::nullptr_t) { core_.on_change({}); }
    void on_write (std::nullptr_t) { core_.on_write({}); }
    SendStatus retire() { return core_.retire(); }

private:
    template <class H>
    static std::function<void(const VariableUpdate&)> adapt(H&& h) {
        return [f = std::forward<H>(h)](const VariableUpdate& u) mutable {
            T v{};
            if (!priv::decode(v, u.value(), u.raw_schema())) return;
            if constexpr (std::is_invocable_v<std::decay_t<H>&, const T&, const VariableUpdate&>)
                f(v, u);
            else
                f(v);
        };
    }
    VariableDefinition<> core_;
};

/* RemoteVariable<T>: the typed accessor of a value owned elsewhere. */
template <class T> class RemoteVariable {
public:
    RemoteVariable() = default;
    RemoteVariable(Node& n, std::string_view name, const VariableOptions<T>& o = {}) {
        const Schema* sc = priv::schema_of<T>();
        if (!sc) { priv::raise_msg("dart::RemoteVariable: DART_SCHEMA compile failed"); return; }
        VariableOptions<> uo;
        uo.catch_up = o.catch_up; uo.backpressure_wait_us = o.backpressure_wait_us;
        core_ = RemoteVariable<>(n, name, sc, uo);
    }
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    std::optional<T> get() const {
        auto b = core_.get();
        if (!b) return std::nullopt;
        T v{};
        if (!priv::decode(v, Bytes(b->data(), b->size()), nullptr)) return std::nullopt;
        return v;
    }
    /* Block (driving the node loop) until a value exists or timeout_ms elapses. */
    bool wait(int timeout_ms)    { return core_.wait(timeout_ms); }
    SendStatus set(const T& v)   { std::vector<uint8_t> s; return core_.set(priv::encode(v, s)); }
    SendStatus force(const T& v) { std::vector<uint8_t> s; return core_.force(priv::encode(v, s)); }
    SendStatus unforce()         { return core_.unforce(); }
    bool forced()          const { return core_.forced(); }
    bool has_definition()  const { return core_.has_definition(); }
    int  match_count()     const { return core_.match_count(); }

    /* Observe (see the untyped core for the change/write contract). Handler forms:
     * void(const T&) or void(const T&, const VariableUpdate&); nullptr clears. */
    template <class H, class = std::enable_if_t<
        std::is_invocable_v<std::decay_t<H>&, const T&> ||
        std::is_invocable_v<std::decay_t<H>&, const T&, const VariableUpdate&>>>
    void on_change(H&& h) { core_.on_change(adapt(std::forward<H>(h))); }
    template <class H, class = std::enable_if_t<
        std::is_invocable_v<std::decay_t<H>&, const T&> ||
        std::is_invocable_v<std::decay_t<H>&, const T&, const VariableUpdate&>>>
    void on_write(H&& h)  { core_.on_write(adapt(std::forward<H>(h))); }
    void on_change(std::nullptr_t) { core_.on_change({}); }
    void on_write (std::nullptr_t) { core_.on_write({}); }
    SendStatus retire() { return core_.retire(); }

private:
    template <class H>
    static std::function<void(const VariableUpdate&)> adapt(H&& h) {
        return [f = std::forward<H>(h)](const VariableUpdate& u) mutable {
            T v{};
            if (!priv::decode(v, u.value(), u.raw_schema())) return;
            if constexpr (std::is_invocable_v<std::decay_t<H>&, const T&, const VariableUpdate&>)
                f(v, u);
            else
                f(v);
        };
    }
    RemoteVariable<> core_;
};

/* Signal<T>: the typed signal. Handler forms: void(const T&) or
 * void(const T&, const MessageView&). */
template <class T> class Signal {
public:
    Signal() = default;
    /* emit-only handle */
    Signal(Node& n, std::string_view name, const SignalOptions& o = {}) {
        const Schema* sc = priv::schema_of<T>();
        if (!sc) { priv::raise_msg("dart::Signal: DART_SCHEMA compile failed"); return; }
        core_ = Signal<>(n, name, sc, o);
    }
    /* listening handle */
    template <class H, class = std::enable_if_t<
        std::is_invocable_v<std::decay_t<H>&, const T&> ||
        std::is_invocable_v<std::decay_t<H>&, const T&, const MessageView&>>>
    Signal(Node& n, std::string_view name, H&& handler, const SignalOptions& o = {}) {
        const Schema* sc = priv::schema_of<T>();
        if (!sc) { priv::raise_msg("dart::Signal: DART_SCHEMA compile failed"); return; }
        core_ = Signal<>(n, name, sc, adapt(std::forward<H>(handler)), o);
    }
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    SendStatus emit(const T& v) {
        std::vector<uint8_t> s;
        return core_.emit(priv::encode(v, s));
    }
    int listener_count() const { return core_.listener_count(); }
    SendStatus retire() { return core_.retire(); }

private:
    template <class H>
    static std::function<void(const MessageView&)> adapt(H&& h) {
        return [f = std::forward<H>(h)](const MessageView& m) mutable {
            T v{};
            if (!priv::decode(v, m.data(), m.raw_schema())) return;
            if constexpr (std::is_invocable_v<std::decay_t<H>&, const T&, const MessageView&>)
                f(v, m);
            else
                f(v);
        };
    }
    Signal<> core_;
};

#endif /* !DART_NO_PATTERNS */

/* Publisher<T>: the typed publish side. */
template <class T> class Publisher {
public:
    Publisher() = default;
    Publisher(Node& n, std::string_view name, const Qos& qos = {}) {
        const Schema* sc = priv::schema_of<T>();
        if (!sc) { priv::raise_msg("dart::Publisher: DART_SCHEMA compile failed"); return; }
        core_ = Publisher<>(n, name, sc, qos);
    }
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    SendStatus send(const T& v) {
        std::vector<uint8_t> s;
        return core_.send(priv::encode(v, s));
    }
    int  match_count()   const { return core_.match_count(); }
    int  pending_count() const { return core_.pending_count(); }
    bool ready()         const { return core_.ready(); }
    SendStatus retire()        { return core_.retire(); }   /* see Topic::retire */
    Topic& topic()             { return core_.topic(); }

private:
    Publisher<> core_;
};

/* Subscriber<T>: the typed subscribe side. Handler forms: void(const T&) or
 * void(const T&, const MessageView&); or consume with the typed take(). */
template <class T> class Subscriber {
public:
    Subscriber() = default;
    Subscriber(Node& n, std::string_view name, const Qos& qos = {}) {
        const Schema* sc = priv::schema_of<T>();
        if (!sc) { priv::raise_msg("dart::Subscriber: DART_SCHEMA compile failed"); return; }
        core_ = Subscriber<>(n, name, sc, qos);
    }
    template <class H, class = std::enable_if_t<
        std::is_invocable_v<std::decay_t<H>&, const T&> ||
        std::is_invocable_v<std::decay_t<H>&, const T&, const MessageView&>>>
    Subscriber(Node& n, std::string_view name, H&& handler, const Qos& qos = {}) {
        const Schema* sc = priv::schema_of<T>();
        if (!sc) { priv::raise_msg("dart::Subscriber: DART_SCHEMA compile failed"); return; }
        core_ = Subscriber<>(n, name, sc, adapt(std::forward<H>(handler)), qos);
    }
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /* Pop + decode the next queued message into an OWNING Message<T> (envelope copied,
     * so it outlives the next take). timeout_ms as Topic::take. */
    std::optional<Message<T>> take(int timeout_ms = 0) {
        auto um = core_.take(timeout_ms);
        if (!um) return std::nullopt;
        Message<T> m;
        if (!priv::decode(m.v_, um->data(), um->raw_schema())) return std::nullopt;
        m.pub_     = std::string(um->publisher_name());
        m.topic_   = std::string(um->topic_name());
        m.pid_     = um->publisher_id();
        m.idx_     = um->topic_index();
        m.recv_    = um->recv_us();
        m.written_ = um->written_us();
        return m;
    }
    int dispatch(int max_msgs = 0, int timeout_ms = 0) { return core_.dispatch(max_msgs, timeout_ms); }
    SendStatus retire() { return core_.retire(); }   /* see Topic::retire */
    Topic& topic() { return core_.topic(); }

private:
    template <class H>
    static Node::MessageHandler adapt(H&& h) {
        return [f = std::forward<H>(h)](const MessageView& m) mutable {
            T v{};
            if (!priv::decode(v, m.data(), m.raw_schema())) return;
            if constexpr (std::is_invocable_v<std::decay_t<H>&, const T&, const MessageView&>)
                f(v, m);
            else
                f(v);
        };
    }
    Subscriber<> core_;
};

}   /* namespace dart */

/* ---- DART_SCHEMA(T, fields...): reflect a struct for the typed codec -------------
 * Invoke at GLOBAL scope (or any namespace enclosing ::dart), after the struct
 * definition, listing up to 64 non-static data members in wire order:
 *
 *     struct Pose { double x, y; dart::String<16> frame; };
 *     DART_SCHEMA(Pose, x, y, frame);
 *
 * The schema's wire name is the type name with namespace qualifiers stripped, so
 * matching structs in different namespaces (or languages) interoperate. */
#define DART_PP_EXPAND(x) x
#define DART_PP_CAT2(a, b) a##b
#define DART_PP_CAT(a, b) DART_PP_CAT2(a, b)
#define DART_PP_ARGN( \
    _1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16, \
    _17,_18,_19,_20,_21,_22,_23,_24,_25,_26,_27,_28,_29,_30,_31,_32, \
    _33,_34,_35,_36,_37,_38,_39,_40,_41,_42,_43,_44,_45,_46,_47,_48, \
    _49,_50,_51,_52,_53,_54,_55,_56,_57,_58,_59,_60,_61,_62,_63,_64, N, ...) N
#define DART_PP_NARG(...) DART_PP_EXPAND(DART_PP_ARGN(__VA_ARGS__, \
    64,63,62,61,60,59,58,57,56,55,54,53,52,51,50,49, \
    48,47,46,45,44,43,42,41,40,39,38,37,36,35,34,33, \
    32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17, \
    16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1))
#define DART_PP_FE_1(M, T, x) M(T, x)
#define DART_PP_FE_2(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_1(M, T, __VA_ARGS__))
#define DART_PP_FE_3(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_2(M, T, __VA_ARGS__))
#define DART_PP_FE_4(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_3(M, T, __VA_ARGS__))
#define DART_PP_FE_5(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_4(M, T, __VA_ARGS__))
#define DART_PP_FE_6(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_5(M, T, __VA_ARGS__))
#define DART_PP_FE_7(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_6(M, T, __VA_ARGS__))
#define DART_PP_FE_8(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_7(M, T, __VA_ARGS__))
#define DART_PP_FE_9(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_8(M, T, __VA_ARGS__))
#define DART_PP_FE_10(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_9(M, T, __VA_ARGS__))
#define DART_PP_FE_11(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_10(M, T, __VA_ARGS__))
#define DART_PP_FE_12(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_11(M, T, __VA_ARGS__))
#define DART_PP_FE_13(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_12(M, T, __VA_ARGS__))
#define DART_PP_FE_14(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_13(M, T, __VA_ARGS__))
#define DART_PP_FE_15(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_14(M, T, __VA_ARGS__))
#define DART_PP_FE_16(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_15(M, T, __VA_ARGS__))
#define DART_PP_FE_17(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_16(M, T, __VA_ARGS__))
#define DART_PP_FE_18(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_17(M, T, __VA_ARGS__))
#define DART_PP_FE_19(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_18(M, T, __VA_ARGS__))
#define DART_PP_FE_20(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_19(M, T, __VA_ARGS__))
#define DART_PP_FE_21(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_20(M, T, __VA_ARGS__))
#define DART_PP_FE_22(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_21(M, T, __VA_ARGS__))
#define DART_PP_FE_23(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_22(M, T, __VA_ARGS__))
#define DART_PP_FE_24(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_23(M, T, __VA_ARGS__))
#define DART_PP_FE_25(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_24(M, T, __VA_ARGS__))
#define DART_PP_FE_26(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_25(M, T, __VA_ARGS__))
#define DART_PP_FE_27(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_26(M, T, __VA_ARGS__))
#define DART_PP_FE_28(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_27(M, T, __VA_ARGS__))
#define DART_PP_FE_29(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_28(M, T, __VA_ARGS__))
#define DART_PP_FE_30(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_29(M, T, __VA_ARGS__))
#define DART_PP_FE_31(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_30(M, T, __VA_ARGS__))
#define DART_PP_FE_32(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_31(M, T, __VA_ARGS__))
#define DART_PP_FE_33(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_32(M, T, __VA_ARGS__))
#define DART_PP_FE_34(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_33(M, T, __VA_ARGS__))
#define DART_PP_FE_35(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_34(M, T, __VA_ARGS__))
#define DART_PP_FE_36(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_35(M, T, __VA_ARGS__))
#define DART_PP_FE_37(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_36(M, T, __VA_ARGS__))
#define DART_PP_FE_38(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_37(M, T, __VA_ARGS__))
#define DART_PP_FE_39(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_38(M, T, __VA_ARGS__))
#define DART_PP_FE_40(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_39(M, T, __VA_ARGS__))
#define DART_PP_FE_41(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_40(M, T, __VA_ARGS__))
#define DART_PP_FE_42(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_41(M, T, __VA_ARGS__))
#define DART_PP_FE_43(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_42(M, T, __VA_ARGS__))
#define DART_PP_FE_44(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_43(M, T, __VA_ARGS__))
#define DART_PP_FE_45(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_44(M, T, __VA_ARGS__))
#define DART_PP_FE_46(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_45(M, T, __VA_ARGS__))
#define DART_PP_FE_47(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_46(M, T, __VA_ARGS__))
#define DART_PP_FE_48(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_47(M, T, __VA_ARGS__))
#define DART_PP_FE_49(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_48(M, T, __VA_ARGS__))
#define DART_PP_FE_50(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_49(M, T, __VA_ARGS__))
#define DART_PP_FE_51(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_50(M, T, __VA_ARGS__))
#define DART_PP_FE_52(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_51(M, T, __VA_ARGS__))
#define DART_PP_FE_53(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_52(M, T, __VA_ARGS__))
#define DART_PP_FE_54(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_53(M, T, __VA_ARGS__))
#define DART_PP_FE_55(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_54(M, T, __VA_ARGS__))
#define DART_PP_FE_56(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_55(M, T, __VA_ARGS__))
#define DART_PP_FE_57(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_56(M, T, __VA_ARGS__))
#define DART_PP_FE_58(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_57(M, T, __VA_ARGS__))
#define DART_PP_FE_59(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_58(M, T, __VA_ARGS__))
#define DART_PP_FE_60(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_59(M, T, __VA_ARGS__))
#define DART_PP_FE_61(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_60(M, T, __VA_ARGS__))
#define DART_PP_FE_62(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_61(M, T, __VA_ARGS__))
#define DART_PP_FE_63(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_62(M, T, __VA_ARGS__))
#define DART_PP_FE_64(M, T, x, ...) M(T, x) DART_PP_EXPAND(DART_PP_FE_63(M, T, __VA_ARGS__))
#define DART_PP_FOR_EACH(M, T, ...) \
    DART_PP_EXPAND(DART_PP_CAT(DART_PP_FE_, DART_PP_NARG(__VA_ARGS__))(M, T, __VA_ARGS__))

#define DART_SCHEMA_FIELD(T, f) \
    dart_v(::dart::field_tag<decltype(T::f)>{}, #f, offsetof(T, f));

#define DART_SCHEMA(T, ...) \
    template <> struct dart::reflect<T> { \
        using is_dart_schema = void; \
        static_assert(std::is_standard_layout<T>::value, \
                      "DART_SCHEMA: type must be standard-layout"); \
        static constexpr const char* type_name = #T; \
        template <class V> static void visit(V&& dart_v) { \
            DART_PP_FOR_EACH(DART_SCHEMA_FIELD, T, __VA_ARGS__) \
        } \
    }

/* ---- DART_ENUM(E, options...): register a C++ `enum class` so a member of it ships as a
 * NAMED integer (wire kind `enum<uN>`, N the enum's underlying type) instead of a bare int.
 * Invoke at global scope after the enum, listing its enumerators; the wire value of each is
 * the enum's own value, so two ends agree without repeating numbers:
 *
 *     enum class Mode : uint8_t { Idle, Running, Fault };
 *     DART_ENUM(Mode, Idle, Running, Fault);
 *
 * An enum member WITHOUT a DART_ENUM still works, shipping as its plain backing integer (so
 * it cross-matches an `enum<uN>` only by width, not name). */
#define DART_ENUM_ITEM(E, x) dart_v((int64_t)(E::x), #x);
#define DART_ENUM(E, ...) \
    template <> struct dart::reflect_enum<E> { \
        using is_dart_enum = void; \
        static_assert(std::is_enum<E>::value, "DART_ENUM: type must be an enum"); \
        static constexpr const char* type_name = #E; \
        template <class V> static void visit(V&& dart_v) { \
            DART_PP_FOR_EACH(DART_ENUM_ITEM, E, __VA_ARGS__) \
        } \
    }

/* ---- the standard type registrations (global scope: they specialize dart::reflect and
 * dart::std_type). Each mirror declared near the top of this header gets its wire NAME
 * here, so a member of one spells as `at: Pose` and matches only a Pose. */
DART_STD_STRUCT(Float2);   DART_STD_STRUCT(Float3);   DART_STD_STRUCT(Float4);
DART_STD_STRUCT(Double2);  DART_STD_STRUCT(Double3);  DART_STD_STRUCT(Double4);
DART_STD_STRUCT(Int2);     DART_STD_STRUCT(Int3);     DART_STD_STRUCT(Int4);
DART_STD_STRUCT(Quaternion);
DART_STD_STRUCT(Color);    DART_STD_STRUCT(Rect);     DART_STD_STRUCT(RectI);
DART_STD_STRUCT(Pose);     DART_STD_STRUCT(Twist);    DART_STD_STRUCT(GeoPoint);
DART_STD_ALIAS(Uuid,      uint8_t[16]);
DART_STD_ALIAS(Timestamp, int64_t);
DART_STD_ALIAS(Duration,  int64_t);
DART_STD_ALIAS(Matrix3x3, float[9]);
DART_STD_ALIAS(Matrix4x4, float[16]);
DART_STD_ALIAS(Uri,       dart::String<256>);

#endif /* C++ consumer (not the implementation anchor) */
#endif /* DART_HPP_INCLUDED */
