/* The C++ wrapper: a header only layer over the C library, with the C API hidden inside
 * rant::detail. docs/cpp.md explains how to use it. */
#ifndef RANT_HPP_INCLUDED
#define RANT_HPP_INCLUDED

/* rant.h is embedded below exactly once. With RANT_IMPLEMENTATION it lands at global scope
 * as the implementation, else inside namespace rant::detail as declarations only. */
#if !defined(RANT_IMPLEMENTATION) && !defined(__cplusplus)
#define RANT_IMPLEMENTATION
#endif

#if defined(__cplusplus) && !defined(RANT_IMPLEMENTATION)

#include <array>
#include <atomic>
#include <chrono>
#include <climits>
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
#if defined(__has_include) && __has_include(<span>) && \
    (__cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L))
#include <span>
#endif

/* Pre include the C std headers rant.h pulls, so their guards are set before the embed
 * and no std name is dragged into rant::detail. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(RANT_STRING_H) || defined(RANT_TRANSPORT_H) || defined(RANT_NODE_H)
#error "include rant.hpp instead of rant.h (do not include rant.h before rant.hpp)"
#endif

namespace rant {
namespace detail {

#endif  /* C++ consumer: open the hiding namespace before the embed */

/* ---- embedded C library (dist/rant.h spliced here by tools/pack.cmake) ---- */
#include "rant.h"   /* @RANT_EMBED@ */

#if defined(__cplusplus) && !defined(RANT_IMPLEMENTATION)
}   /* namespace detail */

/* Enums, 1:1 with the C enums by value and asserted below. */
enum class Reliability { BestEffort = 0, Reliable = 1 };
enum class Role        { PubSub = 0, PubOnly = 1, SubOnly = 2, Inactive = 3 };

/* rant_topic_send and create result. Ok is 0, the rest mirror RantResult. */
enum class SendStatus    { Ok = 0, NoTopic = -1, TooBig = -2, BadRole = -3, OutOfMemory = -4,
                         State = -5, NoSys = -6, Schema = -7 };

enum class EventKind {
    PeerUp = 0, PeerDown, PeerInterest, MessageLost, Error
};

/* The error carried by an EventKind::Error event, mirrors RantErrorKind. */
enum class ErrorKind {
    None = 0,
    NameCollision, QosIncompatible, KindMismatch, SchemaMismatch, InterestOverflow,
    MetaTruncatedInterest, PeerMetaTooBig, MessageTooBig,
    PeerRefused, EvictedUnsent, UnmatchedSend, DuplicateAuthority,
    Oom, Platform, Socket, Bind, McastJoin, Send, Recv, Poll, Waker, BadAddress,
    BadName, State, BadSchema   /* a create refused: the name, the moment, the schema */
};

/* Schema field kinds for reflection, the C wire values. Array and String are fixed,
 * VString, VArray and Map ride the message tail and report offset and size 0. */
enum class FieldType : uint8_t {
    U8 = 0, U16, U32, U64, I8, I16, I32, I64, F32, F64, Bool, Array, Struct, String,
    VString, VArray, Map, Enum, Named
};

/* A call's outcome, mirrors RantCallStatus. Timeout, PeerLost and NoProvider are
 * synthesized on the caller. Running is task only and the one non terminal status
 * (docs/tasks.md). */
enum class CallStatus { Ok = 0, AppError = 1, NoHandler = 2, Timeout = 3, PeerLost = 4,
                        Cancelled = 5, Running = 6, NoProvider = 7 };

/* What a network entity is, mirrors RantEntityKind. Observers see folded entities, never
 * raw channels (docs/reflection.md). */
enum class EntityKind { Topic = 0, Function, Variable, Task };

/* Severity of a built-in @rant/log line (mirrors RantLogLevel). */
enum class LogLevel { Error = 0, Warn = 1, Info = 2 };

static_assert((int)Reliability::Reliable == detail::RANT_RELIABLE, "reliability enum drift");
static_assert((int)Role::Inactive == detail::RANT_INACTIVE, "role enum drift");
static_assert((int)SendStatus::NoSys == detail::RANT_ERR_NOSYS, "result enum drift");
static_assert((int)SendStatus::Schema == detail::RANT_ERR_SCHEMA, "result enum drift");
static_assert((int)EventKind::Error == detail::RANT_ERROR, "event enum drift");
static_assert((int)ErrorKind::Waker == detail::RANT_E_WAKER, "error enum drift");
static_assert((int)ErrorKind::BadAddress == detail::RANT_E_BAD_ADDRESS, "error enum drift");
static_assert((int)ErrorKind::BadSchema == detail::RANT_E_BAD_SCHEMA, "error enum drift");
static_assert((int)FieldType::Struct == detail::RANT_STRUCT, "field-type enum drift");
static_assert((int)FieldType::String == detail::RANT_STR, "field-type enum drift");
static_assert((int)FieldType::Map == detail::RANT_MAP, "field-type enum drift");
static_assert((int)FieldType::Enum == detail::RANT_ENUM, "field-type enum drift");
static_assert((int)FieldType::Named == detail::RANT_NAMED, "field-type enum drift");
#ifndef RANT_NO_PATTERNS
static_assert((int)CallStatus::Ok == detail::RANT_CALL_OK, "call-status enum drift");
static_assert((int)CallStatus::PeerLost == detail::RANT_CALL_PEER_LOST, "call-status enum drift");
static_assert((int)CallStatus::NoProvider == detail::RANT_CALL_NO_PROVIDER, "call-status enum drift");
static_assert((int)CallStatus::Cancelled == detail::RANT_CALL_CANCELLED, "call-status enum drift");
static_assert((int)CallStatus::Running == detail::RANT_CALL_RUNNING, "call-status enum drift");
static_assert((int)EntityKind::Topic == detail::RANT_ENTITY_TOPIC, "entity enum drift");
static_assert((int)EntityKind::Variable == detail::RANT_ENTITY_VARIABLE, "entity enum drift");
static_assert((int)EntityKind::Task == detail::RANT_ENTITY_TASK, "entity enum drift");
#endif
static_assert((int)LogLevel::Info == detail::RANT_LOG_INFO, "log-level enum drift");

/* forward decls. Every handle is a template over the message type, and the type argument
 * rant::Bytes is the raw form that carries a DSL schema and reads through MessageView. */
class Node;
class Reflection;
class MessageView;
class Event;
class Schema;
namespace priv { class TopicCore; struct NodeImpl; }
template <class Req, class Rsp> class FunctionDefinition;
template <class Req, class Rsp> class RemoteFunction;
template <class Req, class Prg, class Rsp> class TaskDefinition;
template <class Req, class Prg, class Rsp> class RemoteTask;
template <class T> class VariableDefinition;
template <class T> class RemoteVariable;
class VariableUpdate;
template <class T> class Publisher;
template <class T> class Subscriber;
template <class Rsp> class Request;
template <class Rsp> class Deferred;
template <class Prg, class Rsp> class TaskRequest;
template <class Prg, class Rsp> class PendingTask;
template <class Prg> class ProgressView;
template <class Rsp> class Response;
template <class Rsp> class ResponseView;

/* The one exception type, thrown by failed constructors only. Carries the error kind, the
 * OS errno of a socket fault and the formatted text. Never thrown under -fno-exceptions. */
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

/* raise helpers: throw rant::Error when exceptions are on, else return (the caller
 * leaves its handle invalid). raise_last formats rant_last_error(n) as the reason. */
inline void raise_last(detail::RantNode* n, const char* what) {
#if defined(__cpp_exceptions)
    detail::RantEvent e = detail::rant_last_error(n);
    char b[192];
    std::string t = what ? std::string(what) + ": " : std::string();
    t += detail::rant_event_str(&e, b, sizeof b);
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

/* A non owning view of a payload and a contiguous range. It converts from and to the
 * standard string and byte containers (docs/cpp.md). */
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

    /* any contiguous range of byte sized elements. Strings take the overloads above, so
     * this covers vector, array and span of uint8_t, char or std::byte without ambiguity */
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
inline detail::RantBytes    to_c(Bytes b) { return detail::rant_bytes(b.data(), b.size()); }
}

/* A wait with no end, for the timeouts that take one. */
inline constexpr std::chrono::milliseconds forever{ -1 };

namespace priv {
/* A timeout for the C API: whole milliseconds, clamped, any negative one -1. An empty
 * optional is -1 too, which the C reads as the handle's default. */
inline int to_ms(std::chrono::milliseconds d) {
    const auto c = d.count();
    return c < 0 ? -1 : c > INT_MAX ? INT_MAX : static_cast<int>(c);
}
inline int to_ms(const std::optional<std::chrono::milliseconds>& d) { return d ? to_ms(*d) : -1; }
/* An option duration for the C config: microseconds, 0 for none or negative, clamped. */
inline uint32_t to_us(std::chrono::microseconds d) {
    const auto c = d.count();
    return c <= 0 ? 0u : c > (long long)UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(c);
}
}


/* A callback queue of a node, from Node::create_queue. A handle whose options name it parks
 * its callbacks until dispatch() runs them on the calling thread. Non owning, freed with the node. */
class Queue {
public:
    Queue() = default;
    bool valid() const noexcept { return q_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }
    /* Run the callbacks parked at entry, oldest first, up to max_callbacks (0 = all), waiting
     * up to timeout for the first (rant::forever = no end). The count run, or State from a
     * callback or while another thread dispatches this queue. */
    int dispatch(int max_callbacks = 0, std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        return q_ ? detail::rant_queue_dispatch(q_, max_callbacks, priv::to_ms(timeout))
                  : static_cast<int>(SendStatus::State);
    }
    struct Stats { uint32_t waiting = 0; uint32_t dropped = 0; };
    /* Callbacks parked now, and dropped since open. */
    Stats stats() const {
        Stats s;
        if (q_) detail::rant_queue_stats(q_, &s.waiting, &s.dropped);
        return s;
    }
    detail::RantQueue* raw() const noexcept { return q_; }
private:
    friend class Node;
    explicit Queue(detail::RantQueue* q) : q_(q) {}
    detail::RantQueue* q_ = nullptr;
};

namespace priv {
inline detail::RantQueue* to_c(const Queue* q) { return q ? q->raw() : nullptr; }
}

/* Qos / NodeOptions: plain structs mirroring the C config, all zero = default. */
struct Qos {
    Reliability reliability          = Reliability::BestEffort;
    uint16_t    keep_last            = 0;   /* recent messages retained (late join / repair) */
    uint16_t    catch_up             = 0;   /* recent messages a new subscriber replays */
    uint32_t    max_message_bytes    = 0;   /* 0 = one fragment, or grow-to-fit */
    std::chrono::microseconds heartbeat{ 0 };           /* reliable idle writer ping, 0 = 100 ms */
    std::chrono::microseconds repair_delay{ 0 };        /* reader re ask bound, 0 = adaptive from the RTT */
    std::chrono::microseconds backpressure_wait{ 0 };   /* reliable: send pause for a slow reader, 0 = none */
    uint32_t    shm_max_bytes        = 0;   /* pin topic to one same-host SHM size class */
    uint32_t    queue_bytes          = 0;   /* the handle's ring on its queue, 0 = 1 MB */
    uint16_t    max_rate_hz          = 0;   /* best effort sub: kept samples per second, 0 = all */
    bool        no_timestamp         = false;   /* omit the source stamp, written_us() reads 0 */
    /* The queue a subscriber's handler parks on, null = inline on the loop thread. Same name
     * handles share one slot and must agree on it. */
    const Queue* queue               = nullptr;
    /* A rant::Bytes handle with no schema takes its type from the mesh (docs/reflection.md).
     * refresh() re types it later. */
    bool         reflect_from_mesh   = false;
};

/* Who runs the node loop and where callbacks fire, chosen at open. */
enum class Threading {
    ServiceThread = 0,   /* the service thread runs from construction, callbacks fire on it */
    Manual        = 1,   /* your thread calls poll(), callbacks fire there */
    Dispatch      = 2    /* the service thread runs, callbacks wait until node.dispatch() */
};

struct NodeOptions {
    Threading                threading            = Threading::ServiceThread;
    uint16_t                 domain               = 0;   /* logical-network selector */
    uint16_t                 max_topics           = 8;   /* how many topics may be created */
    bool                     disable_shm          = false;
    bool                     fetch_details        = false;   /* fetch every peer topic's schema */
    std::chrono::milliseconds match_wait{ 0 };   /* 0 = 1 s, negative = drop loudly */
    /* networking (all optional) */
    /* built in observability, all on by default (Node::log, Node::on_log, Reflection::meta) */
    bool                     disable_logs         = false; /* strip the @rant/log/{error,warn,info}
                                                              topics (saves their history memory) */
    bool                     disable_meta         = false; /* do not host the @rant/meta endpoint */
    bool                     disable_error_logs   = false; /* suppress the default mirroring of this
                                                              node's errors onto @rant/log/error */
    uint16_t                 data_port            = 0;   /* 0 = OS-assigned */
    std::string              discovery_group;            /* empty = "239.255.0.<domain>" default */
    uint16_t                 discovery_port       = 0;   /* 0 = 7400 */
    std::string              multicast_interface;   /* empty = auto, "127.0.0.1" = single host */
    uint8_t                  multicast_ttl        = 0;   /* 0 = 1 hop */
    std::vector<std::string> seed_peers;   /* "ip" or "ip:port", unicast announce targets */
    bool                     unicast_only         = false;   /* no group join, seeds relay us */
    uint16_t                 fragment_size        = 0;   /* UDP payload bytes per fragment */
    /* Data socket OS buffers. 0 = the OS default, which is too small to hold a multi
     * megabyte message whole, so raise both for big payloads. See docs/discovery.md. */
    uint32_t                 recv_buffer_bytes    = 0;
    uint32_t                 send_buffer_bytes    = 0;
    /* Advertise this locator to every peer instead of letting each learn it from the datagram
     * source. For a static 1:1 mapping such as a cloud IP or a published container port. */
    std::string              self_ip;   /* "203.0.113.7", empty = learn per path */
    uint16_t                 advertise_port       = 0;   /* 0 = the port we actually bound */
    /* discovery cadence */
    std::chrono::microseconds announce_interval{ 0 };   /* 0 = 1 s */
    std::chrono::microseconds peer_timeout{ 0 };        /* 0 = 3.5 s */
    uint16_t                 max_peers            = 0;   /* 0 = 16 */
    /* Leave memory null for the dynamic allocator. A fixed buffer means static mode: no heap,
     * no growth, shared memory off, and construction fails if it is too small (docs/cpp.md). */
    void*                    memory      = nullptr;
    size_t                   memory_size = 0;
};

#ifndef RANT_NO_PATTERNS
/* Per-pattern options (all zero = defaults). */
struct FunctionOptions {
    std::chrono::microseconds backpressure_wait{ 0 };   /* 0 = 1 s: patterns are low rate, loss unacceptable */
    std::chrono::microseconds timeout{ 0 };             /* remote call timeout, 0 = 5 s */
    uint16_t keep_last            = 0;   /* req and rsp ring depth, 0 = 10 */
    const Queue* queue            = nullptr;   /* where every callback parks, null = inline */
    bool     reflect_from_mesh    = false;   /* rant::Bytes only: take the schemas from the mesh */
};
/* Task options, mirrors RantTaskOpts (docs/tasks.md). */
struct TaskOptions {
    bool     progress_best_effort = false;   /* the definition offers, a remote requests */
    uint16_t progress_keep_last   = 0;   /* progress ring depth, 0 = the pattern default */
    bool     no_cancel            = false;   /* cancel not honored, remotes get BadRole */
    bool     exclusive            = false;   /* declared serialization, handler enforced */
    bool     multi                = false;   /* redundant providers, one executor each */
    std::chrono::microseconds timeout{ 0 };             /* remote: bound until the first response, 0 = 5 s */
    std::chrono::microseconds backpressure_wait{ 0 };   /* 0 = 1 s */
    uint16_t keep_last            = 0;     /* req + rsp ring depth (FunctionOptions::keep_last) */
    const Queue* queue            = nullptr;   /* where every callback parks, null = inline */
    bool     reflect_from_mesh    = false;   /* rant::Bytes only: take the schemas from the mesh */
};
/* Per call options, mirrors RantCallOpts. provider directs a call at one definition by peer
 * id, 0 = first answer wins. A task request is always directed, 0 = the oldest provider. */
struct CallOptions {
    uint32_t  provider = 0;
    uint32_t* id_out   = nullptr;   /* receives the call id at commit (before any wait): the
                                       handle for RemoteTask::cancel from another thread */
};
/* VariableOptions<T> for the typed definition, VariableOptions<Bytes> the raw twin whose
 * initial value is raw Bytes. */
template <class T> struct VariableOptions {
    std::optional<T> initial{};              /* the value before any set */
    bool     read_only            = false;   /* no set channel: remote sets get BadRole */
    bool     allow_force          = false;   /* permit force (local + remote) */
    uint16_t catch_up             = 0;   /* value channel catch_up, 0 = 1 */
    uint16_t keep_last            = 0;   /* both channels' repair window, 0 = 10 */
    std::chrono::microseconds backpressure_wait{ 0 };   /* 0 = 1 s */
    const Queue* queue            = nullptr;   /* where on_change and on_write park, null = inline */
    bool     reflect_from_mesh    = false;   /* rant::Bytes only: take the schema from the mesh */
};
template <> struct VariableOptions<Bytes> {
    Bytes    initial{};
    bool     read_only            = false;
    bool     allow_force          = false;
    uint16_t catch_up             = 0;
    uint16_t keep_last            = 0;
    std::chrono::microseconds backpressure_wait{ 0 };
    const Queue* queue            = nullptr;
    bool     reflect_from_mesh    = false;
};
#endif /* !RANT_NO_PATTERNS */

/* Schema: a compiled message schema the node owns, from Node::schema. A value that stays
 * valid for the node's life, copyable and cheap. */
class Schema {
public:
    /* An EMPTY schema (empty() true, raw() null): what an untyped entity reports, and the
     * state a default-constructed member holds until assigned. */
    Schema() = default;
    /* Wraps a schema a node holds, such as one a raw C call returned. */
    explicit Schema(const detail::RantSchema* raw) : schema_(raw) {}

    bool empty() const noexcept { return schema_ == nullptr; }
    explicit operator bool() const noexcept { return schema_ != nullptr; }

    std::string_view name() const {
        if (!schema_) return {};
        detail::RantString n = detail::rant_schema_name(schema_);
        return { n.data, n.len };
    }
    uint32_t size() const { return schema_ ? detail::rant_schema_size(schema_) : 0; }
    uint64_t hash() const { return schema_ ? detail::rant_schema_hash(schema_) : 0; }
    uint16_t field_count() const { return schema_ ? detail::rant_schema_field_count(schema_) : 0; }
    /* Flat index of a field by name, nested members by dotted path ("velocity.dx") and struct
     * array members by index ("corners[2].x"). -1 if unknown. */
    int field_index(std::string_view path) const {
        return detail::rant_schema_field_index(schema_, std::string(path).c_str());
    }
    /* Can a reader declaring THIS schema read messages written with `pub`? Type NAMES
     * narrow: an anonymous type reads a named one, never the reverse. */
    bool can_read(const Schema& pub) const {
        return schema_ && pub.schema_ && detail::rant_schema_subset(schema_, pub.schema_) != 0;
    }
    /* Why this schema cannot read pub, one line. Empty when it can. */
    std::string why_not(const Schema& pub) const {
        char buf[256];
        if (!schema_ || !pub.schema_) return "no schema";
        if (detail::rant_schema_subset_why(schema_, pub.schema_, buf, sizeof buf)) return {};
        return buf;
    }
    /* Spell the schema back as compile-ready DSL text (the inverse of compile), with
     * every named type it uses hoisted to a leading definition. */
    std::string to_dsl() const {
        if (!schema_) return {};
        uint32_t n = detail::rant_schema_print(schema_, nullptr, 0);
        std::string out(n, '\0');
        if (n) detail::rant_schema_print(schema_, &out[0], n + 1);
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
        uint16_t  arr_parent;    /* flat index of the nearest enclosing struct ARRAY, 0xFFFF for none */
        uint16_t  arr_depth;     /* the struct arrays around the field, one index each */
        uint32_t  offset, size;
        uint32_t  elem_size;     /* bytes of one array element, else 0 */
    };
    bool field_at(uint16_t i, Field& out) const {
        detail::RantSchemaFieldInfo f;
        if (!schema_ || !detail::rant_schema_field_at(schema_, i, &f)) return false;
        out.name      = { f.name.data, f.name.len };
        out.type_name = { f.type_name.data ? f.type_name.data : "", f.type_name.len };
        out.elem_name = { f.elem_name.data ? f.elem_name.data : "", f.elem_name.len };
        out.kind   = static_cast<FieldType>(f.kind);
        out.elem   = static_cast<FieldType>(f.elem);
        out.count  = f.count; out.depth = f.depth;
        out.str_cap = f.str_cap; out.arr_parent = f.arr_parent; out.arr_depth = f.arr_depth;
        out.offset = f.offset; out.size = f.size; out.elem_size = f.elem_size;
        return true;
    }

    /* The whole flat field table (views into this schema: keep it alive while you read). */
    std::vector<Field> fields() const {
        std::vector<Field> out;
        Field f;
        for (uint16_t i = 0; field_at(i, f); i++) out.push_back(f);
        return out;
    }

    /* Enum options (by flat field index). One option of an Enum field. */
    struct EnumVariant { int64_t value; std::string_view name; };
    uint16_t enum_count(uint16_t field) const { return detail::rant_schema_enum_count(schema_, field); }
    bool enum_variant(uint16_t field, uint16_t i, EnumVariant& out) const {
        detail::RantString n; int64_t v;
        if (!detail::rant_schema_enum_variant(schema_, field, i, &v, &n)) return false;
        out.value = v; out.name = { n.data, n.len };
        return true;
    }

    /* The raw compiled schema, opaque to consumers. The create calls use it. */
    const detail::RantSchema* raw() const { return schema_; }

private:
    const detail::RantSchema* schema_ = nullptr;
};

/* The self describing tagged value tree of a map field: a thin layer over the C map codec.
 * Write with MapWriter, read with MapReader, or decode whole with MapReader::to_map. */
class MapReader;
class MapItem;
using MapList = std::vector<MapItem>;             /* a decoded array value's elements */
using MapDict = std::map<std::string, MapItem>;   /* a decoded map: the std::map readback */

/* Builds a map body into a fixed internal buffer sized by the ctor arg. Key order is yours,
 * array elements pass a null key. finish() returns empty on overflow, so check ok(). */
class MapWriter {
public:
    explicit MapWriter(size_t capacity = 512) : buf_(capacity ? capacity : 1) {
        w_ = detail::rant_map_begin(buf_.data(), buf_.size());
    }
    MapWriter(const MapWriter&) = delete;             /* holds a raw buffer pointer */
    MapWriter& operator=(const MapWriter&) = delete;

    MapWriter& put_uint  (const char* key, uint64_t v)         { detail::rant_map_put_uint    (&w_, key, v); return *this; }
    MapWriter& put_int   (const char* key, int64_t  v)         { detail::rant_map_put_int     (&w_, key, v); return *this; }
    MapWriter& put_f64   (const char* key, double   v)         { detail::rant_map_put_f64     (&w_, key, v); return *this; }
    MapWriter& put_f32   (const char* key, float    v)         { detail::rant_map_put_f32     (&w_, key, v); return *this; }
    MapWriter& put_bool  (const char* key, bool     v)         { detail::rant_map_put_bool    (&w_, key, v ? 1 : 0); return *this; }
    MapWriter& put_string(const char* key, std::string_view v) { detail::rant_map_put_string(&w_, key, detail::rant_string(v.data(), v.size())); return *this; }
    /* nested map / array: open, write members (array members pass key = nullptr), close */
    MapWriter& open_map  (const char* key) { detail::rant_map_open_map    (&w_, key); return *this; }
    MapWriter& open_array(const char* key) { detail::rant_map_open_array(&w_, key); return *this; }
    MapWriter& close()                     { detail::rant_map_close       (&w_);      return *this; }

    bool  ok() const { return w_.err == 0; }
    Bytes finish() { uint32_t n = detail::rant_map_finish(&w_); return { buf_.data(), n }; }

private:
    std::vector<uint8_t>    buf_;
    detail::RantMapWriter w_{};
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
    uint16_t array_count()          const { return detail::rant_map_array_count(v_.bytes); }
    MapValue array_at(uint16_t i)   const { MapValue o; detail::rant_map_array_at(v_.bytes, i, &o.v_); return o; }

private:
    detail::RantValue v_{};
    friend class MapReader;
};

/* Reads a map body: by key, or iterate by index. Every walk is bounds-checked in the
 * C core. The body is a view into the message, so read it inside the handler. */
class MapReader {
public:
    MapReader() = default;
    explicit MapReader(Bytes body) : body_(detail::rant_bytes(body.data(), body.size())) {}
    uint16_t count() const { return detail::rant_map_count(body_); }
    bool get(const char* key, MapValue& out) const { return detail::rant_map_get(body_, key, &out.v_) != 0; }
    bool at(uint16_t i, std::string_view& key, MapValue& out) const {
        detail::RantString k;
        if (!detail::rant_map_at(body_, i, &k, &out.v_)) return false;
        key = { k.data, k.len };
        return true;
    }
    Bytes body() const { return { body_.data, body_.len }; }
    /* Decode the whole map into an owning std::map, copied out of the message so it outlives
     * the handler. Keys are sorted. Empty for a mismatched field. */
    MapDict to_map() const;

private:
    detail::RantBytes body_{};
};

inline MapReader MapValue::as_map() const { return MapReader(Bytes{ v_.bytes.data, v_.bytes.len }); }

/* One node of a decoded map: an owning scalar, string, MapList or MapDict. Numeric getters
 * coerce between kinds and a mismatch yields the zero value, nothing throws. */
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

/* A mutable message buffer bound to a Schema. Set fields by name or dotted path, then pass
 * it to Publisher<Bytes>::send. Variable fields grow the buffer, an over cap value flips ok() false. */
class MessageBuilder {
public:
    explicit MessageBuilder(const Schema& s) : schema_(s.raw()), buf_(detail::rant_schema_msg_min(s.raw())) {
        detail::rant_schema_message_default(schema_, buf_.data(), buf_.size());
    }
    MessageBuilder& set_uint (const char* field, uint64_t v) { ok_ &= detail::rant_set_uint (buf_.data(), buf_.size(), schema_, field, v) != 0; return *this; }
    MessageBuilder& set_int  (const char* field, int64_t  v) { ok_ &= detail::rant_set_int    (buf_.data(), buf_.size(), schema_, field, v) != 0; return *this; }
    MessageBuilder& set_f64  (const char* field, double   v) { ok_ &= detail::rant_set_f64    (buf_.data(), buf_.size(), schema_, field, v) != 0; return *this; }
    MessageBuilder& set_f32  (const char* field, float    v) { ok_ &= detail::rant_set_f32    (buf_.data(), buf_.size(), schema_, field, v) != 0; return *this; }
    MessageBuilder& set_bool (const char* field, bool     v) { ok_ &= detail::rant_set_uint (buf_.data(), buf_.size(), schema_, field, v ? 1u : 0u) != 0; return *this; }
    /* a capped OR variable string (rant_set_string handles both) */
    MessageBuilder& set_string(const char* field, std::string_view v) {
        grow_for(v.size());
        ok_ &= detail::rant_set_string(buf_.data(), buf_.size(), schema_, field, detail::rant_string(v.data(), v.size())) != 0;
        return *this;
    }
    /* one element of a string array (a variable string array must be grown first with
       a set_array of empty slots, per the C API) */
    /* Size a variable array to count elements, zero filled, so a struct array's members can
     * then be set by path such as "pts[2].x". */
    MessageBuilder& set_array_count(const char* field, uint32_t count) {
        int i = detail::rant_schema_field_index(schema_, field);
        detail::RantSchemaFieldInfo fi;
        if (i < 0 || !detail::rant_schema_field_at(schema_, (uint16_t)i, &fi)) { ok_ = false; return *this; }
        grow_for((size_t)count * fi.elem_size);
        ok_ &= detail::rant_set_array_count(buf_.data(), buf_.size(), schema_, field, count) != 0;
        return *this;
    }
    MessageBuilder& set_string_at(const char* field, uint16_t index, std::string_view v) {
        grow_for(v.size() + 2);
        ok_ &= detail::rant_set_string_at(buf_.data(), buf_.size(), schema_, field, index, detail::rant_string(v.data(), v.size())) != 0;
        return *this;
    }
    /* a fixed or variable array as raw element bytes. A string array takes whole
       [u16 len][cap] slots */
    MessageBuilder& set_array(const char* field, Bytes elems) {
        grow_for(elems.size());
        ok_ &= detail::rant_set_array(buf_.data(), buf_.size(), schema_, field, detail::rant_bytes(elems.data(), elems.size())) != 0;
        return *this;
    }
    /* a `map` field, from a finished map body */
    MessageBuilder& set_map(const char* field, Bytes map_body) {
        grow_for(map_body.size());
        ok_ &= detail::rant_set_map(buf_.data(), buf_.size(), schema_, field, detail::rant_bytes(map_body.data(), map_body.size())) != 0;
        return *this;
    }
    MessageBuilder& set_map(const char* field, MapWriter& w) { return set_map(field, w.finish()); }
    /* an enum field by number or by option name. An unknown name is refused, ok() false */
    MessageBuilder& set_enum(const char* field, int64_t value) {
        ok_ &= detail::rant_set_int(buf_.data(), buf_.size(), schema_, field, value) != 0;
        return *this;
    }
    MessageBuilder& set_enum(const char* field, std::string_view name) {
        std::string n(name);
        ok_ &= detail::rant_set_enum(buf_.data(), buf_.size(), schema_, field, n.c_str()) != 0;
        return *this;
    }

    /* false if any setter was refused (over-cap string, unknown field, short buffer) */
    bool ok() const { return ok_; }
    /* the live message bytes (fixed section + its variable tail) */
    Bytes bytes() const { return { buf_.data(), detail::rant_schema_msg_len(schema_, buf_.data(), buf_.size()) }; }
    operator Bytes() const { return bytes(); }

private:
    /* ensure room for a variable frame to grow: the new content can add at most its
       own length past the current live length. resize preserves the live prefix. */
    void grow_for(size_t extra) {
        size_t used = detail::rant_schema_msg_len(schema_, buf_.data(), buf_.size());
        if (buf_.size() < used + extra) buf_.resize(used + extra);
    }
    const detail::RantSchema* schema_;
    std::vector<uint8_t>      buf_;
    bool                      ok_ = true;
};

/* The shared read surface over payload bytes and a schema, derived by MessageView,
 * Request<Bytes> and ResponseView<Bytes>. Reads are meaningful only when has_schema(). */
class FieldView {
public:
    Bytes            data() const { return { d_.data, d_.len }; }
    std::string_view text() const { return { reinterpret_cast<const char*>(d_.data), d_.len }; }
    bool             has_schema() const { return s_ != nullptr; }
    /* the raw compiled schema the payload decodes with, feeds the typed codec */
    const detail::RantSchema* raw_schema() const { return s_; }

    /* Typed field reads by name or dotted path, meaningful only when has_schema(). */
    uint64_t get_uint (const char* field) const { return detail::rant_get_uint (d_, s_, field); }
    int64_t  get_int  (const char* field) const { return detail::rant_get_int    (d_, s_, field); }
    double   get_f64  (const char* field) const { return detail::rant_get_f64    (d_, s_, field); }
    float    get_f32  (const char* field) const { return detail::rant_get_f32    (d_, s_, field); }
    bool     get_bool (const char* field) const { return detail::rant_get_uint (d_, s_, field) != 0; }
    Bytes    get_array(const char* field) const { auto a = detail::rant_get_array(d_, s_, field); return { a.data, a.len }; }
    /* an array's live element count, a fixed one's count, 0 when the field is no array */
    uint32_t get_array_count(const char* field) const { return detail::rant_get_array_count(d_, s_, field); }
    /* a capped or variable string, an empty view on a mismatch */
    std::string_view get_string(const char* field) const { auto s = detail::rant_get_string(d_, s_, field); return { s.data, s.len }; }
    std::string_view get_string_at(const char* field, uint16_t index) const { auto s = detail::rant_get_string_at(d_, s_, field, index); return { s.data, s.len }; }
    /* a `map` field, as a reader over its body (valid while the view is) */
    MapReader get_map(const char* field) const { auto b = detail::rant_get_map(d_, s_, field); return MapReader(Bytes{ b.data, b.len }); }
    /* an enum field's current option name, {} when the stored number has no option */
    std::string_view get_enum_name(const char* field) const { auto s = detail::rant_get_enum(d_, s_, field); return { s.data, s.len }; }

protected:
    FieldView() = default;
    FieldView(detail::RantBytes d, const detail::RantSchema* s) : d_(d), s_(s) {}
    detail::RantBytes           d_{};
    const detail::RantSchema* s_ = nullptr;
};

/* A delivered message. Non owning, valid only inside the handler. */
class MessageView : public FieldView {
public:
    std::string_view publisher_name() const { return { msg_->publisher_name.data,  msg_->publisher_name.len  }; }
    uint32_t         publisher_id()   const { return msg_->publisher_id; }
    std::string_view topic_name()     const { return { msg_->topic_name.data, msg_->topic_name.len }; }
    /* node monotonic us when the poll RECEIVED it (queued: at enqueue), so paced
     * consumers measure true arrival times, never their own cadence */
    uint64_t         recv_us()        const { return msg_->recv_us; }
    /* the writer's wall clock in UTC us when it wrote the message, kept across repair and
     * replay. 0 = the publisher opted out. Never mix it with the monotonic recv_us. */
    uint64_t         written_us()        const { return msg_->written_us; }
    /* When the data was true, as against when it was sent. 0 = the publisher gave none. */
    uint64_t         capture_us()        const { return msg_->capture_us; }

private:
    explicit MessageView(const detail::RantMsg* m) : FieldView(m->data, m->schema), msg_(m) {}
    const detail::RantMsg* msg_;
    friend class Node;
    friend class Subscriber<Bytes>;
};

/* A peer, message loss or error notification. Everything that goes wrong arrives as
 * EventKind::Error with error() set, and to_string() formats any of them. */
class Event {
public:
    EventKind        kind()           const { return static_cast<EventKind>(ev_->kind); }
    ErrorKind        error()          const { return static_cast<ErrorKind>(ev_->error); }
    bool             is_error()       const { return ev_->kind == detail::RANT_ERROR; }
    uint32_t         peer()           const { return ev_->peer; }
    /* the peer's node name for peer scoped events, empty when unknown. Prefer it over
       peer() in messages, an id means nothing to a human */
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
    std::string to_string() const { char b[192]; return detail::rant_event_str(ev_, b, sizeof b); }

private:
    explicit Event(const detail::RantEvent* e) : ev_(e) {}
    const detail::RantEvent* ev_;
    friend class Node;
    friend struct priv::NodeImpl;
};

/* One folded network entity as an owned snapshot, schemas included (docs/reflection.md).
 * name is the base name, a "0x????????" placeholder until the peer's details arrive. */
struct Entity {
    EntityKind  kind = EntityKind::Topic;
    std::string name;
    uint32_t    hash = 0;             /* the primary channel's low-32 name hash (the placeholder) */
    bool        provides   = false;   /* a publisher, definition or owner is present */
    bool        consumes   = false;   /* a subscriber, caller or accessor is present */
    bool        reliable   = false;   /* the primary channel's reliability */
    bool        writable   = false;   /* VARIABLE: a set channel is advertised */
    bool        forceable  = false;   /* VARIABLE: the owner permits force */
    bool        cancellable= false;   /* TASK: the provider honors cancel */
    bool        exclusive  = false;   /* TASK: declared serialization */
    bool        multi      = false;   /* duplicate authority is intended */
    bool        incomplete = false;   /* a pattern half pair: surfaced, never silently dropped */
    bool        conflict   = false;   /* mesh: live schemas that cannot read each other */
    uint16_t    providers = 0;        /* live endpoints on each side */
    uint16_t    consumers = 0;
    uint32_t    provider = 0;   /* the ranked provider's peer id, Self = us, iff providers > 0 */
    std::string from;                 /* the node the schemas were read from */
    Schema      schema;   /* value, request or payload. empty() when untyped or unfetched */
    Schema      rsp_schema;           /* FUNCTION / TASK: the response */
    Schema      progress_schema;      /* TASK: the progress channel */
    uint64_t    generation = 0;   /* changes iff the provider, a schema or an attr changed */
};

/* Peer: a copied snapshot of a discovered peer (safe to keep after the poll). Dropped
 * peers are listed (they may return under the same uuid): gate on `active`. */
struct Peer {
    uint32_t                id = 0;
    std::array<uint8_t, 16> uuid{};          /* the process instance: a restart is a new uuid */
    std::string             name;
    std::string             address;         /* "1.2.3.4:port" */
    bool                    active = false;
    uint64_t                last_heard_us = 0;
    uint32_t                epoch = 0;       /* bumps on every reflected change at this peer */
    bool                    catching_up = false;   /* it advertises newer state than we hold yet */
    uint16_t                fragment_size = 0;
    uint32_t                rtt_us = 0;   /* the smoothed round trip, 0 samples = none yet */
    uint32_t                rtt_jitter_us = 0;
    uint32_t                rtt_min_us = 0;
    uint32_t                rtt_samples = 0;
};

/* One decoded @rant/log line for a Node::on_log handler. The views are valid for the
 * callback only. wall_us is epoch us, mono_us the publisher's monotonic clock, recv_us ours. */
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

/* The RANT_SCHEMA reflection and the typed codec. RANT_SCHEMA specializes rant::reflect<T>,
 * and the codec synthesizes the DSL, compiles it and builds a copy table (docs/cpp.md). */

template <class T> struct reflect;              /* specialized by RANT_SCHEMA */
template <class E> struct reflect_enum;         /* specialized by RANT_ENUM */
template <class U> struct field_tag {};         /* visitor dispatch tag */

/* The capped string wire slot, [u16 live length][N bytes]. assign() refuses over capacity
 * rather than truncating, view() clamps a hostile length. */
template <uint16_t N> struct String {
    uint16_t len = 0;
    char     data[N] = {};

    String() = default;
    template <size_t M> String(const char (&lit)[M]) {
        static_assert(M - 1 <= N, "string literal exceeds rant::String capacity");
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

/* The standard types (docs/stdtypes.md). A wire name rides the schema and narrows matching.
 * Each fixed mirror is identical to the wire, so the memcpy path applies. */

/* The naming hook: name is the wire type name, repr is void for a struct shaped type and the
 * representation type for an alias. Unspecialized means an ordinary anonymous type. */
template <class T> struct std_type { static constexpr const char* name = nullptr; using repr = void; };

/* Apart from the root so Color or Quaternion never clashes with an engine type under a
 * using directive. */
namespace types {

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
/* meters and radians. parent "" = unstated, the cap keeps the packed 88 bytes 8 aligned */
struct Transform { Double3 translation; Quaternion rotation; String<30> parent; };
struct Twist   { Double3 linear, angular; };                    /* m/s, rad/s */
struct GeoPoint{ double lat, lon, alt; };                       /* degrees, degrees, meters */
struct Uuid    { uint8_t bytes[16]; };                          /* RFC 4122 byte order */
struct Matrix3x3 { float m[9];  };                              /* row-major */
struct Matrix4x4 { float m[16]; };                              /* row-major */
struct Uri     { String<256> value; };
/* microseconds since the Unix epoch, UTC, the clock Message::written_us uses */
struct Timestamp { int64_t us = 0; };
struct Duration  { int64_t us = 0; };

/* The video family (docs/stdtypes.md). The enum values are the wire values, and
 * VideoCodec::Unknown is the unstated codec hint. */
enum class ImageFormat : uint8_t { Mono8, Mono16, Rgb8, Rgba8, Bgr8, Yuyv, Nv12, Monof32,
                                   Jpeg = 16, Png = 17 };
enum class VideoCodec : uint8_t { Unknown, Mjpeg, H264, H265, Av1 };
enum class VideoStreamKind : uint8_t { Rtsp, WebrtcWhep, Hls, Srt, Rtp, HttpMjpeg,
                                       Other = 15 };
struct Image {   /* stride = bytes per row, data laid out per format */
    uint32_t width = 0, height = 0, stride = 0;
    ImageFormat format = ImageFormat::Mono8;
    std::vector<uint8_t> data;
};
struct VideoFrame {               /* width/height 0 = unstated (the bitstream rules) */
    VideoCodec codec = VideoCodec::Unknown;
    uint32_t   width = 0, height = 0;
    bool       keyframe = false;
    Timestamp  pts;               /* presentation time, the Timestamp clock */
    std::vector<uint8_t> data;
};
/* Fully fixed, so it works as a latched variable: hand a viewer a URL, not pixels. codec,
 * width and height are hints for pickers, the stream stays authoritative once connected. */
struct ExternalVideoStream {
    VideoStreamKind kind = VideoStreamKind::Rtsp;
    VideoCodec      codec = VideoCodec::Unknown;
    uint32_t        width = 0, height = 0;
    Uri             url;
    String<32>      name;
};
/* NoDistortion rather than None: X11 defines None as a macro. */
enum class DistortionModel : uint8_t { NoDistortion, BrownConrady, Fisheye, Rational };
/* The pinhole model and its lens distortion. coeffs is zero filled past the model's count. */
struct CameraIntrinsics {
    uint32_t width = 0, height = 0;      /* the resolution these numbers are valid for */
    double   fx = 0, fy = 0, cx = 0, cy = 0;
    DistortionModel model = DistortionModel::NoDistortion;
    double   coeffs[8] = { 0 };
};
/* SI: radians or meters, per second, and newtons or newton meters. velocity and effort
 * may be empty. The names ride a JointNames variable, not every sample. */
struct JointState {
    std::vector<double> position, velocity, effort;
};
/* Published once as a variable. The order every JointState array follows. */
struct JointNames {
    std::vector<String<32>> name;
};

inline bool operator==(Timestamp a, Timestamp b) { return a.us == b.us; }
inline bool operator<(Timestamp a, Timestamp b) { return a.us < b.us; }
inline Duration  operator-(Timestamp a, Timestamp b) { return Duration{ a.us - b.us }; }
inline Timestamp operator+(Timestamp t, Duration d) { return Timestamp{ t.us + d.us }; }
inline bool operator==(Duration a, Duration b) { return a.us == b.us; }
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
inline double  length(Double3 a) { return detail::rant_double3_length({ a.x, a.y, a.z }); }
inline Double3 normalize(Double3 a) {
    detail::RantDouble3 n = detail::rant_double3_normalize({ a.x, a.y, a.z });
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
    detail::RantDouble3 r = detail::rant_quaternion_rotate({ q.x, q.y, q.z, q.w }, { v.x, v.y, v.z });
    return { r.x, r.y, r.z };
}
inline Transform identity_transform() {
    return { { 0.0, 0.0, 0.0 }, identity_rotation(), {} };
}
inline bool is_nil(const Uuid& u) {
    for (int i = 0; i < 16; i++) if (u.bytes[i]) return false;
    return true;
}
/* The two values that need a platform (the node runtime provides them). */
inline Timestamp now() { return Timestamp{ detail::rant_timestamp_now() }; }
inline Uuid      new_uuid() { Uuid u; detail::rant_uuid_new((detail::RantUuid*)&u); return u; }

}   /* namespace types */

/* Give a struct-shaped type a wire name: reflect its members, then name it. */
#define RANT_STD_STRUCT(T, ...) \
    template <> struct rant::reflect<rant::types::T> { \
        using is_rant_schema = void; \
        static constexpr const char* type_name = #T; \
        template <class V> static void visit(V&& v) { RANT_STD_MEMBERS_##T(v) } \
    }; \
    template <> struct rant::std_type<rant::types::T> { \
        static constexpr const char* name = #T; using repr = void; }
/* Give an ALIAS-shaped type a wire name: it copies as R (`Uuid = u8[16]`, R = uint8_t[16]). */
#define RANT_STD_ALIAS(T, R) \
    template <> struct rant::std_type<rant::types::T> { \
        static constexpr const char* name = #T; using repr = R; }

#define RANT_STD_F(T, f) v(::rant::field_tag<decltype(::rant::types::T::f)>{}, #f, offsetof(::rant::types::T, f));
#define RANT_STD_MEMBERS_Float2(v)    RANT_STD_F(Float2,x)    RANT_STD_F(Float2,y)
#define RANT_STD_MEMBERS_Float3(v)    RANT_STD_F(Float3,x)    RANT_STD_F(Float3,y)    RANT_STD_F(Float3,z)
#define RANT_STD_MEMBERS_Float4(v)    RANT_STD_F(Float4,x)    RANT_STD_F(Float4,y)    RANT_STD_F(Float4,z)    RANT_STD_F(Float4,w)
#define RANT_STD_MEMBERS_Double2(v) RANT_STD_F(Double2,x) RANT_STD_F(Double2,y)
#define RANT_STD_MEMBERS_Double3(v) RANT_STD_F(Double3,x) RANT_STD_F(Double3,y) RANT_STD_F(Double3,z)
#define RANT_STD_MEMBERS_Double4(v) RANT_STD_F(Double4,x) RANT_STD_F(Double4,y) RANT_STD_F(Double4,z) RANT_STD_F(Double4,w)
#define RANT_STD_MEMBERS_Int2(v)      RANT_STD_F(Int2,x)      RANT_STD_F(Int2,y)
#define RANT_STD_MEMBERS_Int3(v)      RANT_STD_F(Int3,x)      RANT_STD_F(Int3,y)      RANT_STD_F(Int3,z)
#define RANT_STD_MEMBERS_Int4(v)      RANT_STD_F(Int4,x)      RANT_STD_F(Int4,y)      RANT_STD_F(Int4,z)      RANT_STD_F(Int4,w)
#define RANT_STD_MEMBERS_Quaternion(v) RANT_STD_F(Quaternion,x) RANT_STD_F(Quaternion,y) RANT_STD_F(Quaternion,z) RANT_STD_F(Quaternion,w)
#define RANT_STD_MEMBERS_Color(v)     RANT_STD_F(Color,r)     RANT_STD_F(Color,g)     RANT_STD_F(Color,b)     RANT_STD_F(Color,a)
#define RANT_STD_MEMBERS_Rect(v)      RANT_STD_F(Rect,x)      RANT_STD_F(Rect,y)      RANT_STD_F(Rect,w)      RANT_STD_F(Rect,h)
#define RANT_STD_MEMBERS_RectI(v)     RANT_STD_F(RectI,x)     RANT_STD_F(RectI,y)     RANT_STD_F(RectI,w)     RANT_STD_F(RectI,h)
#define RANT_STD_MEMBERS_Transform(v) v(::rant::field_tag<::rant::types::Double3>{}, "translation", offsetof(::rant::types::Transform, translation)); \
                                    v(::rant::field_tag<::rant::types::Quaternion>{}, "rotation", offsetof(::rant::types::Transform, rotation)); \
                                    RANT_STD_F(Transform,parent)
#define RANT_STD_MEMBERS_Twist(v)     v(::rant::field_tag<::rant::types::Double3>{}, "linear", offsetof(::rant::types::Twist, linear)); \
                                    v(::rant::field_tag<::rant::types::Double3>{}, "angular", offsetof(::rant::types::Twist, angular));
#define RANT_STD_MEMBERS_GeoPoint(v) RANT_STD_F(GeoPoint,lat) RANT_STD_F(GeoPoint,lon) RANT_STD_F(GeoPoint,alt)
#define RANT_STD_MEMBERS_Image(v)        RANT_STD_F(Image,width) RANT_STD_F(Image,height) \
                                       RANT_STD_F(Image,stride) RANT_STD_F(Image,format) RANT_STD_F(Image,data)
#define RANT_STD_MEMBERS_VideoFrame(v) RANT_STD_F(VideoFrame,codec) \
                                       RANT_STD_F(VideoFrame,width) RANT_STD_F(VideoFrame,height) \
                                       RANT_STD_F(VideoFrame,keyframe) \
                                       RANT_STD_F(VideoFrame,pts) RANT_STD_F(VideoFrame,data)
#define RANT_STD_MEMBERS_ExternalVideoStream(v) RANT_STD_F(ExternalVideoStream,kind) \
                                       RANT_STD_F(ExternalVideoStream,codec) \
                                       RANT_STD_F(ExternalVideoStream,width) RANT_STD_F(ExternalVideoStream,height) \
                                       RANT_STD_F(ExternalVideoStream,url) RANT_STD_F(ExternalVideoStream,name)
#define RANT_STD_MEMBERS_CameraIntrinsics(v) RANT_STD_F(CameraIntrinsics,width) RANT_STD_F(CameraIntrinsics,height) \
                                       RANT_STD_F(CameraIntrinsics,fx) RANT_STD_F(CameraIntrinsics,fy) \
                                       RANT_STD_F(CameraIntrinsics,cx) RANT_STD_F(CameraIntrinsics,cy) \
                                       RANT_STD_F(CameraIntrinsics,model) RANT_STD_F(CameraIntrinsics,coeffs)
#define RANT_STD_MEMBERS_JointState(v) RANT_STD_F(JointState,position) \
                                       RANT_STD_F(JointState,velocity) RANT_STD_F(JointState,effort)
#define RANT_STD_MEMBERS_JointNames(v) RANT_STD_F(JointNames,name)

namespace priv {

template <class> inline constexpr bool always_false = false;

template <class U> struct is_rant_string : std::false_type {};
template <uint16_t N> struct is_rant_string<String<N>> : std::true_type {
    static constexpr uint16_t cap = N;
};
template <class U> struct is_std_array : std::false_type {};
template <class E, size_t N> struct is_std_array<std::array<E, N>> : std::true_type {
    using elem = E;
    static constexpr size_t count = N;
};
template <class U, class = void> struct is_reflected : std::false_type {};
template <class U> struct is_reflected<U, std::void_t<typename reflect<U>::is_rant_schema>>
    : std::true_type {};
/* an enum type registered with RANT_ENUM (else a plain enum ships as its backing integer) */
template <class U, class = void> struct is_reg_enum : std::false_type {};
template <class U> struct is_reg_enum<U, std::void_t<typename reflect_enum<U>::is_rant_enum>>
    : std::true_type {};
/* std::string: the unbounded string type, a tail frame. Legal as a member and a bare root. */
template <class U> struct is_var_string : std::is_same<U, std::string> {};
/* std::vector<E>: the variable array E[], a tail frame. Legal as a member and a bare root.
 * E must be a wire scalar, and vector<bool> is bit packed so it is refused. */
template <class U> struct is_std_vector : std::false_type {};
template <class E> struct is_std_vector<std::vector<E>> : std::true_type {
    using elem = E;
};

/* map a C++ scalar type onto the wire kind, -1 = not a wire scalar */
template <class U> constexpr int scalar_kind_of() {
    if constexpr (std::is_same_v<U, bool>) return (int)detail::RANT_BOOL;
    else if constexpr (std::is_integral_v<U>) {
        if constexpr (sizeof(U) == 1) return std::is_signed_v<U> ? (int)detail::RANT_I8    : (int)detail::RANT_U8;
        else if constexpr (sizeof(U) == 2) return std::is_signed_v<U> ? (int)detail::RANT_I16 : (int)detail::RANT_U16;
        else if constexpr (sizeof(U) == 4) return std::is_signed_v<U> ? (int)detail::RANT_I32 : (int)detail::RANT_U32;
        else if constexpr (sizeof(U) == 8) return std::is_signed_v<U> ? (int)detail::RANT_I64 : (int)detail::RANT_U64;
        else return -1;
    }
    else if constexpr (std::is_floating_point_v<U>) {
        if constexpr (sizeof(U) == 4) return (int)detail::RANT_F32;
        else if constexpr (sizeof(U) == 8) return (int)detail::RANT_F64;
        else return -1;
    }
    else return -1;
}

inline const char* scalar_kind_name(int k) {
    static const char* names[] = { "u8","u16","u32","u64","i8","i16","i32","i64","f32","f64","bool" };
    return (k >= 0 && k <= (int)detail::RANT_BOOL) ? names[k] : "?";
}
inline uint32_t scalar_wire_size(int k) {
    static const uint8_t sz[] = { 1,2,4,8,1,2,4,8,4,8,1 };
    return (k >= 0 && k <= (int)detail::RANT_BOOL) ? sz[k] : 0;
}
inline bool host_le() {
    const uint16_t probe = 1;
    return *reinterpret_cast<const uint8_t*>(&probe) == 1;
}

/* one leaf field of the flattened struct: where it lives in the struct, where on the
 * wire, and how to copy it. Arrays are one leaf with count > 1 (per-element strides). */
struct Leaf {
    uint32_t    struct_off = 0, wire_off = 0;
    uint8_t     kind = 0;   /* scalar kind, RANT_STR, or the enum backing kind */
    uint8_t     is_array = 0;
    uint8_t     is_enum = 0;   /* wire kind ENUM, copied as its backing integer */
    uint16_t    count = 1, cap = 0;   /* elements, or the string capacity */
    uint32_t    s_stride = 0, w_stride = 0;     /* per-element byte strides */
    std::string path;                           /* dotted path for by-name offset lookup */
    std::vector<uint16_t> elems;                /* its index in each enclosing struct array */
};

/* One variable member, a length framed section on the message tail reached by dotted path
 * through the C accessors. Access is type erased through function pointers. */
struct Tail {
    uint32_t    struct_off = 0;
    uint8_t     is_string = 0;                  /* std::string member (VSTR frame) */
    uint8_t     elem_kind = 0;                  /* std::vector<E>: E's wire kind */
    uint16_t    cap = 0;                        /* std::vector<String<N>>: N */
    std::string path;
    /* std::vector of structs: one element's leaves, offsets within the element, copied
       per element through the array's frame */
    uint8_t     is_structs = 0;
    std::vector<Leaf> elem;
    uint32_t    elem_host = 0;                  /* sizeof the element */
    uint32_t    elem_wire = 0;                  /* its wire size, resolved per schema */
    uint16_t    index = 0;                      /* the array's row, resolved per schema */
    size_t (*count)(const uint8_t* member) = nullptr;
    const uint8_t* (*items)(const uint8_t* member) = nullptr;
    uint8_t* (*resize)(uint8_t* member, size_t n) = nullptr;
    size_t (*extra)(const uint8_t* member) = nullptr;   /* payload bytes to reserve */
    bool (*write)(const uint8_t* member, uint8_t* buf, size_t cap,
                  const detail::RantSchema* s, const char* path) = nullptr;
    bool (*read)(uint8_t* member, detail::RantBytes msg,
                 const detail::RantSchema* s, const char* path) = nullptr;
};

inline void copy_swapped(uint8_t* dst, const uint8_t* src, size_t n);

template <class E> struct vector_tail {
    static size_t extra(const uint8_t* m) {
        return reinterpret_cast<const std::vector<E>*>(m)->size() * sizeof(E);
    }
    static bool write(const uint8_t* m, uint8_t* buf, size_t cap,
                      const detail::RantSchema* s, const char* path) {
        const std::vector<E>& v = *reinterpret_cast<const std::vector<E>*>(m);
        size_t bytes = v.size() * sizeof(E);
        if (host_le() || sizeof(E) == 1)
            return detail::rant_set_array(buf, cap, s, path,
                                          detail::rant_bytes(v.data(), bytes)) != 0;
        std::vector<uint8_t> tmp(bytes);        /* big-endian host: the wire is LE */
        for (size_t i = 0; i < v.size(); i++)
            copy_swapped(tmp.data() + i * sizeof(E),
                         reinterpret_cast<const uint8_t*>(&v[i]), sizeof(E));
        return detail::rant_set_array(buf, cap, s, path,
                                      detail::rant_bytes(tmp.data(), bytes)) != 0;
    }
    static bool read(uint8_t* m, detail::RantBytes msg,
                     const detail::RantSchema* s, const char* path) {
        std::vector<E>& v = *reinterpret_cast<std::vector<E>*>(m);
        detail::RantBytes view = detail::rant_get_array(msg, s, path);
        if (!view.data) return false;   /* empty is non NULL, NULL = mismatch */
        const uint8_t* src = view.data;
        size_t n = view.len / sizeof(E);
        v.resize(n);
        if (host_le() || sizeof(E) == 1) {      /* memcpy: the view may be unaligned */
            if (n) std::memcpy(v.data(), src, n * sizeof(E));
        } else {
            for (size_t i = 0; i < n; i++)
                copy_swapped(reinterpret_cast<uint8_t*>(&v[i]), src + i * sizeof(E), sizeof(E));
        }
        return true;
    }
};

/* a std::vector<String<N>>: [u16 len][N bytes] slots, copied one by one since the
 * struct may carry a pad byte */
template <uint16_t N> struct string_vector_tail {
    static size_t extra(const uint8_t* m) {
        return reinterpret_cast<const std::vector<String<N>>*>(m)->size() * (2u + N);
    }
    static bool write(const uint8_t* m, uint8_t* buf, size_t cap,
                      const detail::RantSchema* s, const char* path) {
        const std::vector<String<N>>& v = *reinterpret_cast<const std::vector<String<N>>*>(m);
        std::vector<uint8_t> tmp(v.size() * (2u + N), 0);
        for (size_t i = 0; i < v.size(); i++) {
            uint8_t* slot = tmp.data() + i * (2u + N);
            if (v[i].len > N) return false;   /* past its slot: refused, never cut */
            uint16_t len = v[i].len;
            slot[0] = (uint8_t)len; slot[1] = (uint8_t)(len >> 8);
            std::memcpy(slot + 2, v[i].data, len);
        }
        return detail::rant_set_array(buf, cap, s, path,
                                      detail::rant_bytes(tmp.data(), tmp.size())) != 0;
    }
    static bool read(uint8_t* m, detail::RantBytes msg,
                     const detail::RantSchema* s, const char* path) {
        std::vector<String<N>>& v = *reinterpret_cast<std::vector<String<N>>*>(m);
        detail::RantBytes view = detail::rant_get_array(msg, s, path);
        if (!view.data) return false;
        size_t n = view.len / (2u + N);
        v.assign(n, String<N>());
        for (size_t i = 0; i < n; i++) {
            const uint8_t* slot = view.data + i * (2u + N);
            uint16_t len = (uint16_t)(slot[0] | (slot[1] << 8));
            if (len > N) len = N;                  /* clamp a hostile length prefix */
            v[i].len = len;
            std::memcpy(v[i].data, slot + 2, len);
        }
        return true;
    }
};

/* the type erased access to a std::vector of structs */
template <class E> struct struct_vector_ops {
    static size_t count(const uint8_t* m) {
        return reinterpret_cast<const std::vector<E>*>(m)->size();
    }
    static const uint8_t* items(const uint8_t* m) {
        return reinterpret_cast<const uint8_t*>(reinterpret_cast<const std::vector<E>*>(m)->data());
    }
    static uint8_t* resize(uint8_t* m, size_t n) {
        std::vector<E>& v = *reinterpret_cast<std::vector<E>*>(m);
        v.assign(n, E{});
        return reinterpret_cast<uint8_t*>(v.data());
    }
};

struct string_tail {
    static size_t extra(const uint8_t* m) {
        return reinterpret_cast<const std::string*>(m)->size();
    }
    static bool write(const uint8_t* m, uint8_t* buf, size_t cap,
                      const detail::RantSchema* s, const char* path) {
        const std::string& v = *reinterpret_cast<const std::string*>(m);
        detail::RantString sv; sv.data = v.data(); sv.len = v.size();
        return detail::rant_set_string(buf, cap, s, path, sv) != 0;
    }
    static bool read(uint8_t* m, detail::RantBytes msg,
                     const detail::RantSchema* s, const char* path) {
        std::string& v = *reinterpret_cast<std::string*>(m);
        detail::RantString sv = detail::rant_get_string(msg, s, path);
        if (!sv.data) return false;
        v.assign(sv.data, sv.len);
        return true;
    }
};

inline std::string strip_namespaces(const char* type_name) {
    std::string n(type_name);
    size_t p = n.rfind(':');
    return p == std::string::npos ? n : n.substr(p + 1);
}

struct SchemaBuilder {
    std::string       dsl;
    std::vector<Leaf> leaves;
    std::vector<Tail> tails;
    uint32_t          base = 0;
    std::string       prefix;
    bool              first = true;
    bool              value_root = false;   /* the schema IS one bare type: no name, no braces */
    int               quiet = 0;            /* > 0: emit leaves only (inside a NAMED type,
                                               whose spelling is just its name) */
    std::vector<uint16_t> elems;            /* the element being walked in each struct array */
    std::string       defs;                 /* nested struct definitions, before the root */
    std::unordered_set<const void*> defined;   /* the types already in defs */
    SchemaBuilder*    root = nullptr;       /* a definition's builder writes defs here */

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
        static_assert(k >= 0, "RANT_SCHEMA: field/element type is not a wire scalar");
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
        l.elems = elems;
        leaves.push_back(std::move(l));
    }
    template <class E> void add_string(const char* name, size_t off, size_t count, bool arr) {
        constexpr uint16_t cap = is_rant_string<E>::cap;
        sep(name);
        put("string<"); put(std::to_string(cap)); put(">");
        if (arr) { put("["); put(std::to_string(count)); put("]"); }
        Leaf l;
        l.struct_off = base + (uint32_t)off;
        l.kind = (uint8_t)detail::RANT_STR; l.is_array = arr ? 1 : 0;
        l.count = (uint16_t)count; l.cap = cap;
        l.s_stride = (uint32_t)sizeof(E);
        l.w_stride = 2u + cap;
        l.path = prefix + name;
        l.elems = elems;
        leaves.push_back(std::move(l));
    }
    /* a RANT_ENUM-registered enum: emit `enum<uN> { A=v, ... }`, copy as the backing int */
    template <class E> void add_enum(const char* name, size_t off) {
        using Backing = std::underlying_type_t<E>;
        constexpr int k = scalar_kind_of<Backing>();
        static_assert(k >= 0 && k <= (int)detail::RANT_I64,
                      "RANT_ENUM: backing must be an integer type (u8..i64)");
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
        l.elems = elems;
        leaves.push_back(std::move(l));
    }
    /* a VARIABLE member: no fixed leaf, a Tail reached by path via the C accessors */
    template <class E> void add_vector(const char* name, size_t off) {
        constexpr int k = scalar_kind_of<E>();
        static_assert(k >= 0, "RANT_SCHEMA: std::vector element must be a wire scalar "
                              "or a rant::String<N> or a struct (no std::string, no vectors of vectors)");
        static_assert(!std::is_same_v<E, bool>,
                      "RANT_SCHEMA: std::vector<bool> is bit-packed; use std::vector<uint8_t>");
        sep(name);
        put(scalar_kind_name(k)); put("[]");
        Tail t;
        t.struct_off = base + (uint32_t)off;
        t.elem_kind  = (uint8_t)k;
        t.path  = prefix + name;
        t.extra = &vector_tail<E>::extra;
        t.write = &vector_tail<E>::write;
        t.read  = &vector_tail<E>::read;
        tails.push_back(std::move(t));
    }
    void add_var_string(const char* name, size_t off) {
        sep(name);
        put("string");
        Tail t;
        t.struct_off = base + (uint32_t)off;
        t.is_string  = 1;
        t.path  = prefix + name;
        t.extra = &string_tail::extra;
        t.write = &string_tail::write;
        t.read  = &string_tail::read;
        tails.push_back(std::move(t));
    }
    template <class U> void add(const char* name, size_t off);
    template <class U> void define();
    template <class U> void members(const char* name, size_t off);
    template <class E> void add_struct_array(const char* name, size_t off, size_t n);
    template <class E> void add_struct_vector(const char* name, size_t off);
    template <class E> void add_string_vector(const char* name, size_t off);
};

/* a struct shaped type: reflected, or a standard struct, never an alias */
template <class U> constexpr bool is_struct_type() {
    if constexpr (std_type<U>::name != nullptr) return std::is_void_v<typename std_type<U>::repr>;
    else return is_reflected<U>::value;
}

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
        if constexpr (is_reg_enum<U>::value) add_enum<U>(name, off);   /* named options */
        else add_scalar<std::underlying_type_t<U>>(name, off, 1, false); /* a plain int */
    } else if constexpr (is_rant_string<U>::value) {
        add_string<U>(name, off, 1, false);
    } else if constexpr (std::is_array_v<U>) {
        using E = std::remove_extent_t<U>;
        constexpr size_t n = std::extent_v<U>;
        if constexpr (is_rant_string<E>::value) add_string<E>(name, off, n, true);
        else if constexpr (is_struct_type<E>()) add_struct_array<E>(name, off, n);
        else                                    add_scalar<E>(name, off, n, true);
    } else if constexpr (is_std_array<U>::value) {
        using E = typename is_std_array<U>::elem;
        constexpr size_t n = is_std_array<U>::count;
        if constexpr (is_rant_string<E>::value) add_string<E>(name, off, n, true);
        else if constexpr (is_struct_type<E>()) add_struct_array<E>(name, off, n);
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
        /* a nested struct spells as its name, defined once above the root as C# and
           Python do, and still contributes its leaves one level deeper */
        sep(name);
        put(strip_namespaces(reflect<U>::type_name));
        define<U>();
        members<U>(name, off);
    } else if constexpr (is_std_vector<U>::value) {
        using E = typename is_std_vector<U>::elem;
        if constexpr (is_struct_type<E>())            add_struct_vector<E>(name, off);
        else if constexpr (is_rant_string<E>::value)  add_string_vector<E>(name, off);
        else                                          add_vector<E>(name, off);
    } else if constexpr (is_var_string<U>::value) {
        add_var_string(name, off);
    } else {
        static_assert(always_false<U>,
            "RANT_SCHEMA: unsupported field type (no pointers/maps; use scalars, "
            "rant::String<N>, fixed arrays, std::vector of scalars, structs or strings, "
            "std::string, or a "
            "nested RANT_SCHEMA struct)");
    }
}

template <class U> struct type_key { static constexpr char id = 0; };

/* U's definition in the root's defs, once per type and after the types it uses */
template <class U> void SchemaBuilder::define() {
    SchemaBuilder& top = root ? *root : *this;
    if (!top.defined.insert(&type_key<U>::id).second) return;
    SchemaBuilder d;
    d.root = &top;
    d.dsl  = strip_namespaces(reflect<U>::type_name);
    d.dsl += " { ";
    reflect<U>::visit(SchemaVisit{ &d });
    d.dsl += " }\n";
    top.defs += d.dsl;
}

/* U's leaves under name, one level deeper, without spelling anything */
template <class U> void SchemaBuilder::members(const char* name, size_t off) {
    uint32_t    saved_base   = base;
    std::string saved_prefix = prefix;
    base   = base + (uint32_t)off;
    prefix = prefix + name + ".";
    ++quiet;
    reflect<U>::visit(SchemaVisit{ this });
    --quiet;
    base   = saved_base;
    prefix = std::move(saved_prefix);
}

/* A fixed array of structs: `name: Elem[n]`, the element defined once above. Every
 * element contributes its own leaves, each carrying its index, so nesting needs nothing more. */
template <class E> void SchemaBuilder::add_struct_array(const char* name, size_t off, size_t n) {
    sep(name);
    if constexpr (std_type<E>::name != nullptr) put(std_type<E>::name);
    else { put(strip_namespaces(reflect<E>::type_name)); define<E>(); }
    put("["); put(std::to_string(n)); put("]");
    for (size_t i = 0; i < n; i++) {
        std::string at = std::string(name) + "[" + std::to_string(i) + "]";
        elems.push_back((uint16_t)i);
        members<E>(at.c_str(), off + i * sizeof(E));
        elems.pop_back();
    }
}

/* A variable array of structs: `name: Elem[]`, a tail whose element leaves sit relative
 * to one element, copied per element through the array's frame. */
template <class E> void SchemaBuilder::add_struct_vector(const char* name, size_t off) {
    sep(name);
    if constexpr (std_type<E>::name != nullptr) put(std_type<E>::name);
    else { put(strip_namespaces(reflect<E>::type_name)); define<E>(); }
    put("[]");
    SchemaBuilder sub;                          /* the element's leaves, base 0 */
    sub.root   = root ? root : this;
    sub.prefix = prefix + name + ".";
    sub.quiet  = 1;
    reflect<E>::visit(SchemaVisit{ &sub });
    Tail t;
    t.struct_off = base + (uint32_t)off;
    t.path       = prefix + name;
    t.is_structs = 1;
    t.elem_kind  = (uint8_t)detail::RANT_STRUCT;
    t.elem       = std::move(sub.leaves);
    t.elem_host  = (uint32_t)sizeof(E);
    t.count  = &struct_vector_ops<E>::count;
    t.items  = &struct_vector_ops<E>::items;
    t.resize = &struct_vector_ops<E>::resize;
    tails.push_back(std::move(t));
}

/* A variable array of capped strings: `name: string<N>[]`. */
template <class E> void SchemaBuilder::add_string_vector(const char* name, size_t off) {
    constexpr uint16_t cap = is_rant_string<E>::cap;
    sep(name);
    put("string<"); put(std::to_string(cap)); put(">[]");
    Tail t;
    t.struct_off = base + (uint32_t)off;
    t.elem_kind  = (uint8_t)detail::RANT_STR;
    t.cap        = cap;
    t.path  = prefix + name;
    t.extra = &string_vector_tail<cap>::extra;
    t.write = &string_vector_tail<cap>::write;
    t.read  = &string_vector_tail<cap>::read;
    tails.push_back(std::move(t));
}

/* the wire offset of field fi reached through the enclosing struct arrays by elems, each
 * fixed, stopping at row stop, a variable array whose frame is the base. False when an
 * index is out of range or another array is variable. */
inline bool static_offset(const detail::RantSchema* s, detail::RantSchemaFieldInfo fi,
                          const std::vector<uint16_t>& elems, uint32_t& out,
                          uint16_t stop = 0xFFFF) {
    uint64_t off = fi.offset;
    size_t k = elems.size();
    if (k + (stop != 0xFFFF ? 1u : 0u) != fi.arr_depth) return false;
    while (fi.arr_parent != 0xFFFF && fi.arr_parent != stop) {
        detail::RantSchemaFieldInfo pa;
        if (!detail::rant_schema_field_at(s, fi.arr_parent, &pa)) return false;
        uint16_t e = elems[--k];
        if (pa.kind != (uint8_t)detail::RANT_ARR || e >= pa.count) return false;
        off += (uint64_t)e * pa.elem_size;
        fi = pa;
    }
    if (off > 0xFFFFFFFFu) return false;
    out = (uint32_t)off;
    return true;
}

/* resolve every leaf's wire offset by dotted path in a compiled schema, verifying the kinds.
 * Used on our own schema at registration and on an incoming one for the rebase decode. */
inline bool fill_offsets(const detail::RantSchema* s, std::vector<Leaf>& lv) {
    for (Leaf& l : lv) {
        int idx = detail::rant_schema_field_index(s, l.path.c_str());
        if (idx < 0) return false;
        detail::RantSchemaFieldInfo fi;
        if (!detail::rant_schema_field_at(s, (uint16_t)idx, &fi)) return false;
        if (l.is_enum) {
            if (fi.kind != (uint8_t)detail::RANT_ENUM || fi.elem != l.kind) return false;
        } else if (l.is_array) {
            if (fi.kind != (uint8_t)detail::RANT_ARR || fi.elem != l.kind || fi.count != l.count) return false;
            if (l.kind == (uint8_t)detail::RANT_STR && fi.str_cap != l.cap) return false;
        } else if (l.kind == (uint8_t)detail::RANT_STR) {
            if (fi.kind != (uint8_t)detail::RANT_STR || fi.str_cap != l.cap) return false;
        } else {
            if (fi.kind != l.kind) return false;
        }
        if (!static_offset(s, fi, l.elems, l.wire_off)) return false;
    }
    return true;
}

/* resolve every tail member in a compiled schema with the matching variable kind. A
 * frame needs no offset, the accessors walk it per message, but a struct vector's element
 * leaves get their offsets within one element, and the element's wire size. */
inline bool tails_resolve(const detail::RantSchema* s, std::vector<Tail>& tv) {
    for (Tail& t : tv) {
        int idx = detail::rant_schema_field_index(s, t.path.c_str());
        if (idx < 0) return false;
        detail::RantSchemaFieldInfo fi;
        if (!detail::rant_schema_field_at(s, (uint16_t)idx, &fi)) return false;
        if (t.is_string) {
            if (fi.kind != (uint8_t)detail::RANT_VSTR) return false;
            continue;
        }
        if (fi.kind != (uint8_t)detail::RANT_VARR || fi.elem != t.elem_kind) return false;
        if (t.elem_kind == (uint8_t)detail::RANT_STR && fi.str_cap != t.cap) return false;
        if (!t.is_structs) continue;
        t.index = (uint16_t)idx;
        t.elem_wire = fi.elem_size;
        for (Leaf& l : t.elem) {
            int li = detail::rant_schema_field_index(s, l.path.c_str());
            detail::RantSchemaFieldInfo lf;
            if (li < 0 || !detail::rant_schema_field_at(s, (uint16_t)li, &lf)) return false;
            if (!static_offset(s, lf, l.elems, l.wire_off, t.index)) return false;
            if ((uint64_t)l.wire_off + (uint64_t)l.w_stride * l.count > t.elem_wire) return false;
        }
    }
    return true;
}

inline void copy_swapped(uint8_t* dst, const uint8_t* src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[n - 1 - i];
}

/* false on a String<N> whose length is past N, which only a direct write of len makes */
inline bool leaf_to_wire(const Leaf& l, const uint8_t* sbase, uint8_t* wire, bool le) {
    const uint8_t* sp = sbase + l.struct_off;
    uint8_t*       wp = wire + l.wire_off;
    for (uint16_t i = 0; i < l.count; i++, sp += l.s_stride, wp += l.w_stride) {
        if (l.kind == (uint8_t)detail::RANT_STR) {
            uint16_t len;
            std::memcpy(&len, sp, 2);
            if (len > l.cap) return false;
            if (le) std::memcpy(wp, &len, 2); else copy_swapped(wp, reinterpret_cast<uint8_t*>(&len), 2);
            std::memcpy(wp + 2, sp + 2, l.cap);
        } else if (le || l.w_stride == 1) {
            std::memcpy(wp, sp, l.w_stride);
        } else {
            copy_swapped(wp, sp, l.w_stride);
        }
    }
    return true;
}
/* the memcpy path copies as is, so it checks its string lengths first */
inline bool strings_fit(const std::vector<Leaf>& leaves, const uint8_t* sbase) {
    for (const Leaf& l : leaves) {
        if (l.kind != (uint8_t)detail::RANT_STR) continue;
        const uint8_t* sp = sbase + l.struct_off;
        for (uint16_t i = 0; i < l.count; i++, sp += l.s_stride) {
            uint16_t len;
            std::memcpy(&len, sp, 2);
            if (len > l.cap) return false;
        }
    }
    return true;
}
inline void leaf_from_wire(const Leaf& l, uint8_t* sbase, const uint8_t* wire, bool le) {
    uint8_t*       sp = sbase + l.struct_off;
    const uint8_t* wp = wire + l.wire_off;
    for (uint16_t i = 0; i < l.count; i++, sp += l.s_stride, wp += l.w_stride) {
        if (l.kind == (uint8_t)detail::RANT_STR) {
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

/* a struct vector into its sized frame, element by element */
inline bool struct_vector_write(const Tail& t, const uint8_t* member, uint8_t* buf, size_t cap,
                                const detail::RantSchema* s, bool le) {
    size_t n = t.count(member);
    if (!detail::rant_set_array_count(buf, cap, s, t.path.c_str(), (uint32_t)n)) return false;
    detail::RantBytes fr = detail::rant_get_array(detail::rant_bytes(buf, cap), s, t.path.c_str());
    if (n && (!fr.data || fr.len < n * t.elem_wire)) return false;
    uint8_t* w = const_cast<uint8_t*>(fr.data);   /* a view into buf, ours to write */
    const uint8_t* src = t.items(member);
    for (size_t i = 0; i < n; i++)
        for (const Leaf& l : t.elem)
            if (!leaf_to_wire(l, src + i * t.elem_host, w + i * t.elem_wire, le)) return false;
    return true;
}

/* a struct vector out of its frame, resized to the live element count */
inline bool struct_vector_read(const Tail& t, uint8_t* member, detail::RantBytes msg,
                               const detail::RantSchema* s, bool le) {
    detail::RantBytes fr = detail::rant_get_array(msg, s, t.path.c_str());
    if (!fr.data || !t.elem_wire) return false;
    size_t n = fr.len / t.elem_wire;
    uint8_t* dst = t.resize(member, n);
    for (size_t i = 0; i < n; i++)
        for (const Leaf& l : t.elem)
            leaf_from_wire(l, dst + i * t.elem_host, fr.data + i * t.elem_wire, le);
    return true;
}

/* T is a bare wire type, usable as a handle's whole schema with no RANT_SCHEMA. std::string
 * is the unbounded string root and std::vector<E> the E[] root, both on the tail path. */
template <class U> constexpr bool is_value_type() {
    return scalar_kind_of<U>() >= 0 || std::is_enum_v<U> || is_rant_string<U>::value
        || is_std_array<U>::value || is_std_vector<U>::value || is_var_string<U>::value;
}

/* the per node, per T registration: schema, copy table and rebase cache, built once per
 * node and freed with it */
struct TypeCodec {
    bool                      ok = false;
    bool                      memcpy_ok = false;      /* our own layout coincides with our wire */
    bool                      memcpy_capable = false; /* trivially copyable + little-endian host */
    bool                      has_strings = false;    /* a String<N> leaf, checked on the memcpy path */
    const detail::RantSchema* raw = nullptr;          /* the node's schema for T */
    uint64_t                  hash = 0;
    uint32_t                  wire_size = 0;   /* the fixed section, all of it when no tails */
    uint32_t                  msg_min = 0;            /* fixed section + one empty frame per tail */
    uint32_t                  struct_size = 0;
    std::vector<Leaf>         leaves;
    std::vector<Tail>         tails;                  /* variable members, declaration order */

    /* one incoming schema pointer's resolved offsets. A rebased schema keeps our hash with the
     * publisher's offsets, so offsets are resolved per pointer and never chosen by hash. */
    struct Rebased { std::vector<Leaf> leaves; std::vector<Tail> tails; uint32_t size = 0;
                     bool ok = false, coincide = false; };
    std::mutex                                       mu;
    std::map<const detail::RantSchema*, Rebased>       rebased;

    Schema schema() const { return Schema(raw); }

    const Rebased* rebased_for(const detail::RantSchema* s) {
        std::lock_guard<std::mutex> g(mu);
        auto it = rebased.find(s);
        if (it != rebased.end()) return &it->second;
        Rebased r;
        r.leaves = leaves;                    /* keep struct offsets, refill wire offsets */
        r.tails  = tails;
        r.ok     = fill_offsets(s, r.leaves) && tails_resolve(s, r.tails);
        r.size   = detail::rant_schema_size(s);
        if (r.ok && memcpy_capable && r.size >= struct_size) {
            bool co = true;
            for (const Leaf& l : r.leaves)
                co = co && l.struct_off == l.wire_off && l.s_stride == l.w_stride;
            r.coincide = co;
        }
        return &rebased.emplace(s, std::move(r)).first->second;
    }
};

template <class T> TypeCodec* build_codec(detail::RantNode* node) {
    static_assert(std_type<T>::name != nullptr || is_reflected<T>::value || is_value_type<T>(),
                  "type has no RANT_SCHEMA(T, fields...) declaration and is not a bare wire type");
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
    } else {
        b.value_root = true;                    /* a bare type: the schema is its spelling alone */
        b.add<T>("", 0);                        /* (std::string / std::vector roots land a Tail) */
    }
    c->leaves = std::move(b.leaves);
    c->tails  = std::move(b.tails);
    b.dsl = b.defs + b.dsl;                     /* nested definitions come first */
    c->raw = detail::rant_node_schema(node, b.dsl.c_str());
    if (!c->raw) return c;
    c->hash        = detail::rant_schema_hash(c->raw);
    c->wire_size   = detail::rant_schema_size(c->raw);
    c->msg_min     = detail::rant_schema_msg_min(c->raw);
    c->struct_size = (uint32_t)sizeof(T);
    if (!fill_offsets(c->raw, c->leaves)) return c;
    if (!tails_resolve(c->raw, c->tails)) return c;
    c->memcpy_capable = std::is_trivially_copyable<T>::value && host_le();
    for (const Leaf& l : c->leaves) c->has_strings = c->has_strings || l.kind == (uint8_t)detail::RANT_STR;
    bool coincide = c->memcpy_capable && c->tails.empty() && sizeof(T) == c->wire_size;
    for (const Leaf& l : c->leaves)
        coincide = coincide && l.struct_off == l.wire_off && l.s_stride == l.w_stride;
    c->memcpy_ok = coincide;
    c->ok = true;
    return c;
}

/* the codec for T in one node, built on first use there. Defined after Node. */
template <class T> TypeCodec* type_codec(Node& n);
inline bool codec_ok(const TypeCodec* c) { return c && c->ok; }

/* the compiled Schema for T in node n. nullopt if the synthesized DSL failed to compile,
 * a codec bug the handle constructors surface */
template <class T> std::optional<Schema> schema_of(Node& n) {
    TypeCodec* c = type_codec<T>(n);
    if (!codec_ok(c)) return std::nullopt;
    return c->schema();
}

/* struct to wire. The memcpy path returns a view of the struct itself, else the field loop
 * packs into scratch, tails in declaration order. Empty Bytes = codec invalid. */
/* struct to wire. Empty when the value does not fit the schema, so a write refuses rather
 * than sending an empty message. */
template <class T> std::optional<Bytes> encode(TypeCodec& c, const T& v, std::vector<uint8_t>& scratch) {
    if (!c.ok) return std::nullopt;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(&v);
    if (c.memcpy_ok) {
        if (c.has_strings && !strings_fit(c.leaves, base)) return std::nullopt;
        return Bytes(&v, sizeof(T));
    }
    const bool le = host_le();
    if (c.tails.empty()) {
        scratch.assign(c.wire_size, 0);
        for (const Leaf& l : c.leaves)
            if (!leaf_to_wire(l, base, scratch.data(), le)) return std::nullopt;
        return Bytes(scratch.data(), scratch.size());
    }
    size_t need = c.msg_min;
    for (const Tail& t : c.tails)
        need += t.is_structs ? t.count(base + t.struct_off) * t.elem_wire
                             : t.extra(base + t.struct_off);
    scratch.assign(need, 0);
    if (!detail::rant_schema_message_default(c.raw, scratch.data(), scratch.size()))
        return std::nullopt;
    for (const Leaf& l : c.leaves)
        if (!leaf_to_wire(l, base, scratch.data(), le)) return std::nullopt;
    for (const Tail& t : c.tails) {
        bool ok = t.is_structs
            ? struct_vector_write(t, base + t.struct_off, scratch.data(), scratch.size(), c.raw, le)
            : t.write(base + t.struct_off, scratch.data(), scratch.size(), c.raw, t.path.c_str());
        if (!ok) return std::nullopt;
    }
    return Bytes(scratch.data(),
                 detail::rant_schema_msg_len(c.raw, scratch.data(), scratch.size()));
}

/* wire to struct. Offsets always come from the delivered schema and are cached per schema
 * pointer, with the memcpy path when that layout matches. nullptr assumes our own layout. */
template <class T> bool decode(TypeCodec& c, T& out, Bytes data, const detail::RantSchema* schema) {
    if (!c.ok) return false;
    const detail::RantSchema* sch = c.raw;
    const std::vector<Leaf>* lv = &c.leaves;
    const std::vector<Tail>* tv = &c.tails;
    uint32_t need = c.wire_size;
    bool fast = c.memcpy_ok;
    if (schema && schema != c.raw) {
        const TypeCodec::Rebased* rb = c.rebased_for(schema);
        if (!rb->ok) return false;
        lv   = &rb->leaves;
        tv   = &rb->tails;
        need = rb->size;
        fast = rb->coincide;
        sch  = schema;
    }
    if constexpr (std::is_trivially_copyable<T>::value) {   /* a tail type is never fast */
        if (fast) {
            if (data.size() < sizeof(T)) return false;
            std::memcpy(&out, data.data(), sizeof(T));
            return true;
        }
    } else {
        (void)fast;
    }
    if (data.size() < need) return false;
    const bool le = host_le();
    uint8_t* base = reinterpret_cast<uint8_t*>(&out);
    for (const Leaf& l : *lv) leaf_from_wire(l, base, data.data(), le);
    if (!tv->empty()) {                         /* frames walked per message, by path */
        detail::RantBytes mb; mb.data = data.data(); mb.len = data.size();
        for (const Tail& t : *tv) {
            bool ok = t.is_structs ? struct_vector_read(t, base + t.struct_off, mb, sch, le)
                                   : t.read(base + t.struct_off, mb, sch, t.path.c_str());
            if (!ok) return false;
        }
    }
    return true;
}

}   /* namespace priv */

namespace priv {

using MessageHandler = std::function<void(const MessageView&)>;
using EventHandler   = std::function<void(const Event&)>;
using LogHandler     = std::function<void(const LogLine&)>;

/* One name's topic slot and how many live handles hold each side of it. */
struct TopicRec {
    detail::RantTopic* ch; uint8_t bits; uint64_t schema_hash; detail::RantQueue* queue; bool pull;
    int pubs = 0, subs = 0;
};
/* A subscriber handler and the id its handle removes it by, 0 for the node's own. */
struct SubHandler { uint64_t id; MessageHandler fn; };

/* The state behind a Node. Handles share it, so one that outlives its node reads
 * node == nullptr instead of freed memory. */
struct NodeImpl {
    detail::RantNode*          node = nullptr;
    Threading                  threading = Threading::ServiceThread;
    detail::RantQueue*         queue = nullptr;   /* the node queue under Dispatch */
    /* read on the loop thread, swapped whole under reg_mu */
    std::shared_ptr<const EventHandler> on_event;
    std::shared_ptr<const LogHandler>   on_log;
    bool                       log_bound = false;   /* the level topics hold our handler, under create_mu */
    std::string                disc_group;
    std::string                mcast_if;
    std::string                self_ip;
    std::vector<detail::RantAddr> seeds;

    /* wrapper registries. create_mu serializes wrapper side creates and closes. reg_mu is a
     * leaf lock, never call into C while holding it. */
    std::mutex               create_mu;
    std::mutex               reg_mu;
    std::map<std::string, TopicRec, std::less<>> topics;   /* name to shared slot */
    std::unordered_map<uint16_t, std::shared_ptr<const std::vector<SubHandler>>> sub_handlers;
    uint64_t                 next_handler_id = 1;
    std::vector<std::unique_ptr<HandlerBox>> boxes;  /* pattern handler boxes */
#ifndef RANT_NO_PATTERNS
    std::unordered_set<void*> async_live;            /* outstanding AsyncBox* */
#endif
    /* the typed codecs, one per T, keyed by a per T address. codec_mu is held while a
     * codec compiles through the node, never inside a callback */
    std::mutex codec_mu;
    std::map<const void*, std::unique_ptr<TypeCodec>> codecs;

    /* a handle's queue: the named one, else the node queue under Dispatch, else inline */
    detail::RantQueue* queue_for(const Queue* q) const { return q ? q->raw() : queue; }

    /* the app's event handler, else the default that prints errors to stderr */
    void emit(const Event& ev) {
        std::shared_ptr<const EventHandler> h;
        {
            std::lock_guard<std::mutex> g(reg_mu);
            h = on_event;
        }
        if (!h) {
            if (ev.is_error()) std::fprintf(stderr, "rant: %s\n", ev.to_string().c_str());
            return;
        }
#if defined(__cpp_exceptions)
        /* the event handler is where a throw would be reported, so it prints instead */
        try { (*h)(ev); }
        catch (const std::exception& x) { std::fprintf(stderr, "rant: the event handler threw: %s\n", x.what()); }
        catch (...) { std::fprintf(stderr, "rant: the event handler threw\n"); }
#else
        (*h)(ev);
#endif
    }
    void emit_error(detail::RantErrorKind kind, const char* topic_name, const char* detail = nullptr) {
        detail::RantEvent e;
        std::memset(&e, 0, sizeof e);
        e.kind = detail::RANT_ERROR;
        e.error = kind;
        e.topic_name = topic_name;
        e.schema_detail = detail;
        e.user = this;
        emit(Event(&e));
    }
    /* an app callback threw: the text on stderr and an Error event of kind None, never an
     * unwind into the C loop */
    void report_throw(const char* what) {
        std::fprintf(stderr, "rant: a handler threw: %s\n", what);
        emit_error(detail::RANT_E_NONE, nullptr);
    }
    /* a payload that did not decode into the handle's type: an Error event, never a silent drop */
    void report_decode(const std::string& name, const char* what) {
        emit_error(detail::RANT_E_SCHEMA_MISMATCH, name.empty() ? nullptr : name.c_str(), what);
    }
    /* a handle destroyed inside its own inline callback: its entity stays until close */
    void report_close_refused(const std::string& name) {
        emit_error(detail::RANT_E_STATE, name.empty() ? nullptr : name.c_str());
    }

    /* Close the C node once: stop the loop, leave with a bye, reap the async calls. */
    void shutdown();
    ~NodeImpl() { shutdown(); }
};

/* The name slot under Publisher<Bytes> and Subscriber<Bytes>: one C topic per name, its
 * role following the live handles. Each handle holds one side, and the last close retires
 * the topic. */
class TopicCore {
public:
    TopicCore() = default;
    /* Create (or share) a topic on `node`. Throws rant::Error on failure (with
     * -fno-exceptions: check valid()). schema is copied into the node. */
    TopicCore(Node& node, std::string_view name, Role role, const Schema* schema, const Qos& qos)
        : TopicCore(node, name, role, schema, qos, false) {}
    /* A pulled subscribe side: no callbacks, messages wait for take(). */
    static TopicCore pulled(Node& node, std::string_view name, const Schema* schema, const Qos& qos) {
        return TopicCore(node, name, Role::SubOnly, schema, qos, true);
    }
    TopicCore(TopicCore&& o) noexcept { move_from(o); }
    TopicCore& operator=(TopicCore&& o) noexcept {
        if (this != &o) { (void)close(); move_from(o); }
        return *this;
    }
    TopicCore(const TopicCore&) = delete;
    TopicCore& operator=(const TopicCore&) = delete;
    ~TopicCore() {
        if (close() == SendStatus::State && impl_) impl_->report_close_refused(name());
    }

    /* The oldest waiting message of a pulled topic, or with latest the newest. 1 got one,
     * 0 none, negative the C refusal. The view lives until the next take. */
    int take(detail::RantMsg* out, int timeout_ms, bool latest) {
        detail::RantTopic* c = live();
        if (!c) return static_cast<int>(SendStatus::NoTopic);
        return latest ? detail::rant_topic_take_latest(c, out, timeout_ms)
                      : detail::rant_topic_take(c, out, timeout_ms);
    }

    bool valid() const noexcept { return live() != nullptr; }
    /* A reflect_from_mesh topic: re read the mesh and re type in place if what it took has
     * moved. true = re typed, false = current or not a reflect handle. */
    bool refresh() { detail::RantTopic* c = live(); return c && detail::rant_topic_refresh(c) == 1; }
    explicit operator bool() const noexcept { return valid(); }

    /* capture is when the data was true, as against when it was sent. The default is
     * unstated, which costs no wire bytes. */
    SendStatus send(Bytes data, types::Timestamp capture = {}) {
        detail::RantSendOpts o;
        detail::RantTopic* c = live();
        if (!c) return SendStatus::NoTopic;
        o.capture_us = static_cast<uint64_t>(capture.us);
        return static_cast<SendStatus>(
            detail::rant_topic_send(c, detail::rant_bytes(data.data(), data.size()), &o));
    }
    /* Drop this handle's side of the name. The last handle on a name retires the topic, so
     * the name can be created again with another schema. State from the topic's own inline
     * callback, and the handle stays valid then. */
    SendStatus close();
    /* Run h for every message on this topic until this handle closes. */
    void add_handler(MessageHandler h);
    /* The schema the topic uses now, empty when untyped. A reflect_from_mesh topic reports
     * what it adopted. */
    Schema schema() const {
        detail::RantTopic* c = live();
        return Schema(c ? detail::rant_topic_schema(c) : nullptr);
    }
    /* The topic's name, empty once closed. */
    std::string name() const;
    uint16_t index() const {
        detail::RantTopic* c = live();
        return c ? detail::rant_topic_index(c) : 0xffff;
    }
    int match_count() const {
        detail::RantTopic* c = live();
        return c ? detail::rant_topic_match_count(c) : 0;
    }
    /* 1 when a send would not wait: a subscriber is matched, or matching has converged
     * so there is nobody to wait for. The async form of the send-path match wait. */
    bool ready() const {
        detail::RantTopic* c = live();
        return c && detail::rant_topic_ready(c) == 1;
    }
    /* Pump until every reader has acked, or timeout elapses. Call before
     * closing so a final burst is not cut off by the BYE. */
    bool drain(std::chrono::milliseconds timeout) {
        detail::RantTopic* c = live();
        return !c || detail::rant_topic_drain(c, priv::to_ms(timeout)) == 1;
    }

    /* Cumulative traffic counters (always on): messages/bytes this node committed to the
     * topic (tx) and delivered from it (rx). Also carried in the @rant/meta snapshot. */
    struct Counts { uint64_t tx_msgs = 0, tx_bytes = 0, rx_msgs = 0, rx_bytes = 0; };
    Counts counts() const {
        Counts c;
        if (detail::RantTopic* t = live())
            detail::rant_topic_counts(t, &c.tx_msgs, &c.tx_bytes, &c.rx_msgs, &c.rx_bytes);
        return c;
    }

private:
    TopicCore(Node& node, std::string_view name, Role role, const Schema* schema, const Qos& qos,
              bool pull);
    /* the C topic while the node lives, else null */
    detail::RantTopic* live() const noexcept { return impl_ && impl_->node ? ch_ : nullptr; }
    void move_from(TopicCore& o) noexcept {
        ch_ = o.ch_; impl_ = std::move(o.impl_); bits_ = o.bits_; handler_id_ = o.handler_id_;
        o.ch_ = nullptr; o.bits_ = 0; o.handler_id_ = 0;
    }

    detail::RantTopic*        ch_ = nullptr;
    std::shared_ptr<NodeImpl> impl_;
    uint8_t                   bits_ = 0;         /* the side this handle holds: pub 1, sub 2 */
    uint64_t                  handler_id_ = 0;   /* this handle's subscriber handler, 0 = none */
    friend class ::rant::Node;
};

}   /* namespace priv */

#ifndef RANT_NO_PATTERNS

/* A parked function reply from Request::defer. Movable and single shot, completed from any
 * thread. Dropping it leaves the caller to its timeout. */
template <> class Deferred<Bytes> {
public:
    Deferred() = default;
    Deferred(Deferred&& o) noexcept : fn_(o.fn_), token_(o.token_), impl_(std::move(o.impl_)) {
        o.fn_ = nullptr; o.token_ = 0;
    }
    Deferred& operator=(Deferred&& o) noexcept {
        if (this != &o) {
            fn_ = o.fn_; token_ = o.token_; impl_ = std::move(o.impl_);
            o.fn_ = nullptr; o.token_ = 0;
        }
        return *this;
    }
    Deferred(const Deferred&) = delete;
    Deferred& operator=(const Deferred&) = delete;

    /* false once completed, and once the node closed */
    bool valid() const noexcept { return fn_ != nullptr && token_ != 0 && impl_ && impl_->node; }
    explicit operator bool() const noexcept { return valid(); }

    /* message: optional outcome text, ResponseView::message on the caller, truncated at
     * RANT_CALL_MSG_MAX. On fail it is what a generic consumer displays. */
    bool complete(Bytes rsp = Bytes(), std::string_view message = {}) {
        return finish(detail::RANT_CALL_OK, message, rsp);
    }
    bool fail(std::string_view message = {}, Bytes rsp = Bytes()) {
        return finish(detail::RANT_CALL_APP_ERROR, message, rsp);
    }

private:
    Deferred(detail::RantFunction* fn, uint64_t token, std::shared_ptr<priv::NodeImpl> impl)
        : fn_(fn), token_(token), impl_(std::move(impl)) {}
    bool finish(int status, std::string_view message, Bytes rsp) {
        if (!valid()) return false;
        std::string m(message);   /* the C API takes a NUL-terminated string */
        int r = detail::rant_function_complete(fn_, token_,
                    static_cast<detail::RantCallStatus>(status),
                    m.empty() ? nullptr : m.c_str(), priv::to_c(rsp));
        fn_ = nullptr; token_ = 0;
        return r == 0;
    }
    detail::RantFunction* fn_ = nullptr;
    uint64_t                token_ = 0;
    std::shared_ptr<priv::NodeImpl> impl_;
    template <class R> friend class Request;
};

/* The untyped request seen by a function handler, valid for the callback only. Reply once,
 * or defer() and complete later. Returning without a reply acknowledges Ok empty. */
template <> class Request<Bytes> : public FieldView {
public:
    std::string_view function_name() const { return { rq_->function_name.data, rq_->function_name.len }; }
    uint32_t         caller()        const { return rq_->caller; }
    std::string_view caller_name()   const { return { rq_->caller_name.data, rq_->caller_name.len }; }
    uint64_t         recv_us()       const { return rq_->recv_us; }
    uint64_t         written_us()       const { return rq_->written_us; }   /* the caller's stamp */

    void reply(Bytes rsp)       { detail::rant_request_reply(rq_, priv::to_c(rsp)); }
    /* message: the failure text, ResponseView::message on the caller, truncated at
     * RANT_CALL_MSG_MAX. Empty = the default "app error". rsp may carry data beside it. */
    void fail(std::string_view message = {}, Bytes rsp = {}) {
        std::string m(message);
        detail::rant_request_fail(rq_, m.empty() ? nullptr : m.c_str(), priv::to_c(rsp));
    }
    /* Park the reply and return now. The Deferred completes the call later from any thread. */
    Deferred<Bytes> defer() { return Deferred<Bytes>(fn_, detail::rant_request_defer(rq_), impl_); }

private:
    Request(detail::RantRequest* rq, detail::RantFunction* fn, std::shared_ptr<priv::NodeImpl> impl)
        : FieldView(rq->data, rq->schema), rq_(rq), fn_(fn), impl_(std::move(impl)) {}
    detail::RantRequest*    rq_;
    detail::RantFunction* fn_;
    std::shared_ptr<priv::NodeImpl> impl_;
    template <class A, class B> friend class FunctionDefinition;
};

/* A deferred task call in flight from TaskRequest::defer, movable into any thread the app
 * owns. The verb contract is in docs/cpp.md and docs/tasks.md. */
template <> class PendingTask<Bytes, Bytes> {
public:
    PendingTask() = default;
    PendingTask(PendingTask&& o) noexcept : fn_(o.fn_), token_(o.token_), impl_(std::move(o.impl_)) {
        o.fn_ = nullptr; o.token_ = 0;
    }
    PendingTask& operator=(PendingTask&& o) noexcept {
        if (this != &o) {
            fn_ = o.fn_; token_ = o.token_; impl_ = std::move(o.impl_);
            o.fn_ = nullptr; o.token_ = 0;
        }
        return *this;
    }
    PendingTask(const PendingTask&) = delete;
    PendingTask& operator=(const PendingTask&) = delete;

    /* false once completed, and once the node closed */
    bool valid() const noexcept { return fn_ != nullptr && token_ != 0 && impl_ && impl_->node; }
    explicit operator bool() const noexcept { return valid(); }

    /* one progress update, broadcast on the progress channel (any observer may watch) */
    SendStatus progress(Bytes update) {
        if (!valid()) return SendStatus::State;
        return static_cast<SendStatus>(detail::rant_function_progress(fn_, token_, priv::to_c(update)));
    }
    /* true the moment a cancel for this call arrived (false on an emptied handle) */
    bool cancelled() const {
        return valid() && detail::rant_function_cancelled(fn_, token_) == 1;
    }
    /* message as on Deferred: optional outcome text (ResponseView::message on the caller) */
    SendStatus complete(Bytes rsp = Bytes(), std::string_view message = {}) {
        return finish(detail::RANT_CALL_OK, message, rsp);
    }
    SendStatus fail(std::string_view message = {}, Bytes rsp = Bytes()) {
        return finish(detail::RANT_CALL_APP_ERROR, message, rsp);
    }
    /* honor a cancel: the caller's terminal status is Cancelled. rsp may carry a partial
     * result, which a function cannot express. */
    SendStatus complete_cancelled(std::string_view message = {}, Bytes rsp = Bytes()) {
        return finish(detail::RANT_CALL_CANCELLED, message, rsp);
    }

private:
    PendingTask(detail::RantFunction* fn, uint64_t token, std::shared_ptr<priv::NodeImpl> impl)
        : fn_(fn), token_(token), impl_(std::move(impl)) {}
    SendStatus finish(int status, std::string_view message, Bytes rsp) {
        if (!valid()) return SendStatus::State;
        std::string m(message);   /* the C API takes a NUL-terminated string */
        int r = detail::rant_function_complete(fn_, token_,
                    static_cast<detail::RantCallStatus>(status),
                    m.empty() ? nullptr : m.c_str(), priv::to_c(rsp));
        fn_ = nullptr; token_ = 0;
        return static_cast<SendStatus>(r);
    }
    detail::RantFunction* fn_ = nullptr;
    uint64_t                token_ = 0;
    std::shared_ptr<priv::NodeImpl> impl_;
    template <class A, class B> friend class TaskRequest;
};

/* The untyped request seen by a task handler: Request<Bytes> plus the task verbs. Answer inline,
 * or start() and defer() and return. Returning with nothing answers AppError. */
template <> class TaskRequest<Bytes, Bytes> : public FieldView {
public:
    std::string_view task_name()   const { return { rq_->function_name.data, rq_->function_name.len }; }
    uint32_t         caller()      const { return rq_->caller; }
    std::string_view caller_name() const { return { rq_->caller_name.data, rq_->caller_name.len }; }
    uint64_t         recv_us()     const { return rq_->recv_us; }
    uint64_t         written_us()  const { return rq_->written_us; }   /* the caller's stamp */

    void reply(Bytes rsp) { detail::rant_request_reply(rq_, priv::to_c(rsp)); }
    void fail(std::string_view message = {}, Bytes rsp = {}) {
        std::string m(message);
        detail::rant_request_fail(rq_, m.empty() ? nullptr : m.c_str(), priv::to_c(rsp));
    }
    /* send RUNNING to the caller now, idempotent. defer() implies it */
    SendStatus start() { return static_cast<SendStatus>(detail::rant_request_start(rq_)); }
    /* park the call and return now: the returned PendingTask carries it to completion */
    PendingTask<Bytes, Bytes> defer() {
        return PendingTask<Bytes, Bytes>(fn_, detail::rant_request_defer(rq_), impl_);
    }

private:
    TaskRequest(detail::RantRequest* rq, detail::RantFunction* fn, std::shared_ptr<priv::NodeImpl> impl)
        : FieldView(rq->data, rq->schema), rq_(rq), fn_(fn), impl_(std::move(impl)) {}
    detail::RantRequest*    rq_;
    detail::RantFunction* fn_;
    std::shared_ptr<priv::NodeImpl> impl_;
    template <class A, class B, class C> friend class TaskDefinition;
};

/* An owning function call outcome from RemoteFunction<Bytes, Bytes>::call. send_status() carries a
 * synchronous refusal when the call never launched. */
template <> class Response<Bytes> {
public:
    Response() = default;
    CallStatus status()      const { return st_; }
    bool       ok()          const { return st_ == CallStatus::Ok; }
    explicit operator bool() const { return ok(); }
    uint32_t   provider()    const { return provider_; }
    SendStatus send_status() const { return ss_; }
    uint64_t   written_us()     const { return written_; }   /* provider stamp, 0 = synthesized */
    Bytes      data()        const { return { data_.data(), data_.size() }; }
    /* the outcome text, owned: the provider's message or the default status text on any
     * non OK outcome. Empty on OK with no message and on a synchronous send refusal. */
    std::string_view message() const { return message_; }
    /* the schema data decodes with (interned in the node, valid until node close) */
    const detail::RantSchema* raw_schema() const { return schema_; }

private:
    CallStatus                st_ = CallStatus::Timeout;
    SendStatus                ss_ = SendStatus::Ok;
    uint32_t                  provider_ = 0;
    uint64_t                  written_ = 0;
    std::vector<uint8_t>      data_;
    std::string               message_;
    const detail::RantSchema* schema_ = nullptr;
    template <class A, class B> friend class RemoteFunction;
    template <class A, class B, class C> friend class RemoteTask;
};

/* ResponseView<Bytes> (untyped): the async outcome, valid for the callback only. */
template <> class ResponseView<Bytes> : public FieldView {
public:
    CallStatus status()   const { return static_cast<CallStatus>(r_->status); }
    bool       ok()       const { return r_->status == detail::RANT_CALL_OK; }
    uint32_t   provider() const { return r_->provider; }
    uint64_t   written_us()  const { return r_->written_us; }   /* the provider's write stamp */
    /* the outcome text as a view for the callback: the provider's message, else the default
     * status text. Empty only on OK with no message. */
    std::string_view message() const { return { r_->message.data, r_->message.len }; }

private:
    explicit ResponseView(const detail::RantResponse* r)
        : FieldView(r->data, r->schema), r_(r) {}
    const detail::RantResponse* r_;
    template <class A, class B> friend class RemoteFunction;
    template <class A, class B, class C> friend class RemoteTask;
};

/* One progress update for a task caller's on_progress, valid for the callback only. The
 * RUNNING ack fires it once with has_value() false, and field reads need has_value(). */
template <> class ProgressView<Bytes> : public FieldView {
public:
    uint32_t call_id()    const { return p_->call_id; }
    uint32_t provider()   const { return p_->provider; }   /* the peer working the call */
    uint64_t written_us() const { return p_->written_us; } /* the provider's write stamp */
    uint64_t recv_us()    const { return p_->recv_us; }
    bool     has_value()  const { return p_->data.len != 0; }

private:
    explicit ProgressView(const detail::RantProgress* p) : FieldView(p->data, p->schema), p_(p) {}
    const detail::RantProgress* p_;
    template <class A, class B, class C> friend class RemoteTask;
};

namespace priv {
/* one in flight async call's callbacks, owned by the registry until the one terminal
 * outcome fires. Task progress never frees it. */
struct AsyncBox {
    std::function<void(const ResponseView<Bytes>&)> cb;
    std::mutex*                mu;     /* the node Impl's registry lock */
    std::unordered_set<void*>* live;   /* the node Impl's outstanding-box set */
    std::function<void(const ProgressView<Bytes>&)> on_progress;   /* task calls only */
    NodeImpl*                  impl;   /* reports a callback's throw */
};
}

/* Section-mask bits for a @rant/meta request: OR them into the sections
 * argument of Reflection::meta and meta_async, 0 = every section. */
enum MetaSection : uint32_t {
    MetaNode   = 0x1u,   /* uptime, memory, backpressure, peer/topic counts, last error */
    MetaProc   = 0x2u,   /* per-process cpu/rss (absent where the platform can't measure) */
    MetaTopics = 0x4u,   /* per-topic array: names, roles, match counts, traffic counters */
    MetaPeers  = 0x8u    /* per-peer array: id, name, address, match counts */
};

/* A decoded @rant/meta reply, owned so it outlives the callback. The node and proc scalars
 * are pulled out, the full body stays in info as a MapDict. Absent sections leave zeros. */
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
                               Bytes data, const detail::RantSchema* schema) {
        MetaSnapshot s;
        s.status = st; s.provider = provider;
        if (st != CallStatus::Ok || !schema) return s;
        detail::RantBytes info = detail::rant_get_map(detail::rant_bytes(data.data(), data.size()),
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
            s.node.uptime_us      = u("uptimeUs");
            s.node.wall_us        = u("wallUs");
            s.node.mem_in_use     = u("memInUse");
            s.node.mem_peak       = u("memPeak");
            s.node.alloc_calls    = u("allocCalls");
            s.node.evicted_unsent = u("evictedUnsent");
            s.node.bp_waited_us   = u("bpWaitedUs");
            s.node.bp_waits       = u("bpWaits");
            s.node.peers          = u("peers");
            s.node.max_peers      = u("maxPeers");
            s.node.topics         = u("topics");
            s.node.max_topics     = u("maxTopics");
            s.node.shm_tx         = u("shmTx");
            s.node.shm_rx         = u("shmRx");
            s.node.last_error     = u("lastError");
            s.node.last_error_text= str("lastErrorText");
        }
        it = s.info.find("proc");
        if (it != s.info.end() && it->second.is_map()) {
            const MapDict& m = it->second.as_map();
            auto u = [&](const char* k){ auto j = m.find(k); return j == m.end() ? uint64_t(0) : j->second.as_uint(); };
            s.proc.have     = true;
            s.proc.have_cpu = m.find("cpuUs") != m.end();
            s.proc.pid      = u("pid");
            s.proc.cpu_us   = u("cpuUs");
            s.proc.rss      = u("rss");
            s.proc.peak_rss = u("peakRss");
            s.proc.heap_total = u("heapTotal"); s.proc.heap_free = u("heapFree");
            s.proc.heap_min_free = u("heapMinFree");
            s.proc.heap_largest_free_block = u("heapLargestFreeBlock");
        }
        return s;
    }
    static MetaSnapshot decode(const Response<Bytes>& r) {
        return decode(r.status(), r.provider(), r.data(), r.raw_schema());
    }
    static MetaSnapshot decode(const ResponseView<Bytes>& r) {
        return decode(r.status(), r.provider(), r.data(), r.raw_schema());
    }
};

#endif /* !RANT_NO_PATTERNS */

namespace priv {
/* Run an app callback from a C trampoline: a throw is reported, never unwound into C. */
template <class F> void guarded(NodeImpl* impl, F&& f) {
#if defined(__cpp_exceptions)
    try { f(); }
    catch (const std::exception& e) { if (impl) impl->report_throw(e.what()); }
    catch (...) { if (impl) impl->report_throw("an exception not derived from std::exception"); }
#else
    (void)impl;
    f();
#endif
}
/* A request handler's throw answers the call AppError with the exception text. A no-op
 * if the handler already answered or deferred, one answer per call. */
template <class F> void answered(detail::RantRequest* rq, F&& f) {
#if defined(__cpp_exceptions)
    try { f(); }
    catch (const std::exception& e) { detail::rant_request_fail(rq, e.what(), detail::rant_bytes(nullptr, 0)); }
    catch (...) {
        detail::rant_request_fail(rq, "the handler threw an exception not derived from std::exception",
                                  detail::rant_bytes(nullptr, 0));
    }
#else
    (void)rq;
    f();
#endif
}
/* A payload that came back, decoded into T, empty when none did. A failure is reported. */
template <class T>
std::optional<T> decode_payload(TypeCodec& c, Bytes data, const detail::RantSchema* s,
                                NodeImpl* impl, const std::string& name, const char* what) {
    if (data.size() == 0) return std::nullopt;
    T v{};
    if (!decode(c, v, data, s)) {
        if (impl) impl->report_decode(name, what);
        return std::nullopt;
    }
    return v;
}
}   /* namespace priv */

inline void priv::NodeImpl::shutdown() {
    if (!node) return;
    detail::rant_node_close(node, /*send_bye=*/1);
    node = nullptr;
#ifndef RANT_NO_PATTERNS
    /* the C never fires pending async callbacks at close, so reap the boxes */
    for (void* b : async_live) delete static_cast<AsyncBox*>(b);
    async_live.clear();
#endif
}

namespace priv {
/* A subscriber handler H for T: the typed forms, or the view form for rant::Bytes. */
template <class T, class H> struct is_message_handler : std::integral_constant<bool,
    std::is_same_v<T, Bytes>
        ? std::is_invocable_v<std::decay_t<H>&, const MessageView&>
        : (std::is_invocable_v<std::decay_t<H>&, const T&> ||
           std::is_invocable_v<std::decay_t<H>&, const T&, const MessageView&>)> {};
/* A typed handle takes its schema from its type: refuse one given, and reflect_from_mesh. */
inline bool typed_takes_no_schema(bool has_schema, bool reflect, const char* what) {
    if (!has_schema && !reflect) return true;
    raise_msg(what);
    return false;
}
inline const Schema* schema_ptr(const Schema& s) { return s ? &s : nullptr; }
}   /* namespace priv */

/* Owns the RantNode, its memory and the user callbacks. Every call is serialized by the C
 * node lock. The threading and callback rules are in docs/cpp.md and docs/node.md. */
class Node {
public:
    using MessageHandler = priv::MessageHandler;
    using EventHandler   = priv::EventHandler;

    /* Open a node and join the mesh. An empty name is auto generated. The service thread
     * runs from here unless o.threading is Manual, and on_event may attach after. Throws
     * rant::Error, or under -fno-exceptions check valid(). */
    explicit Node(std::string_view name = {}, const NodeOptions& o = {}) {
        std::shared_ptr<Impl> impl = std::make_shared<Impl>();
        impl->threading = o.threading;
        /* The node retains the net string/seed pointers, so own that storage. */
        impl->disc_group = o.discovery_group;
        impl->mcast_if   = o.multicast_interface;
        impl->self_ip    = o.self_ip;
        for (const std::string& s : o.seed_peers) {
            detail::RantAddr a;
            if (parse_addr(s, a)) impl->seeds.push_back(a);
        }
        std::string nm(name);

        detail::RantNodeOpts co;
        std::memset(&co, 0, sizeof co);
        co.domain        = o.domain;
        co.max_topics    = o.max_topics;
        co.disable_shm   = o.disable_shm ? 1 : 0;
        co.fetch_details = o.fetch_details ? 1 : 0;
        co.match_wait_ms = priv::to_ms(o.match_wait);
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
        co.net.recv_buffer_bytes   = o.recv_buffer_bytes;
        co.net.send_buffer_bytes   = o.send_buffer_bytes;
        co.discovery.announce_interval_us = priv::to_us(o.announce_interval);
        co.discovery.peer_timeout_us      = priv::to_us(o.peer_timeout);
        co.discovery.max_peers            = o.max_peers;

        detail::RantAllocator mem = (o.memory && o.memory_size)
            ? detail::rant_allocator_static(o.memory, o.memory_size)
            : detail::rant_allocator_heap(0);
        detail::RantNode* n = detail::rant_node_open(
            &mem, nm.empty() ? nullptr : nm.c_str(),
            &Node::on_msg_tramp, &Node::on_evt_tramp, &co);
        if (!n) { priv::raise_last(nullptr, "rant::Node open"); return; }
        impl->node = n;
        if (o.threading == Threading::Dispatch) {
            impl->queue = detail::rant_node_create_queue(n);
            if (!impl->queue || detail::rant_node_set_event_queue(n, impl->queue) != 0) {
                priv::raise_last(n, "rant::Node: the dispatch queue");
                return;
            }
        }
        if (o.threading != Threading::Manual && detail::rant_node_start(n) != 0) {
            priv::raise_last(n, "rant::Node: the service thread");
            return;
        }
        impl_ = std::move(impl);
    }

    bool valid() const noexcept { return impl_ != nullptr && impl_->node != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    Node(Node&&) noexcept = default;
    Node& operator=(Node&& o) noexcept {
        if (this != &o) { close(); impl_ = std::move(o.impl_); }
        return *this;
    }
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    ~Node() { close(); }

    /* Stop the loop, leave the mesh with a bye and free the node. A handle that outlives it
     * answers NoTopic. Close before the state its handlers capture goes out of scope. */
    void close() {
        if (impl_) impl_->shutdown();
        impl_.reset();
    }

    /* Who runs the loop, as opened. */
    Threading threading() const { return valid() ? impl_->threading : Threading::Manual; }

    /* Manual threading: one loop tick of discovery, receive, timers and queued sends, where
     * callbacks fire. Blocks up to timeout, 0 = non blocking. State on another threading. */
    int poll(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        if (!valid()) return (int)SendStatus::State;
        return detail::rant_node_poll(impl_->node, priv::to_ms(timeout));
    }

    /* Dispatch threading: run the parked callbacks on this thread, at most max_callbacks
     * (0 = all), waiting up to timeout for the first (rant::forever = no end). The count
     * run, or State on another threading. */
    int dispatch(int max_callbacks = 0, std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        if (!valid() || !impl_->queue) return (int)SendStatus::State;
        return detail::rant_queue_dispatch(impl_->queue, max_callbacks, priv::to_ms(timeout));
    }

    /* Peer, loss and error events, where the node's callbacks run. Optional: with none set
     * errors print to stderr, and last_error() records the last one either way. A later call
     * replaces the handler and {} restores the default. */
    void on_event(EventHandler h) {
        if (!valid()) return;
        std::shared_ptr<const EventHandler> p;
        if (h) p = std::make_shared<const EventHandler>(std::move(h));
        std::lock_guard<std::mutex> g(impl_->reg_mu);
        impl_->on_event = std::move(p);
    }

    /* Block until discovery and matching settle, so everything sent now reaches everyone on
     * the network. Call after creating the topics. No timeout = 3 announce intervals. */
    bool settle(std::optional<std::chrono::milliseconds> timeout = std::nullopt) {
        return valid() && detail::rant_node_settle(impl_->node, priv::to_ms(timeout)) == 1;
    }

    /* The mesh as this node sees it: peers, entities, the folded mesh and meta snapshots. */
    Reflection reflection() const;

    /* This node's own counters: memory, the reliable send waits, and the sends that evicted
     * never sent history after the bounded wait (the send burst indicator). */
    struct Stats {
        size_t   memory_in_use = 0, memory_peak = 0;
        uint64_t alloc_calls = 0;
        uint64_t backpressure_waited_us = 0;
        uint32_t backpressure_waited_sends = 0;
        uint32_t evicted_unsent = 0;
    };
    Stats stats() const {
        Stats s;
        if (!valid()) return s;
        detail::rant_node_mem_stats(impl_->node, &s.memory_in_use, &s.memory_peak, &s.alloc_calls);
        detail::rant_node_backpressure_stats(impl_->node, &s.backpressure_waited_us, &s.backpressure_waited_sends);
        s.evicted_unsent = detail::rant_node_evicted_unsent(impl_->node);
        return s;
    }

    /* The most recent error this node reported (also delivered via on_event): the
     * formatted one-line message, and its machine-readable ErrorKind. */
    std::string last_error() const {
        detail::RantEvent e = detail::rant_last_error(valid() ? impl_->node : nullptr);
        char b[192]; return detail::rant_event_str(&e, b, sizeof b);
    }
    ErrorKind last_error_kind() const {
        return static_cast<ErrorKind>(detail::rant_last_error(valid() ? impl_->node : nullptr).error);
    }

    /* Compiles schema text in this node's registry: every definition stays in scope for the
     * node's later compiles, and a name on its own is a schema. The node owns the result for
     * its life, one handle per shape. Throws rant::Error, or under -fno-exceptions returns an
     * empty Schema, with last_error() saying where. */
    Schema schema(std::string_view text) {
        std::string t(text);
        const detail::RantSchema* s = valid() ? detail::rant_node_schema(impl_->node, t.c_str()) : nullptr;
        if (!s) priv::raise_last(valid() ? impl_->node : nullptr, "rant::Node::schema");
        return Schema(s);
    }
    /* The same from a schema's wire bytes, such as a peer's. */
    Schema schema_from_wire(Bytes wire) {
        const detail::RantSchema* s = valid()
            ? detail::rant_node_schema_parse(impl_->node, wire.data(), wire.size()) : nullptr;
        if (!s) priv::raise_last(valid() ? impl_->node : nullptr, "rant::Node::schema_from_wire");
        return Schema(s);
    }

    /* Publish an already formatted line on a level's built in log topic, truncated at
     * RANT_LOG_MAX. SendStatus::NoSys when logs are disabled. */
    SendStatus log(LogLevel level, std::string_view text) {
        if (!valid()) return SendStatus::State;
        return static_cast<SendStatus>(detail::rant_node_log_text(
            impl_->node, static_cast<detail::RantLogLevel>(level),
            text.data(), static_cast<int>(text.size())));
    }

    /* printf style overloads with the C log API's bounded formatting: truncated at
     * RANT_LOG_MAX, and a formatting failure publishes an empty line. */
    template <class... Args>
    SendStatus log(LogLevel level, const char* fmt, Args&&... args) {
        if (!valid()) return SendStatus::State;
        if (!fmt) return SendStatus::NoTopic;
        char text[RANT_LOG_MAX];
        int len = std::snprintf(text, sizeof text, fmt, std::forward<Args>(args)...);
        if (len < 0) len = 0;
        if (len >= static_cast<int>(sizeof text)) len = static_cast<int>(sizeof text) - 1;
        return log(level, std::string_view(text, static_cast<size_t>(len)));
    }

    /* A callback queue of this node, for handles that want their callbacks on a thread of
     * their own: name it in their options. At most 8 per node, freed with the node. */
    Queue create_queue() {
        if (!valid()) { priv::raise_msg("rant::Node::create_queue: node is not valid"); return {}; }
        detail::RantQueue* q = detail::rant_node_create_queue(impl_->node);
        if (!q) { priv::raise_last(impl_->node, "rant::Node::create_queue"); return {}; }
        return Queue(q);
    }
    /* Every other node's log lines at every level, decoded to a LogLine, where the node's
     * callbacks run. One handler: a later call replaces it and {} clears it. Throws, or
     * returns false, when this node was opened with disable_logs. */
    bool on_log(std::function<void(const LogLine&)> h) {
        if (!valid()) return false;
        std::shared_ptr<const LogHandler> p;
        if (h) p = std::make_shared<const LogHandler>(std::move(h));
        {
            std::lock_guard<std::mutex> g(impl_->reg_mu);
            impl_->on_log = std::move(p);
        }
        std::lock_guard<std::mutex> g(impl_->create_mu);
        if (impl_->log_bound || !impl_->on_log) return true;
        /* the queue first, so the catch up replay parks too */
        if (impl_->queue && detail::rant_node_set_log_queue(impl_->node, impl_->queue) != 0) {
            priv::raise_last(impl_->node, "rant::Node::on_log");
            return false;
        }
        const LogLevel levels[] = { LogLevel::Error, LogLevel::Warn, LogLevel::Info };
        for (LogLevel level : levels) {
            detail::RantTopic* ch = detail::rant_node_log_topic(
                impl_->node, static_cast<detail::RantLogLevel>(level));
            if (!ch) { priv::raise_msg("rant::Node::on_log: logs are disabled on this node"); return false; }
            if (detail::rant_topic_set_role(ch, detail::RANT_PUBSUB) != 0) {
                priv::raise_last(impl_->node, "rant::Node::on_log");
                return false;
            }
            Impl* impl = impl_.get();   /* the handler lives in impl, so no ownership here */
            MessageHandler mh = [impl, level](const MessageView& m) {
                std::shared_ptr<const LogHandler> f;
                {
                    std::lock_guard<std::mutex> g2(impl->reg_mu);
                    f = impl->on_log;
                }
                if (!f) return;
                LogLine ln;
                ln.level   = level;
                ln.node    = m.publisher_name();
                ln.node_id = m.publisher_id();
                ln.wall_us = m.get_uint("wallUs");
                ln.mono_us = m.get_uint("monoUs");
                ln.recv_us = m.recv_us();
                ln.written_us = m.written_us();
                ln.text    = m.get_string("text");
                (*f)(ln);
            };
            uint16_t idx = detail::rant_topic_index(ch);
            std::lock_guard<std::mutex> g2(impl_->reg_mu);   /* leaf lock: never call C while held */
            auto& slot = impl_->sub_handlers[idx];
            auto nv = slot ? std::make_shared<std::vector<priv::SubHandler>>(*slot)
                           : std::make_shared<std::vector<priv::SubHandler>>();
            nv->push_back(priv::SubHandler{ 0, std::move(mh) });
            slot = std::move(nv);
        }
        impl_->log_bound = true;
        return true;
    }

    /* The handle factories. T is a RANT_SCHEMA struct, a plain value type, or rant::Bytes
     * for raw bytes read through a schema. Only a rant::Bytes handle takes a schema, and
     * one without a schema may set reflect_from_mesh in its options. */
    template <class T>
    Publisher<T> publisher(std::string_view name, const Qos& qos = {}, const Schema& schema = {});
    /* handler is void(const T&) or void(const T&, const MessageView&), and for rant::Bytes
     * void(const MessageView&). It runs on the loop thread or at dispatch() of qos.queue. */
    template <class T, class H, class = std::enable_if_t<priv::is_message_handler<T, H>::value>>
    Subscriber<T> subscriber(std::string_view name, H&& handler, const Qos& qos = {},
                             const Schema& schema = {});
    /* No handler: pulled, messages wait until take() or take_latest() reads them. */
    template <class T>
    Subscriber<T> subscriber(std::string_view name, const Qos& qos = {}, const Schema& schema = {});
#ifndef RANT_NO_PATTERNS
    /* handler is Rsp(const Req&) or void(const Req&, Request<Rsp>&), and for rant::Bytes
     * void(Request<Bytes>&). A throwing handler answers AppError. */
    template <class Req, class Rsp, class H>
    FunctionDefinition<Req, Rsp> function_definition(std::string_view name, H&& handler,
        const FunctionOptions& o = {}, const Schema& req_schema = {}, const Schema& rsp_schema = {});
    template <class Req, class Rsp>
    RemoteFunction<Req, Rsp> remote_function(std::string_view name, const FunctionOptions& o = {},
        const Schema& req_schema = {}, const Schema& rsp_schema = {});
    /* handler is void(const Req&, TaskRequest<Prg, Rsp>&), and for rant::Bytes
     * void(TaskRequest<Bytes, Bytes>&). */
    template <class Req, class Prg, class Rsp, class H>
    TaskDefinition<Req, Prg, Rsp> task_definition(std::string_view name, H&& handler,
        const TaskOptions& o = {}, const Schema& req_schema = {}, const Schema& prg_schema = {},
        const Schema& rsp_schema = {});
    template <class Req, class Prg, class Rsp>
    RemoteTask<Req, Prg, Rsp> remote_task(std::string_view name, const TaskOptions& o = {},
        const Schema& req_schema = {}, const Schema& prg_schema = {}, const Schema& rsp_schema = {});
    template <class T>
    VariableDefinition<T> variable_definition(std::string_view name,
        const VariableOptions<T>& o = {}, const Schema& schema = {});
    template <class T>
    RemoteVariable<T> remote_variable(std::string_view name, const VariableOptions<T>& o = {},
        const Schema& schema = {});

#endif

private:
    using LogHandler = priv::LogHandler;
    using Impl = priv::NodeImpl;
    std::shared_ptr<Impl> impl_;

    struct LockGuard {
        detail::RantNode* n;
        explicit LockGuard(detail::RantNode* node) : n(node) { detail::rant_node_lock(n); }
        ~LockGuard() { detail::rant_node_unlock(n); }
    };

    static void on_msg_tramp(const detail::RantMsg* m) {
        Impl* impl = static_cast<Impl*>(m->user);
        if (!impl) return;
        std::shared_ptr<const std::vector<priv::SubHandler>> hs;
        {
            std::lock_guard<std::mutex> g(impl->reg_mu);
            auto it = impl->sub_handlers.find(m->topic_index);
            if (it != impl->sub_handlers.end()) hs = it->second;
        }
        if (!hs) return;
        MessageView msg(m);
        for (const priv::SubHandler& h : *hs) priv::guarded(impl, [&] { h.fn(msg); });
    }
    static void on_evt_tramp(const detail::RantEvent* e) {
        Impl* impl = static_cast<Impl*>(e->user);
        if (impl) impl->emit(Event(e));
    }

    /* a schema view from a walk, registered in the node so it outlives the poll */
    static Schema keep_schema(detail::RantNode* node, const detail::RantSchema* view) {
        if (!view) return Schema();
        detail::RantBytes w = detail::rant_schema_wire(view);
        return Schema(detail::rant_node_schema_parse(node, w.data, w.len));
    }

    /* one entity view (valid under the node lock the walk holds) into an owned Entity */
    static Entity entity_from(detail::RantNode* node, const detail::RantEntityInfo& ei) {
        Entity e;
        e.kind = static_cast<EntityKind>(ei.kind);
        if (ei.name.data) e.name.assign(ei.name.data, ei.name.len);
        else {
            char hx[16];
            std::snprintf(hx, sizeof hx, "0x%08x", (unsigned)ei.hash);
            e.name = hx;
        }
        e.hash       = ei.hash;
        e.provides   = ei.provides != 0;
        e.consumes   = ei.consumes != 0;
        e.reliable   = ei.reliable != 0;
        e.writable   = ei.writable != 0;
        e.forceable  = ei.forceable != 0;
        e.cancellable= ei.cancellable != 0;
        e.exclusive  = ei.exclusive != 0;
        e.multi      = ei.multi != 0;
        e.incomplete = ei.incomplete != 0;
        e.conflict   = ei.conflict != 0;
        e.providers  = ei.providers;
        e.consumers  = ei.consumers;
        e.provider   = ei.provider;
        if (ei.from.data) e.from.assign(ei.from.data, ei.from.len);
        e.schema          = keep_schema(node, ei.schema);
        e.rsp_schema      = keep_schema(node, ei.rsp_schema);
        e.progress_schema = keep_schema(node, ei.progress_schema);
        e.generation = ei.generation;
        return e;
    }

    static detail::RantQos to_c(const Qos& q) {
        detail::RantQos c;
        std::memset(&c, 0, sizeof c);
        c.reliability          = static_cast<detail::RantReliability>(q.reliability);
        c.keep_last            = q.keep_last;
        c.catch_up             = q.catch_up;
        c.max_message_bytes    = q.max_message_bytes;
        c.heartbeat_us         = priv::to_us(q.heartbeat);
        c.repair_delay_us      = priv::to_us(q.repair_delay);
        c.backpressure_wait_us = priv::to_us(q.backpressure_wait);
        c.shm_max_bytes        = q.shm_max_bytes;
        c.queue_bytes          = q.queue_bytes;
        c.max_rate_hz          = q.max_rate_hz;
        c.no_timestamp         = q.no_timestamp ? 1 : 0;
        return c;
    }


    /* Parse "ip" or "ip:port" into a locator, port 0 = discovery_port. Hand rolled so no
     * locale bound scanf is needed. */
    static bool parse_addr(const std::string& s, detail::RantAddr& out) {
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

    friend class priv::TopicCore;
    friend class Reflection;
    template <class A> friend priv::TypeCodec* priv::type_codec(Node& n);
    template <class A, class B> friend class FunctionDefinition;
    template <class A, class B> friend class RemoteFunction;
    template <class A, class B, class C> friend class TaskDefinition;
    template <class A, class B, class C> friend class RemoteTask;
    template <class A> friend class VariableDefinition;
    template <class A> friend class RemoteVariable;
    template <class A> friend class Publisher;
    template <class A> friend class Subscriber;
};

/* The mesh as one node sees it, from node.reflection(). Every walk returns a copied
 * snapshot that outlives the loop (docs/reflection.md). Valid while its node lives. */
class Reflection {
public:
    /* Every discovered peer, dropped ones included, so check active. */
    std::vector<Peer> peers() const {
        std::vector<Peer> out;
        if (!impl_ || !impl_->node) return out;
        Node::LockGuard guard(impl_->node);
        detail::RantIter it; std::memset(&it, 0, sizeof it);
        detail::RantPeerInfo p;
        while (detail::rant_node_peers_next(impl_->node, &it, &p)) {
            Peer peer;
            peer.id = p.id;
            std::memcpy(peer.uuid.data(), p.uuid, 16);
            if (p.name.data)    peer.name.assign(p.name.data, p.name.len);
            if (p.address.data) peer.address.assign(p.address.data, p.address.len);
            peer.active        = (p.liveness == detail::RANT_PEER_ACTIVE);
            peer.last_heard_us = p.last_heard_us;
            peer.epoch         = p.epoch;
            peer.catching_up   = p.catching_up != 0;
            peer.fragment_size = p.fragment_size;
            peer.rtt_us        = p.rtt_us;
            peer.rtt_jitter_us = p.rtt_jitter_us;
            peer.rtt_min_us    = p.rtt_min_us;
            peer.rtt_samples   = p.rtt_samples;
            out.push_back(std::move(peer));
        }
        return out;
    }
    /* The entities one node advertises, peer 0 for this one. It copies every schema. */
    std::vector<Entity> entities(uint32_t peer = 0) const {
        std::vector<Entity> out;
        if (!impl_ || !impl_->node) return out;
        Node::LockGuard guard(impl_->node);
        detail::RantIter it; std::memset(&it, 0, sizeof it);
        detail::RantEntityInfo ei;
        while (detail::rant_node_entities_next(impl_->node, peer, &it, &ei))
            out.push_back(Node::entity_from(impl_->node, ei));
        return out;
    }
    /* The whole mesh folded: one entity per kind and name across every active peer and this
     * node, schemas from the provider, conflict when the endpoints disagree. */
    std::vector<Entity> mesh() const {
        std::vector<Entity> out;
        if (!impl_ || !impl_->node) return out;
        Node::LockGuard guard(impl_->node);
        detail::RantIter it; std::memset(&it, 0, sizeof it);
        detail::RantEntityInfo ei;
        while (detail::rant_node_mesh_next(impl_->node, &it, &ei))
            out.push_back(Node::entity_from(impl_->node, ei));
        return out;
    }
    /* One folded entity by kind and name, or nullopt. */
    std::optional<Entity> find(EntityKind kind, std::string_view name) const {
        if (!impl_ || !impl_->node) return std::nullopt;
        Node::LockGuard guard(impl_->node);
        detail::RantEntityInfo ei;
        std::string nm(name);
        if (!detail::rant_node_mesh_find(impl_->node, static_cast<detail::RantEntityKind>(kind),
                                         nm.c_str(), &ei)) return std::nullopt;
        return Node::entity_from(impl_->node, ei);
    }
    /* Moves on every reflected change anywhere in the mesh: re walk only when it moved. */
    uint32_t epoch() const {
        return impl_ && impl_->node ? detail::rant_node_mesh_epoch(impl_->node) : 0;
    }
#ifndef RANT_NO_PATTERNS
    /* A peer's snapshot through a directed @rant/meta call, blocking up to timeout_ms.
     * sections is a MetaSection mask, 0 = all. valid is false when it did not answer. */
    MetaSnapshot meta(uint32_t peer, uint32_t sections = 0,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
    /* The same, returning at once: cb fires once where the node's callbacks run. */
    SendStatus meta_async(uint32_t peer, std::function<void(const MetaSnapshot&)> cb,
                          uint32_t sections = 0);
#endif

private:
    explicit Reflection(std::shared_ptr<priv::NodeImpl> impl) : impl_(std::move(impl)) {}
#ifndef RANT_NO_PATTERNS
    /* the node's own @rant/meta caller handle, invalid when meta is disabled */
    RemoteFunction<Bytes, Bytes> meta_fn();
    static Bytes sections_req(uint32_t sections, uint8_t* buf);
#endif
    std::shared_ptr<priv::NodeImpl> impl_;
    friend class Node;
};

inline Reflection Node::reflection() const { return Reflection(impl_); }

/* Create, or share the same name slot with a widened role. A live same name topic with a
 * different schema refuses, since two modules disagreeing is a bug. Close its handles first. */
namespace priv {
template <class T> inline const char codec_key = 0;   /* one address per T, the codec map key */
}
template <class T> priv::TypeCodec* priv::type_codec(Node& n) {
    Node::Impl* impl = n.impl_.get();
    if (!impl) return nullptr;
    std::lock_guard<std::mutex> g(impl->codec_mu);
    auto it = impl->codecs.find(&codec_key<T>);
    if (it != impl->codecs.end()) return it->second.get();
    std::unique_ptr<TypeCodec> c(build_codec<T>(impl->node));
    return impl->codecs.emplace(&codec_key<T>, std::move(c)).first->second.get();
}

inline priv::TopicCore::TopicCore(Node& node, std::string_view name, Role role,
                                  const Schema* schema, const Qos& qos, bool pull) {
    if (!node.valid()) { priv::raise_msg("rant topic create: node is not valid"); return; }
    const std::shared_ptr<NodeImpl>& impl = node.impl_;
    std::lock_guard<std::mutex> g(impl->create_mu);
    std::string nm(name);
    uint64_t sh = schema ? schema->hash() : 0;
    detail::RantQueue* want_q = pull ? priv::to_c(qos.queue) : impl->queue_for(qos.queue);
    uint8_t rb = priv::role_bits(role);
    auto it = impl->topics.find(nm);
    if (it != impl->topics.end()) {
        if (sh && it->second.schema_hash && sh != it->second.schema_hash) {
            priv::raise_msg("rant topic create: the name already exists with a different schema"
                            " (close every handle on it to retype the name)");
            return;
        }
        if (want_q != it->second.queue || pull != it->second.pull) {
            priv::raise_msg("rant topic create: the name already exists with a different queue"
                            " (same name handles share one slot and its queue)");
            return;
        }
        uint8_t bits = (uint8_t)(it->second.bits | rb);
        if (bits != it->second.bits) {
            detail::rant_topic_set_role(it->second.ch, static_cast<detail::RantRole>(priv::role_from_bits(bits)));
            it->second.bits = bits;
        }
        it->second.pubs += rb & 1;
        it->second.subs += (rb >> 1) & 1;
        ch_ = it->second.ch;
        impl_ = impl;
        bits_ = rb;
        return;
    }
    detail::RantTopicOpts co;
    std::memset(&co, 0, sizeof co);
    co.qos = Node::to_c(qos);
    co.reflect_from_mesh = qos.reflect_from_mesh ? 1 : 0;
    co.queue = want_q;
    co.pull = pull ? 1 : 0;
    detail::RantTopic* ch = detail::rant_node_create_topic(impl->node, nm.c_str(),
              static_cast<detail::RantRole>(role), schema ? schema->raw() : nullptr, &co);
    if (!ch) { priv::raise_last(impl->node, "rant topic create"); return; }
    impl->topics.emplace(std::move(nm), TopicRec{ ch, rb, sh, want_q, pull, rb & 1, (rb >> 1) & 1 });
    ch_ = ch;
    impl_ = impl;
    bits_ = rb;
}

inline std::string priv::TopicCore::name() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> g(impl_->create_mu);
    for (const auto& kv : impl_->topics) if (kv.second.ch == ch_) return kv.first;
    return {};
}

inline void priv::TopicCore::add_handler(MessageHandler h) {
    if (!live()) return;
    uint16_t idx = detail::rant_topic_index(ch_);
    std::lock_guard<std::mutex> g(impl_->reg_mu);
    handler_id_ = impl_->next_handler_id++;
    auto& slot = impl_->sub_handlers[idx];
    auto nv = slot ? std::make_shared<std::vector<SubHandler>>(*slot)
                   : std::make_shared<std::vector<SubHandler>>();
    nv->push_back(SubHandler{ handler_id_, std::move(h) });
    slot = std::move(nv);
}

/* The last side on the name retires the topic and forgets its handlers, since the slot may
 * be reused by another name. Otherwise the role narrows to the sides still held. */
inline SendStatus priv::TopicCore::close() {
    if (!ch_) return SendStatus::NoTopic;
    if (!impl_->node) {   /* the node closed and freed the topic */
        ch_ = nullptr; bits_ = 0; handler_id_ = 0; impl_.reset();
        return SendStatus::Ok;
    }
    NodeImpl& impl = *impl_;
    std::lock_guard<std::mutex> g(impl.create_mu);
    uint16_t idx = detail::rant_topic_index(ch_);
    auto it = impl.topics.begin();
    while (it != impl.topics.end() && it->second.ch != ch_) ++it;
    int pubs = 0, subs = 0;
    if (it != impl.topics.end()) {
        pubs = it->second.pubs - (bits_ & 1);
        subs = it->second.subs - ((bits_ >> 1) & 1);
    }
    if (pubs <= 0 && subs <= 0) {
        int rc = detail::rant_topic_retire(ch_);
        if (rc != 0) return static_cast<SendStatus>(rc);
        if (it != impl.topics.end()) impl.topics.erase(it);
        std::lock_guard<std::mutex> g2(impl.reg_mu);
        impl.sub_handlers.erase(idx);
    } else {
        uint8_t bits = (uint8_t)((pubs > 0 ? 1 : 0) | (subs > 0 ? 2 : 0));
        if (bits != it->second.bits) {
            detail::rant_topic_set_role(ch_, static_cast<detail::RantRole>(priv::role_from_bits(bits)));
            it->second.bits = bits;
        }
        it->second.pubs = pubs;
        it->second.subs = subs;
        if (handler_id_) {
            std::lock_guard<std::mutex> g2(impl.reg_mu);
            auto hit = impl.sub_handlers.find(idx);
            if (hit != impl.sub_handlers.end() && hit->second) {
                auto nv = std::make_shared<std::vector<SubHandler>>();
                for (const SubHandler& sh : *hit->second) if (sh.id != handler_id_) nv->push_back(sh);
                hit->second = std::move(nv);
            }
        }
    }
    ch_ = nullptr; bits_ = 0; handler_id_ = 0; impl_.reset();
    return SendStatus::Ok;
}

#ifndef RANT_NO_PATTERNS

namespace priv {
/* Retire a function or task handle's C entity, nothing to do once the node closed. A
 * handle made by the node for its own endpoint is never retired. */
inline SendStatus close_function(detail::RantFunction*& fn, std::shared_ptr<NodeImpl>& impl, bool owned) {
    if (!fn) return SendStatus::NoTopic;
    if (owned && impl && impl->node) {
        int rc = detail::rant_function_retire(fn);
        if (rc != 0) return static_cast<SendStatus>(rc);
    }
    fn = nullptr;
    impl.reset();
    return SendStatus::Ok;
}
}   /* namespace priv */

/* ====================== FUNCTIONS (untyped cores) =========================== */

/* The untyped implementation side of a function. One reply per call, one definition per
 * name on the network. Move only, and scope end or close() retires it. */
template <> class FunctionDefinition<Bytes, Bytes> {
public:
    using Handler = std::function<void(Request<Bytes>&)>;

    FunctionDefinition() = default;
    FunctionDefinition(FunctionDefinition&& o) noexcept { move_from(o); }
    FunctionDefinition& operator=(FunctionDefinition&& o) noexcept {
        if (this != &o) { (void)close(); move_from(o); }
        return *this;
    }
    FunctionDefinition(const FunctionDefinition&) = delete;
    FunctionDefinition& operator=(const FunctionDefinition&) = delete;
    ~FunctionDefinition() {
        if (close() == SendStatus::State && impl_) impl_->report_close_refused(name_);
    }
    /* A reflect_from_mesh handle: re-type in place when the mesh moved (true = re-typed). */
    bool refresh() { return live() && detail::rant_function_refresh(fn_) == 1; }

private:
    FunctionDefinition(Node& n, std::string_view name, const Schema* req_schema,
                       const Schema* rsp_schema, Handler handler, const FunctionOptions& o) {
        if (!n.valid()) { priv::raise_msg("rant::FunctionDefinition: node is not valid"); return; }
        if (!handler) { priv::raise_msg("rant::FunctionDefinition: a definition needs a handler"); return; }
        std::string nm(name);
        impl_ = n.impl_;
        name_ = nm;
        detail::RantFunctionOpts co;
        std::memset(&co, 0, sizeof co);
        co.backpressure_wait_us = priv::to_us(o.backpressure_wait);
        co.timeout_us           = priv::to_us(o.timeout);
        co.keep_last            = o.keep_last;
        co.reflect_from_mesh    = o.reflect_from_mesh ? 1 : 0;
        co.queue                = n.impl_->queue_for(o.queue);
        Box* box = new Box();
        box->h = std::move(handler);
        box->impl = impl_;
        fn_ = detail::rant_node_create_function_definition(n.impl_->node, nm.c_str(),
                  req_schema ? req_schema->raw() : nullptr,
                  rsp_schema ? rsp_schema->raw() : nullptr,
                  &FunctionDefinition::tramp, box, &co);
        if (!fn_) {
            delete box;
            priv::raise_last(n.impl_->node, "rant::FunctionDefinition create");
            return;
        }
        box->fn.store(fn_);
        std::lock_guard<std::mutex> g(impl_->reg_mu);
        impl_->boxes.emplace_back(box);
    }
public:

    bool valid() const noexcept { return live() != nullptr; }
    explicit operator bool() const noexcept { return valid(); }
    /* callers currently matched to this definition */
    int match_count() const { return live() ? detail::rant_function_match_count(fn_) : 0; }
    /* Retire the definition and release the name for a successor. The handle is empty after. State
     * from its own inline callback, and the handle stays valid then. */
    SendStatus close() { return priv::close_function(fn_, impl_, owned_); }
    const std::string& name() const { return name_; }
    /* The schemas in use now, empty when untyped. A reflect_from_mesh handle reports what
     * it adopted. */
    Schema request_schema() const {
        detail::RantFunction* f = live();
        return Schema(f ? detail::rant_function_request_schema(f) : nullptr);
    }
    Schema response_schema() const {
        detail::RantFunction* f = live();
        return Schema(f ? detail::rant_function_response_schema(f) : nullptr);
    }

private:
    struct Box : priv::HandlerBox {
        Handler h;
        std::atomic<detail::RantFunction*> fn{ nullptr };
        std::weak_ptr<priv::NodeImpl> impl;   /* weak: the node owns the box */
    };
    static void tramp(detail::RantRequest* rq, void* user) {
        Box* b = static_cast<Box*>(user);
        Request<Bytes> r(rq, b->fn.load(), b->impl.lock());
        priv::answered(rq, [&] { b->h(r); });
    }
    detail::RantFunction* live() const noexcept { return impl_ && impl_->node ? fn_ : nullptr; }
    void move_from(FunctionDefinition& o) noexcept {
        fn_ = o.fn_; impl_ = std::move(o.impl_); name_ = std::move(o.name_); owned_ = o.owned_;
        o.fn_ = nullptr;
    }
    detail::RantFunction* fn_ = nullptr;
    std::shared_ptr<priv::NodeImpl> impl_;
    std::string name_;
    bool owned_ = true;   /* false for the node's own @rant/meta endpoint */
    template <class A, class B> friend class FunctionDefinition;
    friend class Node;
};

/* RemoteFunction<Bytes, Bytes> (untyped): a reference to a definition on another node. */
template <> class RemoteFunction<Bytes, Bytes> {
public:
    RemoteFunction() = default;
    RemoteFunction(RemoteFunction&& o) noexcept { move_from(o); }
    RemoteFunction& operator=(RemoteFunction&& o) noexcept {
        if (this != &o) { (void)close(); move_from(o); }
        return *this;
    }
    RemoteFunction(const RemoteFunction&) = delete;
    RemoteFunction& operator=(const RemoteFunction&) = delete;
    ~RemoteFunction() {
        if (close() == SendStatus::State && impl_) impl_->report_close_refused(name_);
    }
    /* A reflect_from_mesh handle: re type in place when the mesh moved. true = re typed, and
     * outstanding calls are answered Cancelled first. */
    bool refresh() { return live() && detail::rant_function_refresh(fn_) == 1; }

private:
    RemoteFunction(Node& n, std::string_view name, const Schema* req_schema,
                   const Schema* rsp_schema, const FunctionOptions& o) {
        if (!n.valid()) { priv::raise_msg("rant::RemoteFunction: node is not valid"); return; }
        std::string nm(name);
        impl_ = n.impl_;
        name_ = nm;
        detail::RantFunctionOpts co;
        std::memset(&co, 0, sizeof co);
        co.backpressure_wait_us = priv::to_us(o.backpressure_wait);
        co.timeout_us           = priv::to_us(o.timeout);
        co.keep_last            = o.keep_last;
        co.reflect_from_mesh    = o.reflect_from_mesh ? 1 : 0;
        co.queue                = n.impl_->queue_for(o.queue);
        fn_ = detail::rant_node_create_remote_function(n.impl_->node, nm.c_str(),
                  req_schema ? req_schema->raw() : nullptr,
                  rsp_schema ? rsp_schema->raw() : nullptr, &co);
        if (!fn_) { priv::raise_last(n.impl_->node, "rant::RemoteFunction create"); return; }
    }
public:

    bool valid() const noexcept { return live() != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /* Blocking call: waits for the response or timeout_ms, negative = the default, on the
     * service thread's progress under start() and driving the loop otherwise. Refused with
     * State from a callback, use call_async there. */
    Response<Bytes> call(Bytes req, std::optional<std::chrono::milliseconds> timeout = std::nullopt, const CallOptions& opts = {}) {
        Response<Bytes> r;
        if (!live()) { r.ss_ = SendStatus::NoTopic; return r; }
        detail::RantResponse out;
        std::memset(&out, 0, sizeof out);
        detail::RantCallOpts co; std::memset(&co, 0, sizeof co);
        co.provider = opts.provider; co.id_out = opts.id_out;
        int rc = detail::rant_function_call(fn_, priv::to_c(req), &out, priv::to_ms(timeout), &co);
        if (rc == 1) {
            r.st_       = static_cast<CallStatus>(out.status);
            r.provider_ = out.provider;
            r.written_  = out.written_us;
            r.schema_   = out.schema;
            if (out.data.len) r.data_.assign(out.data.data, out.data.data + out.data.len);
        } else if (rc < 0) {
            r.ss_ = static_cast<SendStatus>(rc);   /* status() stays Timeout: not answered */
            if (r.ss_ == SendStatus::State)
                priv::raise_msg("rant: a blocking call from an inline callback, use call_async or put the handle on a queue");
        }
        if (rc >= 0 && out.message.len)   /* answered or timed out: copy the outcome text */
            r.message_.assign(out.message.data, out.message.len);
        return r;
    }
    /* Async form: returns once the request is committed. on_response fires once with the
     * outcome on the polling thread. */
    SendStatus call_async(Bytes req, std::function<void(const ResponseView<Bytes>&)> on_response,
                          const CallOptions& opts = {}) {
        if (!live()) return SendStatus::NoTopic;
        priv::AsyncBox* box = new priv::AsyncBox{ std::move(on_response),
                                                  &impl_->reg_mu, &impl_->async_live, {}, impl_.get() };
        {
            std::lock_guard<std::mutex> g(impl_->reg_mu);
            impl_->async_live.insert(box);
        }
        detail::RantCallOpts co; std::memset(&co, 0, sizeof co);
        co.provider = opts.provider; co.id_out = opts.id_out;
        int rc = detail::rant_function_call_async(fn_, priv::to_c(req),
                                                  &RemoteFunction::async_tramp, box, &co);
        if (rc != 0) {
            std::lock_guard<std::mutex> g(impl_->reg_mu);
            impl_->async_live.erase(box);
            delete box;
        }
        return static_cast<SendStatus>(rc);
    }

    /* definitions currently matched, 0 = nobody provides the name yet */
    int  match_count()    const { return live() ? detail::rant_function_match_count(fn_) : 0; }
    /* Retire the remote and release the name. Every outstanding call completes Cancelled.
     * The handle is empty after. State from its own inline callback, and it stays valid then. */
    SendStatus close() { return priv::close_function(fn_, impl_, owned_); }
    const std::string& name() const { return name_; }
    /* The schemas in use now, empty when untyped. A reflect_from_mesh handle reports what
     * it adopted. */
    Schema request_schema() const {
        detail::RantFunction* f = live();
        return Schema(f ? detail::rant_function_request_schema(f) : nullptr);
    }
    Schema response_schema() const {
        detail::RantFunction* f = live();
        return Schema(f ? detail::rant_function_response_schema(f) : nullptr);
    }

private:
    /* wrap a node-owned function handle (the @rant/meta endpoint): callable, never
     * destroyed by us (functions are never torn down before the node). */
    RemoteFunction(detail::RantFunction* fn, std::shared_ptr<priv::NodeImpl> impl)
        : fn_(fn), impl_(std::move(impl)), owned_(false) {}
    static void async_tramp(const detail::RantResponse* r) {
        priv::AsyncBox* box = static_cast<priv::AsyncBox*>(r->user);
        {
            std::lock_guard<std::mutex> g(*box->mu);
            box->live->erase(box);
        }
        if (box->cb) {
            ResponseView<Bytes> rv(r);
            priv::guarded(box->impl, [&] { box->cb(rv); });
        }
        delete box;
    }
    detail::RantFunction* live() const noexcept { return impl_ && impl_->node ? fn_ : nullptr; }
    void move_from(RemoteFunction& o) noexcept {
        fn_ = o.fn_; impl_ = std::move(o.impl_); name_ = std::move(o.name_); owned_ = o.owned_;
        o.fn_ = nullptr;
    }
    detail::RantFunction* fn_ = nullptr;
    std::shared_ptr<priv::NodeImpl> impl_;
    std::string name_;
    bool owned_ = true;   /* false for the node's own @rant/meta endpoint */
    template <class A, class B> friend class RemoteFunction;
    friend class Node;
    friend class Reflection;
};

/* Reflection's meta calls: out of line so RemoteFunction<Bytes, Bytes> is complete here. */
inline RemoteFunction<Bytes, Bytes> Reflection::meta_fn() {
    if (!impl_ || !impl_->node) return RemoteFunction<Bytes, Bytes>();
    return RemoteFunction<Bytes, Bytes>(detail::rant_node_meta_function(impl_->node), impl_);
}
inline Bytes Reflection::sections_req(uint32_t sections, uint8_t* buf) {
    if (!sections) return Bytes();
    buf[0] = (uint8_t)(sections);       buf[1] = (uint8_t)(sections >> 8);
    buf[2] = (uint8_t)(sections >> 16); buf[3] = (uint8_t)(sections >> 24);
    return Bytes(buf, 4);
}
inline MetaSnapshot Reflection::meta(uint32_t peer, uint32_t sections, std::chrono::milliseconds timeout) {
    RemoteFunction<Bytes, Bytes> m = meta_fn();
    if (!m.valid()) return MetaSnapshot();
    uint8_t buf[4];
    return MetaSnapshot::decode(m.call(sections_req(sections, buf), timeout, CallOptions{ peer }));
}
inline SendStatus Reflection::meta_async(uint32_t peer,
                                         std::function<void(const MetaSnapshot&)> cb, uint32_t sections) {
    RemoteFunction<Bytes, Bytes> m = meta_fn();
    if (!m.valid()) return SendStatus::NoTopic;
    uint8_t buf[4];   /* the mask lives on the stack: call_async commits it now */
    return m.call_async(sections_req(sections, buf), [cb = std::move(cb)](const ResponseView<Bytes>& r) {
        cb(MetaSnapshot::decode(r));
    }, CallOptions{ peer });
}

/* ====================== TASKS (untyped cores) =============================== */

/* What RemoteTask::call_async returns: the send status plus the call id, the handle for
 * cancel(). id is 0 when the request never committed. */
struct TaskCall {
    SendStatus status = SendStatus::NoTopic;
    uint32_t   id     = 0;
    bool ok() const { return status == SendStatus::Ok; }
};

/* The untyped implementation side of a task (docs/tasks.md). The handler fires on the poll
 * thread and must be quick: answer inline or defer to a PendingTask. One definition per name. */
template <> class TaskDefinition<Bytes, Bytes, Bytes> {
public:
    using Handler = std::function<void(TaskRequest<Bytes, Bytes>&)>;

    TaskDefinition() = default;
    TaskDefinition(TaskDefinition&& o) noexcept { move_from(o); }
    TaskDefinition& operator=(TaskDefinition&& o) noexcept {
        if (this != &o) { (void)close(); move_from(o); }
        return *this;
    }
    TaskDefinition(const TaskDefinition&) = delete;
    TaskDefinition& operator=(const TaskDefinition&) = delete;
    ~TaskDefinition() {
        if (close() == SendStatus::State && impl_) impl_->report_close_refused(name_);
    }
    /* A reflect_from_mesh handle: re-type in place when the mesh moved (true = re-typed). */
    bool refresh() { return live() && detail::rant_function_refresh(fn_) == 1; }

private:
    TaskDefinition(Node& n, std::string_view name, const Schema* req_schema, const Schema* prg_schema,
                   const Schema* rsp_schema, Handler handler, const TaskOptions& o) {
        if (!n.valid()) { priv::raise_msg("rant::TaskDefinition: node is not valid"); return; }
        if (!handler) { priv::raise_msg("rant::TaskDefinition: a definition needs a handler"); return; }
        std::string nm(name);
        impl_ = n.impl_;
        name_ = nm;
        detail::RantTaskOpts co;
        std::memset(&co, 0, sizeof co);
        co.progress_best_effort = o.progress_best_effort ? 1 : 0;
        co.progress_keep_last   = o.progress_keep_last;
        co.keep_last            = o.keep_last;
        co.no_cancel            = o.no_cancel ? 1 : 0;
        co.exclusive            = o.exclusive ? 1 : 0;
        co.multi                = o.multi ? 1 : 0;
        co.timeout_us           = priv::to_us(o.timeout);
        co.backpressure_wait_us = priv::to_us(o.backpressure_wait);
        co.reflect_from_mesh    = o.reflect_from_mesh ? 1 : 0;
        co.queue                = n.impl_->queue_for(o.queue);
        Box* box = new Box();
        box->h = std::move(handler);
        box->impl = impl_;
        fn_ = detail::rant_node_create_task_definition(n.impl_->node, nm.c_str(),
                  req_schema ? req_schema->raw() : nullptr,
                  prg_schema ? prg_schema->raw() : nullptr,
                  rsp_schema ? rsp_schema->raw() : nullptr,
                  &TaskDefinition::tramp, box, &co);
        if (!fn_) {
            delete box;
            priv::raise_last(n.impl_->node, "rant::TaskDefinition create");
            return;
        }
        box->fn.store(fn_);
        std::lock_guard<std::mutex> g(impl_->reg_mu);
        impl_->boxes.emplace_back(box);
    }
public:

    bool valid() const noexcept { return live() != nullptr; }
    explicit operator bool() const noexcept { return valid(); }
    /* callers currently matched to this definition */
    int match_count() const { return live() ? detail::rant_function_match_count(fn_) : 0; }

    /* Cancel notification, one slot, {} clears. Fires on the poll thread with the cancelled
     * call's defer token. Optional, since polling PendingTask::cancelled is complete alone. */
    void on_cancel(std::function<void(uint64_t token)> h) {
        if (!live()) return;
        CancelBox* box = nullptr;
        if (h) { box = new CancelBox(); box->h = std::move(h); box->impl = impl_.get(); }
        detail::rant_function_on_cancel(fn_, box ? &TaskDefinition::cancel_tramp : nullptr, box);
        if (box) {   /* kept alive until node close, like every handler box */
            std::lock_guard<std::mutex> g(impl_->reg_mu);
            impl_->boxes.emplace_back(box);
        }
    }

    /* Retire the definition. Every live deferred call answers Cancelled first, so a RUNNING
     * caller never hangs. The handle is empty after. State from its own inline callback. */
    SendStatus close() { return priv::close_function(fn_, impl_, owned_); }
    const std::string& name() const { return name_; }
    /* The schemas in use now, empty when untyped. A reflect_from_mesh handle reports what
     * it adopted. */
    Schema request_schema() const {
        detail::RantFunction* f = live();
        return Schema(f ? detail::rant_function_request_schema(f) : nullptr);
    }
    Schema response_schema() const {
        detail::RantFunction* f = live();
        return Schema(f ? detail::rant_function_response_schema(f) : nullptr);
    }
    Schema progress_schema() const {
        detail::RantFunction* f = live();
        return Schema(f ? detail::rant_function_progress_schema(f) : nullptr);
    }

private:
    struct Box : priv::HandlerBox {
        Handler h;
        std::atomic<detail::RantFunction*> fn{ nullptr };
        std::weak_ptr<priv::NodeImpl> impl;   /* weak: the node owns the box */
    };
    struct CancelBox : priv::HandlerBox {
        std::function<void(uint64_t)> h;
        priv::NodeImpl* impl = nullptr;
    };
    static void tramp(detail::RantRequest* rq, void* user) {
        Box* b = static_cast<Box*>(user);
        TaskRequest<Bytes, Bytes> r(rq, b->fn.load(), b->impl.lock());
        priv::answered(rq, [&] { b->h(r); });
    }
    static void cancel_tramp(uint64_t token, void* user) {
        CancelBox* b = static_cast<CancelBox*>(user);
        priv::guarded(b->impl, [&] { b->h(token); });
    }
    detail::RantFunction* live() const noexcept { return impl_ && impl_->node ? fn_ : nullptr; }
    void move_from(TaskDefinition& o) noexcept {
        fn_ = o.fn_; impl_ = std::move(o.impl_); name_ = std::move(o.name_); owned_ = o.owned_;
        o.fn_ = nullptr;
    }
    detail::RantFunction* fn_ = nullptr;
    std::shared_ptr<priv::NodeImpl> impl_;
    std::string name_;
    bool owned_ = true;   /* false for the node's own @rant/meta endpoint */
    template <class A, class B, class C> friend class TaskDefinition;
    friend class Node;
};

/* The untyped reference to a task definition elsewhere. A request is always directed at
 * one provider. The timeout bounds only the first response, cancel() is the tool after. */
template <> class RemoteTask<Bytes, Bytes, Bytes> {
public:
    using ProgressHandler = std::function<void(const ProgressView<Bytes>&)>;

    RemoteTask() = default;
    RemoteTask(RemoteTask&& o) noexcept { move_from(o); }
    RemoteTask& operator=(RemoteTask&& o) noexcept {
        if (this != &o) { (void)close(); move_from(o); }
        return *this;
    }
    RemoteTask(const RemoteTask&) = delete;
    RemoteTask& operator=(const RemoteTask&) = delete;
    ~RemoteTask() {
        if (close() == SendStatus::State && impl_) impl_->report_close_refused(name_);
    }
    /* A reflect_from_mesh handle: re type in place when the mesh moved. true = re typed, and
     * outstanding calls are answered Cancelled first. */
    bool refresh() { return live() && detail::rant_function_refresh(fn_) == 1; }

private:
    RemoteTask(Node& n, std::string_view name, const Schema* req_schema, const Schema* prg_schema,
               const Schema* rsp_schema, const TaskOptions& o) {
        if (!n.valid()) { priv::raise_msg("rant::RemoteTask: node is not valid"); return; }
        std::string nm(name);
        impl_ = n.impl_;
        name_ = nm;
        detail::RantTaskOpts co;
        std::memset(&co, 0, sizeof co);
        co.progress_best_effort = o.progress_best_effort ? 1 : 0;
        co.progress_keep_last   = o.progress_keep_last;
        co.keep_last            = o.keep_last;
        co.timeout_us           = priv::to_us(o.timeout);
        co.backpressure_wait_us = priv::to_us(o.backpressure_wait);
        co.reflect_from_mesh    = o.reflect_from_mesh ? 1 : 0;
        co.queue                = n.impl_->queue_for(o.queue);
        fn_ = detail::rant_node_create_remote_task(n.impl_->node, nm.c_str(),
                  req_schema ? req_schema->raw() : nullptr,
                  prg_schema ? prg_schema->raw() : nullptr,
                  rsp_schema ? rsp_schema->raw() : nullptr, &co);
        if (!fn_) { priv::raise_last(n.impl_->node, "rant::RemoteTask create"); return; }
    }
public:

    bool valid() const noexcept { return live() != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /* Blocking call: waits for the terminal outcome on the service thread's progress, or
     * drives a Manual node's loop. on_progress fires where the node's callbacks run, so on
     * this thread only under Manual. Refused from a callback. id_out allows a cancel(). */
    Response<Bytes> call(Bytes req, ProgressHandler on_progress = {}, std::optional<std::chrono::milliseconds> timeout = std::nullopt,
                    const CallOptions& opts = {}) {
        Response<Bytes> r;
        if (!live()) { r.ss_ = SendStatus::NoTopic; return r; }
        detail::RantResponse out;
        std::memset(&out, 0, sizeof out);
        detail::RantCallOpts co; std::memset(&co, 0, sizeof co);
        co.provider = opts.provider; co.id_out = opts.id_out;
        BlockingProgress bp{ &on_progress, impl_.get() };
        if (on_progress) {   /* fires only inside rant_function_call: the stack copy holds */
            co.on_progress   = &RemoteTask::blocking_progress_tramp;
            co.progress_user = &bp;
        }
        int rc = detail::rant_function_call(fn_, priv::to_c(req), &out, priv::to_ms(timeout), &co);
        if (rc == 1) {
            r.st_       = static_cast<CallStatus>(out.status);
            r.provider_ = out.provider;
            r.written_  = out.written_us;
            r.schema_   = out.schema;
            if (out.data.len) r.data_.assign(out.data.data, out.data.data + out.data.len);
        } else if (rc < 0) {
            r.ss_ = static_cast<SendStatus>(rc);   /* status() stays Timeout: not answered */
            if (r.ss_ == SendStatus::State)
                priv::raise_msg("rant: a blocking call from an inline callback, use call_async or put the handle on a queue");
        }
        if (rc >= 0 && out.message.len)   /* answered or timed out: copy the outcome text */
            r.message_.assign(out.message.data, out.message.len);
        return r;
    }
    /* Async form: returns once committed, with the call id for cancel(). on_progress fires
     * per update and on_response once, on the polling thread, which frees the call state. */
    TaskCall call_async(Bytes req, ProgressHandler on_progress,
                        std::function<void(const ResponseView<Bytes>&)> on_response,
                        const CallOptions& opts = {}) {
        TaskCall tc;
        if (!live()) return tc;
        priv::AsyncBox* box = new priv::AsyncBox{ std::move(on_response),
                                                  &impl_->reg_mu, &impl_->async_live,
                                                  std::move(on_progress), impl_.get() };
        {
            std::lock_guard<std::mutex> g(impl_->reg_mu);
            impl_->async_live.insert(box);
        }
        detail::RantCallOpts co; std::memset(&co, 0, sizeof co);
        co.provider = opts.provider;
        co.id_out   = &tc.id;
        if (box->on_progress) {
            co.on_progress   = &RemoteTask::async_progress_tramp;
            co.progress_user = box;
        }
        int rc = detail::rant_function_call_async(fn_, priv::to_c(req),
                                                  &RemoteTask::async_response_tramp, box, &co);
        if (rc != 0) {
            std::lock_guard<std::mutex> g(impl_->reg_mu);
            impl_->async_live.erase(box);
            delete box;
            tc.id = 0;
        }
        tc.status = static_cast<SendStatus>(rc);
        if (opts.id_out) *opts.id_out = tc.id;
        return tc;
    }

    /* Request cancellation of the call. Cooperative and never acked: the terminal status is
     * the answer. BadRole when the provider declared no_cancel, State when not pending. */
    SendStatus cancel(uint32_t call_id) {
        if (!live()) return SendStatus::NoTopic;
        return static_cast<SendStatus>(detail::rant_function_cancel(fn_, call_id));
    }

    /* definitions currently matched, 0 = nobody provides the name yet */
    int  match_count()    const { return live() ? detail::rant_function_match_count(fn_) : 0; }
    /* Retire the remote. Every outstanding call completes Cancelled. The handle is empty
     * after. State from its own inline callback, and it stays valid then. */
    SendStatus close() { return priv::close_function(fn_, impl_, owned_); }
    const std::string& name() const { return name_; }
    /* The schemas in use now, empty when untyped. A reflect_from_mesh handle reports what
     * it adopted. */
    Schema request_schema() const {
        detail::RantFunction* f = live();
        return Schema(f ? detail::rant_function_request_schema(f) : nullptr);
    }
    Schema response_schema() const {
        detail::RantFunction* f = live();
        return Schema(f ? detail::rant_function_response_schema(f) : nullptr);
    }
    Schema progress_schema() const {
        detail::RantFunction* f = live();
        return Schema(f ? detail::rant_function_progress_schema(f) : nullptr);
    }

private:
    /* a blocking call's progress handler, on the caller's stack for the call */
    struct BlockingProgress { ProgressHandler* h; priv::NodeImpl* impl; };
    static void blocking_progress_tramp(const detail::RantProgress* p) {
        BlockingProgress* b = static_cast<BlockingProgress*>(p->user);
        ProgressView<Bytes> pv(p);
        priv::guarded(b->impl, [&] { (*b->h)(pv); });
    }
    static void async_progress_tramp(const detail::RantProgress* p) {
        priv::AsyncBox* box = static_cast<priv::AsyncBox*>(p->user);
        ProgressView<Bytes> pv(p);
        priv::guarded(box->impl, [&] { box->on_progress(pv); });
    }
    static void async_response_tramp(const detail::RantResponse* r) {
        priv::AsyncBox* box = static_cast<priv::AsyncBox*>(r->user);
        {
            std::lock_guard<std::mutex> g(*box->mu);
            box->live->erase(box);
        }
        if (box->cb) {
            ResponseView<Bytes> rv(r);
            priv::guarded(box->impl, [&] { box->cb(rv); });
        }
        delete box;
    }
    detail::RantFunction* live() const noexcept { return impl_ && impl_->node ? fn_ : nullptr; }
    void move_from(RemoteTask& o) noexcept {
        fn_ = o.fn_; impl_ = std::move(o.impl_); name_ = std::move(o.name_); owned_ = o.owned_;
        o.fn_ = nullptr;
    }
    detail::RantFunction* fn_ = nullptr;
    std::shared_ptr<priv::NodeImpl> impl_;
    std::string name_;
    bool owned_ = true;   /* false for the node's own @rant/meta endpoint */
    template <class A, class B, class C> friend class RemoteTask;
    friend class Node;
};

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
    const detail::RantSchema* raw_schema() const { return u_->schema; }

private:
    friend class VariableDefinition<Bytes>;
    explicit VariableUpdate(const detail::RantVariableUpdate* u) : u_(u) {}
    const detail::RantVariableUpdate* u_;
};

/* VariableDefinition<Bytes> (untyped): this node holds the authoritative value. */
template <> class VariableDefinition<Bytes> {
public:
    VariableDefinition() = default;
    VariableDefinition(VariableDefinition&& o) noexcept { move_from(o); }
    VariableDefinition& operator=(VariableDefinition&& o) noexcept {
        if (this != &o) { (void)close(); move_from(o); }
        return *this;
    }
    VariableDefinition(const VariableDefinition&) = delete;
    VariableDefinition& operator=(const VariableDefinition&) = delete;
    ~VariableDefinition() {
        if (close() == SendStatus::State && impl_) impl_->report_close_refused(name_);
    }
    /* A reflect_from_mesh handle: re-type in place when the mesh moved (true = re-typed). */
    bool refresh() { return live() && detail::rant_variable_refresh(var_) == 1; }

    bool valid() const noexcept { return live() != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /* the current value, copied out under the node lock (nullopt = none yet) */
    std::optional<std::vector<uint8_t>> get() const {
        if (!live()) return std::nullopt;
        detail::rant_node_lock(node_);
        detail::RantBytes b;
        std::optional<std::vector<uint8_t>> out;
        if (detail::rant_variable_get(var_, &b) == 1) {
            out.emplace();
            if (b.len) out->assign(b.data, b.data + b.len);
        }
        detail::rant_node_unlock(node_);
        return out;
    }
    SendStatus set(Bytes value) {
        if (!live()) return SendStatus::NoTopic;
        return static_cast<SendStatus>(detail::rant_variable_set(var_, priv::to_c(value)));
    }
    /* Force the value: writes are absorbed into the shadow source until unforce, which
     * restores the latest absorbed set. Requires VariableOptions::allow_force: State on the
     * owner without it, BadRole on a remote whose owner advertises none. */
    SendStatus force(Bytes value) {
        if (!live()) return SendStatus::NoTopic;
        return static_cast<SendStatus>(detail::rant_variable_force(var_, priv::to_c(value)));
    }
    SendStatus unforce() {
        if (!live()) return SendStatus::NoTopic;
        return static_cast<SendStatus>(detail::rant_variable_unforce(var_));
    }
    bool forced() const { return live() && detail::rant_variable_forced(var_) == 1; }
    /* the handles matched to this one: remotes at a definition, the definition at a remote */
    int match_count() const { return live() ? detail::rant_variable_match_count(var_) : 0; }

    /* Observe. on_change replays the current value at registration and fires on every state
     * change, on_write on every applied write, both inline on the applying thread. {} clears. */
    void on_change(std::function<void(const VariableUpdate&)> h) { observe(std::move(h), true); }
    void on_write (std::function<void(const VariableUpdate&)> h) { observe(std::move(h), false); }

    /* Retire the variable and release the name for a successor. The handle is empty after.
     * State from its own inline callback, and the handle stays valid then. */
    SendStatus close() {
        if (!var_) return SendStatus::NoTopic;
        if (impl_ && impl_->node) {
            int rc = detail::rant_variable_retire(var_);
            if (rc != 0) return static_cast<SendStatus>(rc);
        }
        var_ = nullptr;
        impl_.reset();
        return SendStatus::Ok;
    }
    const std::string& name() const { return name_; }
    /* The schema in use now, empty when untyped. A reflect_from_mesh handle reports what it
     * adopted. */
    Schema schema() const {
        detail::RantVariable* v = live();
        return Schema(v ? detail::rant_variable_schema(v) : nullptr);
    }

protected:
    VariableDefinition(Node& n, std::string_view name, const Schema* schema,
                       const VariableOptions<Bytes>& o) {
        if (!n.valid()) { priv::raise_msg("rant::VariableDefinition: node is not valid"); return; }
        create(n, name, schema, o, /*definition=*/true);
    }
    struct UBox : priv::HandlerBox {
        std::function<void(const VariableUpdate&)> h;
        priv::NodeImpl* impl = nullptr;
    };
    static void utramp(const detail::RantVariableUpdate* u, void* user) {
        UBox* b = static_cast<UBox*>(user);
        VariableUpdate up(u);
        priv::guarded(b->impl, [&] { b->h(up); });
    }
    void observe(std::function<void(const VariableUpdate&)> h, bool change) {
        if (!live()) return;
        UBox* box = nullptr;
        if (h) { box = new UBox(); box->h = std::move(h); box->impl = impl_.get(); }
        (void)(change ? detail::rant_variable_on_change(var_, box ? &VariableDefinition::utramp : nullptr, box)
                      : detail::rant_variable_on_write (var_, box ? &VariableDefinition::utramp : nullptr, box));
        if (box) {   /* kept alive until node close, like every handler box */
            std::lock_guard<std::mutex> g(impl_->reg_mu);
            impl_->boxes.emplace_back(box);
        }
    }
    void create(Node& n, std::string_view name, const Schema* schema,
                const VariableOptions<Bytes>& o, bool definition) {
        std::string nm(name);
        name_ = nm;
        detail::RantVariableOpts co;
        std::memset(&co, 0, sizeof co);
        co.initial     = priv::to_c(o.initial);
        co.access      = o.read_only ? detail::RANT_VAR_READONLY : detail::RANT_VAR_READWRITE;
        co.allow_force = o.allow_force ? 1 : 0;
        co.catch_up    = o.catch_up;
        co.keep_last   = o.keep_last;
        co.backpressure_wait_us = priv::to_us(o.backpressure_wait);
        co.reflect_from_mesh = o.reflect_from_mesh ? 1 : 0;
        co.queue = n.impl_->queue_for(o.queue);
        node_ = n.impl_->node;
        impl_ = n.impl_;
        var_ = definition
            ? detail::rant_node_create_variable_definition(node_, nm.c_str(),
                  schema ? schema->raw() : nullptr, &co)
            : detail::rant_node_create_remote_variable(node_, nm.c_str(),
                  schema ? schema->raw() : nullptr, &co);
        if (!var_) priv::raise_last(node_, definition ? "rant::VariableDefinition create"
                                                      : "rant::RemoteVariable create");
    }
    detail::RantVariable* live() const noexcept { return impl_ && impl_->node ? var_ : nullptr; }
    void move_from(VariableDefinition& o) noexcept {
        var_ = o.var_; node_ = o.node_; impl_ = std::move(o.impl_); name_ = std::move(o.name_);
        o.var_ = nullptr;
    }
    detail::RantVariable* var_ = nullptr;
    detail::RantNode*       node_ = nullptr;
    std::shared_ptr<priv::NodeImpl> impl_;
    std::string             name_;
    template <class A> friend class VariableDefinition;
    friend class Node;
};

/* The untyped accessor of a value owned elsewhere: reads see the cached latest, writes go
 * over the set channel with no response. */
template <> class RemoteVariable<Bytes> : public VariableDefinition<Bytes> {
public:
    RemoteVariable() = default;
    /* Block (driving the node loop) until a value exists or timeout_ms elapses. */
    bool wait(std::chrono::milliseconds timeout) {
        return live() && detail::rant_variable_wait(var_, priv::to_ms(timeout)) == 1;
    }

private:
    RemoteVariable(Node& n, std::string_view name, const Schema* schema,
                   const VariableOptions<Bytes>& o) {
        if (!n.valid()) { priv::raise_msg("rant::RemoteVariable: node is not valid"); return; }
        create(n, name, schema, o, /*definition=*/false);
    }
    template <class A> friend class RemoteVariable;
    friend class Node;
};

#endif /* !RANT_NO_PATTERNS */

/* ====================== PUB/SUB (raw cores) ================================ */

/* Publisher<Bytes>: the raw publish side. schema is the DSL schema the bytes follow, or
 * null for untyped bytes. Send a MessageBuilder or any Bytes. */
template <> class Publisher<Bytes> {
public:
    Publisher() = default;

    bool valid() const noexcept { return t_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /* capture is when the data was true, as against when it was sent. The default is
     * unstated, which costs no wire bytes. */
    SendStatus send(Bytes data, types::Timestamp capture = {}) { return t_.send(data, capture); }
    /* subscribers currently matched */
    int  match_count()      const { return t_.match_count(); }
    /* true when a send would not wait: a subscriber is matched or matching has converged */
    bool ready()            const { return t_.ready(); }
    /* wait until every reader has acked, the flush before close. false = timeout passed */
    bool drain(std::chrono::milliseconds timeout) { return t_.drain(timeout); }
    /* drop this handle now instead of at scope end, see TopicCore::close */
    SendStatus close()            { return t_.close(); }
    /* a reflect_from_mesh publisher: re type in place when the mesh moved, true = re typed */
    bool        refresh()         { return t_.refresh(); }
    Schema      schema() const    { return t_.schema(); }
    std::string name() const      { return t_.name(); }

private:
    Publisher(Node& n, std::string_view name, const Schema* schema, const Qos& qos)
        : t_(n, name, Role::PubOnly, schema, qos) {}
    priv::TopicCore t_;
    template <class A> friend class Publisher;
    friend class Node;
};

/* Subscriber<Bytes>: the raw subscribe side. The handler fires per message on the polling
 * thread, or at dispatch() of its queue, with a MessageView that reads fields by name
 * through the schema. Made without a handler it is pulled with take() and take_latest(). */
template <> class Subscriber<Bytes> {
public:
    Subscriber() = default;

    /* The oldest waiting message of a pulled subscriber, or nullopt when none arrived within
     * timeout_ms (0 = check, negative = forever). The view lives until the next take. */
    std::optional<MessageView> take(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        return pull(timeout, false);
    }
    /* The newest waiting message, dropping the older ones. As take(). */
    std::optional<MessageView> take_latest(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        return pull(timeout, true);
    }

    bool valid() const noexcept { return t_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /* publishers currently matched */
    int  match_count()  const { return t_.match_count(); }
    /* drop this handle now instead of at scope end, see TopicCore::close */
    SendStatus close()        { return t_.close(); }
    /* a reflect_from_mesh subscriber: re type in place when the mesh moved, true = re typed */
    bool        refresh()      { return t_.refresh(); }
    Schema      schema() const { return t_.schema(); }
    std::string name() const   { return t_.name(); }

private:
    Subscriber(Node& n, std::string_view name, const Schema* schema,
               Node::MessageHandler on_message, const Qos& qos)
        : t_(on_message ? priv::TopicCore(n, name, Role::SubOnly, schema, qos) : priv::TopicCore()) {
        if (!on_message) { priv::raise_msg("rant::Subscriber: the handler is empty, make a pulled subscriber without one"); return; }
        t_.add_handler(std::move(on_message));
    }
    /* A pulled subscriber: messages wait until take() or take_latest() reads them. */
    Subscriber(Node& n, std::string_view name, const Schema* schema, const Qos& qos)
        : t_(priv::TopicCore::pulled(n, name, schema, qos)) {}
    std::optional<MessageView> pull(std::chrono::milliseconds timeout, bool latest) {
        int r = t_.take(&msg_, priv::to_ms(timeout), latest);
        if (r == static_cast<int>(SendStatus::State))
            priv::raise_msg("rant::Subscriber::take: this subscriber has a handler, take needs one made without");
        if (r != 1) return std::nullopt;
        return MessageView(&msg_);
    }
    priv::TopicCore t_;
    detail::RantMsg msg_{};   /* the last take, which the returned view points at */
    template <class A> friend class Subscriber;
    friend class Node;
};

/* Typed sugar: thin template layers over the Bytes cores using the RANT_SCHEMA codec.
 * Each owns its core, so it is move only and releases at scope end. */

#ifndef RANT_NO_PATTERNS

/* The typed view of a request inside a full form function handler. Wraps the untyped
 * Request<Bytes> for the callback lifetime and adds the typed reply. */
template <class Rsp> class Request {
public:
    Request(Request<Bytes>& core, priv::TypeCodec* rsp) : core_(core), rsp_(rsp) {}
    Bytes            data()          const { return core_.data(); }
    uint32_t         caller()        const { return core_.caller(); }
    std::string_view caller_name()   const { return core_.caller_name(); }
    std::string_view function_name() const { return core_.function_name(); }
    uint64_t         recv_us()       const { return core_.recv_us(); }
    uint64_t         written_us()       const { return core_.written_us(); }

    /* A reply that does not encode answers AppError, never an empty OK. */
    void reply(const Rsp& v) {
        std::vector<uint8_t> s;
        if (auto b = priv::encode(*rsp_, v, s)) core_.reply(*b);
        else core_.fail("the reply did not encode into the response schema");
    }
    void reply(Bytes raw)     { core_.reply(raw); }
    void fail(std::string_view message = {}, Bytes raw = {}) { core_.fail(message, raw); }
    Deferred<Rsp> defer()     { return Deferred<Rsp>(core_.defer(), rsp_); }

private:
    Request<Bytes>& core_;
    priv::TypeCodec* rsp_;
};

/* Deferred<Rsp>: the typed parked reply. */
template <class Rsp> class Deferred {
public:
    Deferred() = default;
    Deferred(Deferred<Bytes>&& core, priv::TypeCodec* rsp) : core_(std::move(core)), rsp_(rsp) {}
    Deferred(Deferred&&) noexcept = default;
    Deferred& operator=(Deferred&&) noexcept = default;
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }
    /* A value that does not encode answers AppError and returns false. */
    bool complete(const Rsp& v, std::string_view message = {}) {
        std::vector<uint8_t> s;
        auto b = priv::encode(*rsp_, v, s);
        if (!b) { (void)core_.fail("the reply did not encode into the response schema"); return false; }
        return core_.complete(*b, message);
    }
    bool fail(std::string_view message = {}) { return core_.fail(message); }
private:
    Deferred<Bytes> core_;
    priv::TypeCodec* rsp_ = nullptr;
};

/* Response<Rsp>: the owning typed call outcome. if (r) tests for Ok, and value() holds the
 * payload whenever one came back, so a cancelled task's partial result reads too. */
template <class Rsp> class Response {
public:
    Response() = default;
    CallStatus status()      const { return st_; }
    bool       ok()          const { return st_ == CallStatus::Ok; }
    explicit operator bool() const { return ok(); }
    uint32_t   provider()    const { return provider_; }
    SendStatus send_status() const { return ss_; }
    uint64_t   written_us()     const { return written_; }
    /* human-readable outcome text (owned): see Response<Bytes>::message */
    std::string_view message() const { return message_; }
    /* The decoded payload: the reply on Ok, a partial result or failure data otherwise.
     * Empty when none came back, or when it did not decode, which an error event reports. */
    const std::optional<Rsp>& value() const { return v_; }
private:
    CallStatus  st_ = CallStatus::Timeout;
    SendStatus  ss_ = SendStatus::Ok;
    uint32_t    provider_ = 0;
    uint64_t    written_ = 0;
    std::string message_;
    std::optional<Rsp> v_;
    template <class A, class B> friend class RemoteFunction;
    template <class A, class B, class C> friend class RemoteTask;
};

/* The typed async outcome, valid for the callback. As Response. */
template <class Rsp> class ResponseView {
public:
    CallStatus status()     const { return st_; }
    bool       ok()         const { return st_ == CallStatus::Ok; }
    explicit operator bool() const { return ok(); }
    uint32_t   provider()   const { return provider_; }
    uint64_t   written_us()    const { return written_; }
    /* human-readable outcome text (a view, callback lifetime): see ResponseView<Bytes>::message */
    std::string_view message() const { return message_; }
    /* The decoded payload whenever one came back, as Response::value. */
    const std::optional<Rsp>& value() const { return v_; }
private:
    ResponseView() = default;
    CallStatus       st_ = CallStatus::Timeout;
    uint32_t         provider_ = 0;
    uint64_t         written_ = 0;
    std::string_view message_;
    std::optional<Rsp> v_;
    template <class A, class B> friend class RemoteFunction;
    template <class A, class B, class C> friend class RemoteTask;
};

/* The typed implementation side. Handler forms: Rsp(const Req&), the return value is the
 * reply, or void(const Req&, Request<Rsp>&). A throwing handler answers AppError. */
template <class Req, class Rsp> class FunctionDefinition {
public:
    FunctionDefinition() = default;
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }
    int match_count() const { return core_.match_count(); }
    SendStatus close() { return core_.close(); }
    const std::string& name() const { return core_.name(); }
    Schema request_schema() const   { return core_.request_schema(); }
    Schema response_schema() const  { return core_.response_schema(); }

private:
    template <class H>
    FunctionDefinition(Node& n, std::string_view name, H&& handler, const FunctionOptions& o) {
        priv::TypeCodec* cq = priv::type_codec<Req>(n);
        priv::TypeCodec* cr = priv::type_codec<Rsp>(n);
        if (!priv::codec_ok(cq) || !priv::codec_ok(cr)) { priv::raise_msg("rant::FunctionDefinition: RANT_SCHEMA compile failed"); return; }
        Schema rq = cq->schema(), rs = cr->schema();
        core_ = FunctionDefinition<Bytes, Bytes>(n, name, &rq, &rs, adapt(std::forward<H>(handler), cq, cr), o);
    }
    friend class Node;
    template <class H>
    static typename FunctionDefinition<Bytes, Bytes>::Handler adapt(H&& h, priv::TypeCodec* cq,
                                                                    priv::TypeCodec* cr) {
        if constexpr (std::is_invocable_v<std::decay_t<H>&, const Req&, Request<Rsp>&>) {
            return [f = std::forward<H>(h), cq, cr](Request<Bytes>& u) mutable {
                Req q{};
                if (!priv::decode(*cq, q, u.data(), u.raw_schema())) { u.fail("request decode failed"); return; }
                Request<Rsp> tr(u, cr);
                f(q, tr);
            };
        } else if constexpr (std::is_invocable_v<std::decay_t<H>&, const Req&>) {
            static_assert(std::is_convertible_v<
                              std::invoke_result_t<std::decay_t<H>&, const Req&>, Rsp>,
                          "simple function handler must return Rsp");
            return [f = std::forward<H>(h), cq, cr](Request<Bytes>& u) mutable {
                Req q{};
                if (!priv::decode(*cq, q, u.data(), u.raw_schema())) { u.fail("request decode failed"); return; }
                Rsp r = f(q);
                std::vector<uint8_t> s;
                if (auto b = priv::encode(*cr, r, s)) u.reply(*b);
                else u.fail("the reply did not encode into the response schema");
            };
        } else {
            static_assert(priv::always_false<H>,
                "function handler must be Rsp(const Req&) or void(const Req&, rant::Request<Rsp>&)");
            return {};
        }
    }
    FunctionDefinition<Bytes, Bytes> core_;
};

/* RemoteFunction<Req,Rsp>: the typed caller side. */
template <class Req, class Rsp> class RemoteFunction {
public:
    RemoteFunction() = default;
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /* Blocking call, see RemoteFunction<Bytes, Bytes>::call. Decodes into the owning Response. */
    Response<Rsp> call(const Req& req, std::optional<std::chrono::milliseconds> timeout = std::nullopt, const CallOptions& opts = {}) {
        std::vector<uint8_t> s;
        Response<Rsp> r;
        auto b = priv::encode(*cq_, req, s);
        if (!b) { r.ss_ = SendStatus::Schema; r.message_ = "the request did not encode"; return r; }
        Response<Bytes> ur = core_.call(*b, timeout, opts);
        r.st_ = ur.status(); r.ss_ = ur.send_status(); r.provider_ = ur.provider();
        r.written_ = ur.written_us();
        r.message_.assign(ur.message());   /* own it: ur dies with this frame */
        r.v_ = priv::decode_payload<Rsp>(*cr_, ur.data(), ur.raw_schema(), core_.impl_.get(),
                                         core_.name_, "a response did not decode into Rsp");
        return r;
    }
    SendStatus call_async(const Req& req, std::function<void(const ResponseView<Rsp>&)> cb,
                          const CallOptions& opts = {}) {
        std::vector<uint8_t> s;
        auto b = priv::encode(*cq_, req, s);
        if (!b) return SendStatus::Schema;
        return core_.call_async(*b,
            [cb = std::move(cb), cr = cr_, impl = core_.impl_.get(), nm = core_.name_](const ResponseView<Bytes>& uv) {
                ResponseView<Rsp> tv;
                tv.st_ = uv.status(); tv.provider_ = uv.provider(); tv.written_ = uv.written_us();
                tv.message_ = uv.message();
                tv.v_ = priv::decode_payload<Rsp>(*cr, uv.data(), uv.raw_schema(), impl, nm,
                                                  "a response did not decode into Rsp");
                cb(tv);
            }, opts);
    }

    int  match_count()    const { return core_.match_count(); }
    SendStatus close() { return core_.close(); }
    const std::string& name() const { return core_.name(); }
    Schema request_schema() const   { return core_.request_schema(); }
    Schema response_schema() const  { return core_.response_schema(); }

private:
    RemoteFunction(Node& n, std::string_view name, const FunctionOptions& o) {
        cq_ = priv::type_codec<Req>(n);
        cr_ = priv::type_codec<Rsp>(n);
        if (!priv::codec_ok(cq_) || !priv::codec_ok(cr_)) { priv::raise_msg("rant::RemoteFunction: RANT_SCHEMA compile failed"); return; }
        Schema rq = cq_->schema(), rs = cr_->schema();
        core_ = RemoteFunction<Bytes, Bytes>(n, name, &rq, &rs, o);
    }
    friend class Node;
    RemoteFunction<Bytes, Bytes> core_;
    priv::TypeCodec* cq_ = nullptr;
    priv::TypeCodec* cr_ = nullptr;
};

/* The typed view of a request inside a task handler. Wraps the untyped TaskRequest<Bytes, Bytes> for
 * the callback lifetime and adds the typed verbs. */
template <class Prg, class Rsp> class TaskRequest {
public:
    TaskRequest(TaskRequest<Bytes, Bytes>& core, priv::TypeCodec* prg, priv::TypeCodec* rsp)
        : core_(core), prg_(prg), rsp_(rsp) {}
    Bytes            data()        const { return core_.data(); }
    uint32_t         caller()      const { return core_.caller(); }
    std::string_view caller_name() const { return core_.caller_name(); }
    std::string_view task_name()   const { return core_.task_name(); }
    uint64_t         recv_us()     const { return core_.recv_us(); }
    uint64_t         written_us()  const { return core_.written_us(); }

    /* A reply that does not encode answers AppError, never an empty OK. */
    void reply(const Rsp& v) {
        std::vector<uint8_t> s;
        if (auto b = priv::encode(*rsp_, v, s)) core_.reply(*b);
        else core_.fail("the reply did not encode into the response schema");
    }
    void reply(Bytes raw) { core_.reply(raw); }
    void fail(std::string_view message = {}, Bytes raw = {}) { core_.fail(message, raw); }
    SendStatus start() { return core_.start(); }
    PendingTask<Prg, Rsp> defer() { return PendingTask<Prg, Rsp>(core_.defer(), prg_, rsp_); }

private:
    TaskRequest<Bytes, Bytes>& core_;
    priv::TypeCodec* prg_;
    priv::TypeCodec* rsp_;
};

/* PendingTask<Prg,Rsp>: the typed deferred task (see the untyped core's contract). */
template <class Prg, class Rsp> class PendingTask {
public:
    PendingTask() = default;
    PendingTask(PendingTask<Bytes, Bytes>&& core, priv::TypeCodec* prg, priv::TypeCodec* rsp)
        : core_(std::move(core)), prg_(prg), rsp_(rsp) {}
    PendingTask(PendingTask&&) noexcept = default;
    PendingTask& operator=(PendingTask&&) noexcept = default;
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    SendStatus progress(const Prg& v) {
        std::vector<uint8_t> s;
        auto b = priv::encode(*prg_, v, s);
        return b ? core_.progress(*b) : SendStatus::Schema;
    }
    bool cancelled() const { return core_.cancelled(); }
    /* A result that does not encode answers AppError, so the caller is not left waiting,
     * and returns Schema. */
    SendStatus complete(const Rsp& v, std::string_view message = {}) {
        std::vector<uint8_t> s;
        auto b = priv::encode(*rsp_, v, s);
        if (!b) { (void)core_.fail("the result did not encode into the response schema"); return SendStatus::Schema; }
        return core_.complete(*b, message);
    }
    SendStatus fail(std::string_view message = {}) { return core_.fail(message); }
    SendStatus complete_cancelled(std::string_view message = {}) { return core_.complete_cancelled(message); }
    /* honor a cancel and still carry a typed partial result */
    SendStatus complete_cancelled(std::string_view message, const Rsp& partial) {
        std::vector<uint8_t> s;
        auto b = priv::encode(*rsp_, partial, s);
        if (!b) { (void)core_.complete_cancelled(message); return SendStatus::Schema; }
        return core_.complete_cancelled(message, *b);
    }

private:
    PendingTask<Bytes, Bytes> core_;
    priv::TypeCodec* prg_ = nullptr;
    priv::TypeCodec* rsp_ = nullptr;
};

/* The typed progress update, valid for the callback. value() is meaningful only when
 * has_value(), the RUNNING ack carries none. */
template <class Prg> class ProgressView {
public:
    uint32_t call_id()    const { return call_id_; }
    uint32_t provider()   const { return provider_; }
    uint64_t written_us() const { return written_; }
    uint64_t recv_us()    const { return recv_; }
    bool     has_value()  const { return has_; }
    const Prg& value()      const { return v_; }
    const Prg& operator*()  const { return v_; }
    const Prg* operator->() const { return &v_; }

private:
    ProgressView() = default;
    uint32_t call_id_ = 0, provider_ = 0;
    uint64_t written_ = 0, recv_ = 0;
    bool     has_ = false;
    Prg      v_{};
    template <class A, class B, class C> friend class RemoteTask;
};

/* The typed implementation side. Handler form: void(const Req&, TaskRequest<Prg,Rsp>&),
 * answering inline or through start() and defer(). A throwing handler answers AppError. */
template <class Req, class Prg, class Rsp> class TaskDefinition {
public:
    TaskDefinition() = default;
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }
    int match_count() const { return core_.match_count(); }
    void on_cancel(std::function<void(uint64_t token)> h) { core_.on_cancel(std::move(h)); }
    SendStatus close() { return core_.close(); }
    const std::string& name() const { return core_.name(); }
    Schema request_schema() const   { return core_.request_schema(); }
    Schema progress_schema() const  { return core_.progress_schema(); }
    Schema response_schema() const  { return core_.response_schema(); }

private:
    template <class H>
    TaskDefinition(Node& n, std::string_view name, H&& handler, const TaskOptions& o) {
        priv::TypeCodec* cq = priv::type_codec<Req>(n);
        priv::TypeCodec* cp = priv::type_codec<Prg>(n);
        priv::TypeCodec* cr = priv::type_codec<Rsp>(n);
        if (!priv::codec_ok(cq) || !priv::codec_ok(cp) || !priv::codec_ok(cr)) { priv::raise_msg("rant::TaskDefinition: RANT_SCHEMA compile failed"); return; }
        Schema rq = cq->schema(), pg = cp->schema(), rs = cr->schema();
        core_ = TaskDefinition<Bytes, Bytes, Bytes>(n, name, &rq, &pg, &rs, adapt(std::forward<H>(handler), cq, cp, cr), o);
    }
    friend class Node;
    template <class H>
    static typename TaskDefinition<Bytes, Bytes, Bytes>::Handler adapt(H&& h, priv::TypeCodec* cq,
                                                                       priv::TypeCodec* cp, priv::TypeCodec* cr) {
        static_assert(std::is_invocable_v<std::decay_t<H>&, const Req&, TaskRequest<Prg, Rsp>&>,
                      "task handler must be void(const Req&, rant::TaskRequest<Prg,Rsp>&)");
        return [f = std::forward<H>(h), cq, cp, cr](TaskRequest<Bytes, Bytes>& u) mutable {
            Req q{};
            if (!priv::decode(*cq, q, u.data(), u.raw_schema())) { u.fail("request decode failed"); return; }
            TaskRequest<Prg, Rsp> tr(u, cp, cr);
            f(q, tr);
        };
    }
    TaskDefinition<Bytes, Bytes, Bytes> core_;
};

/* RemoteTask<Req,Prg,Rsp>: the typed caller side. */
template <class Req, class Prg, class Rsp> class RemoteTask {
public:
    using ProgressHandler = std::function<void(const ProgressView<Prg>&)>;

    RemoteTask() = default;
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /* Blocking call, see RemoteTask<Bytes, Bytes, Bytes>::call. on_progress fires typed while it waits. */
    Response<Rsp> call(const Req& req, ProgressHandler on_progress = {},
                       std::optional<std::chrono::milliseconds> timeout = std::nullopt, const CallOptions& opts = {}) {
        std::vector<uint8_t> s;
        Response<Rsp> r;
        auto b = priv::encode(*cq_, req, s);
        if (!b) { r.ss_ = SendStatus::Schema; r.message_ = "the request did not encode"; return r; }
        Response<Bytes> ur = core_.call(*b, adapt_progress(std::move(on_progress)), timeout, opts);
        r.st_ = ur.status(); r.ss_ = ur.send_status(); r.provider_ = ur.provider();
        r.written_ = ur.written_us();
        r.message_.assign(ur.message());   /* own it: ur dies with this frame */
        r.v_ = priv::decode_payload<Rsp>(*cr_, ur.data(), ur.raw_schema(), core_.impl_.get(),
                                         core_.name_, "a response did not decode into Rsp");
        return r;
    }
    /* Async form (see RemoteTask<Bytes, Bytes, Bytes>::call_async): returns the send status + call id. */
    TaskCall call_async(const Req& req, ProgressHandler on_progress,
                        std::function<void(const ResponseView<Rsp>&)> on_response,
                        const CallOptions& opts = {}) {
        std::vector<uint8_t> s;
        auto b = priv::encode(*cq_, req, s);
        if (!b) return TaskCall{ SendStatus::Schema, 0 };
        return core_.call_async(*b, adapt_progress(std::move(on_progress)),
            [cb = std::move(on_response), cr = cr_, impl = core_.impl_.get(), nm = core_.name_](const ResponseView<Bytes>& uv) {
                if (!cb) return;
                ResponseView<Rsp> tv;
                tv.st_ = uv.status(); tv.provider_ = uv.provider(); tv.written_ = uv.written_us();
                tv.message_ = uv.message();
                tv.v_ = priv::decode_payload<Rsp>(*cr, uv.data(), uv.raw_schema(), impl, nm,
                                                  "a response did not decode into Rsp");
                cb(tv);
            }, opts);
    }
    /* Cancel the outstanding call (see RemoteTask<Bytes, Bytes, Bytes>::cancel). */
    SendStatus cancel(uint32_t call_id) { return core_.cancel(call_id); }

    int  match_count()    const { return core_.match_count(); }
    SendStatus close() { return core_.close(); }
    const std::string& name() const { return core_.name(); }
    Schema request_schema() const   { return core_.request_schema(); }
    Schema progress_schema() const  { return core_.progress_schema(); }
    Schema response_schema() const  { return core_.response_schema(); }

private:
    RemoteTask(Node& n, std::string_view name, const TaskOptions& o) {
        cq_ = priv::type_codec<Req>(n);
        cp_ = priv::type_codec<Prg>(n);
        cr_ = priv::type_codec<Rsp>(n);
        if (!priv::codec_ok(cq_) || !priv::codec_ok(cp_) || !priv::codec_ok(cr_)) { priv::raise_msg("rant::RemoteTask: RANT_SCHEMA compile failed"); return; }
        Schema rq = cq_->schema(), pg = cp_->schema(), rs = cr_->schema();
        core_ = RemoteTask<Bytes, Bytes, Bytes>(n, name, &rq, &pg, &rs, o);
    }
    friend class Node;
    RemoteTask<Bytes, Bytes, Bytes>::ProgressHandler adapt_progress(ProgressHandler h) {
        if (!h) return {};
        return [f = std::move(h), cp = cp_, impl = core_.impl_.get(), nm = core_.name_]
               (const ProgressView<Bytes>& up) mutable {
            ProgressView<Prg> tp;
            tp.call_id_ = up.call_id(); tp.provider_ = up.provider();
            tp.written_ = up.written_us(); tp.recv_ = up.recv_us();
            tp.has_ = up.has_value();
            if (tp.has_ && !priv::decode(*cp, tp.v_, up.data(), up.raw_schema())) {
                if (impl) impl->report_decode(nm, "a progress update did not decode into Prg");
                return;
            }
            f(tp);
        };
    }
    RemoteTask<Bytes, Bytes, Bytes> core_;
    priv::TypeCodec* cq_ = nullptr;
    priv::TypeCodec* cp_ = nullptr;
    priv::TypeCodec* cr_ = nullptr;
};

/* VariableDefinition<T>: the typed authoritative value. */
template <class T> class VariableDefinition {
public:
    VariableDefinition() = default;
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /* the current value BY VALUE, decoded under the node lock (nullopt = none yet) */
    std::optional<T> get() const {
        auto b = core_.get();
        if (!b) return std::nullopt;
        return priv::decode_payload<T>(*c_, Bytes(b->data(), b->size()), nullptr, core_.impl_.get(),
                                       core_.name_, "the variable's value did not decode into T");
    }
    SendStatus set(const T& v) {
        thread_local std::vector<uint8_t> s;
        auto b = priv::encode(*c_, v, s);
        return b ? core_.set(*b) : SendStatus::Schema;
    }
    SendStatus force(const T& v) {
        thread_local std::vector<uint8_t> s;
        auto b = priv::encode(*c_, v, s);
        return b ? core_.force(*b) : SendStatus::Schema;
    }
    SendStatus unforce()         { return core_.unforce(); }
    bool forced()          const { return core_.forced(); }
    int  match_count()     const { return core_.match_count(); }

    /* Observe, see the untyped core for the contract. Handler forms: void(const T&) or
     * void(const T&, const VariableUpdate&). nullptr clears. */
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
    SendStatus close() { return core_.close(); }
    const std::string& name() const { return core_.name(); }
    Schema schema() const { return core_.schema(); }

private:
    VariableDefinition(Node& n, std::string_view name, const VariableOptions<T>& o) {
        c_ = priv::type_codec<T>(n);
        if (!priv::codec_ok(c_)) { priv::raise_msg("rant::VariableDefinition: RANT_SCHEMA compile failed"); return; }
        Schema sc = c_->schema();
        std::vector<uint8_t> scratch;
        VariableOptions<Bytes> uo;
        if (o.initial) {
            auto b = priv::encode(*c_, *o.initial, scratch);
            if (!b) { priv::raise_msg("rant::VariableDefinition: the initial value did not encode"); return; }
            uo.initial = *b;
        }
        uo.read_only = o.read_only; uo.allow_force = o.allow_force;
        uo.catch_up = o.catch_up; uo.keep_last = o.keep_last;
        uo.backpressure_wait = o.backpressure_wait; uo.queue = o.queue;
        core_ = VariableDefinition<Bytes>(n, name, &sc, uo);
    }
    friend class Node;
    template <class H>
    std::function<void(const VariableUpdate&)> adapt(H&& h) {
        return [f = std::forward<H>(h), c = c_, impl = core_.impl_.get(), nm = core_.name_]
               (const VariableUpdate& u) mutable {
            T v{};
            if (!priv::decode(*c, v, u.value(), u.raw_schema())) {
                if (impl) impl->report_decode(nm, "a variable update did not decode into T");
                return;
            }
            if constexpr (std::is_invocable_v<std::decay_t<H>&, const T&, const VariableUpdate&>)
                f(v, u);
            else
                f(v);
        };
    }
    VariableDefinition<Bytes> core_;
    priv::TypeCodec* c_ = nullptr;
};

/* RemoteVariable<T>: the typed accessor of a value owned elsewhere. */
template <class T> class RemoteVariable {
public:
    RemoteVariable() = default;
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    std::optional<T> get() const {
        auto b = core_.get();
        if (!b) return std::nullopt;
        return priv::decode_payload<T>(*c_, Bytes(b->data(), b->size()), nullptr, core_.impl_.get(),
                                       core_.name_, "the variable's value did not decode into T");
    }
    /* Block (driving the node loop) until a value exists or timeout_ms elapses. */
    bool wait(std::chrono::milliseconds timeout) { return core_.wait(timeout); }
    SendStatus set(const T& v) {
        thread_local std::vector<uint8_t> s;
        auto b = priv::encode(*c_, v, s);
        return b ? core_.set(*b) : SendStatus::Schema;
    }
    SendStatus force(const T& v) {
        thread_local std::vector<uint8_t> s;
        auto b = priv::encode(*c_, v, s);
        return b ? core_.force(*b) : SendStatus::Schema;
    }
    SendStatus unforce()         { return core_.unforce(); }
    bool forced()          const { return core_.forced(); }
    int  match_count()     const { return core_.match_count(); }

    /* Observe, see the untyped core for the contract. Handler forms: void(const T&) or
     * void(const T&, const VariableUpdate&). nullptr clears. */
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
    SendStatus close() { return core_.close(); }
    const std::string& name() const { return core_.name(); }
    Schema schema() const { return core_.schema(); }

private:
    RemoteVariable(Node& n, std::string_view name, const VariableOptions<T>& o) {
        c_ = priv::type_codec<T>(n);
        if (!priv::codec_ok(c_)) { priv::raise_msg("rant::RemoteVariable: RANT_SCHEMA compile failed"); return; }
        Schema sc = c_->schema();
        VariableOptions<Bytes> uo;
        uo.catch_up = o.catch_up; uo.keep_last = o.keep_last;
        uo.backpressure_wait = o.backpressure_wait; uo.queue = o.queue;
        core_ = RemoteVariable<Bytes>(n, name, &sc, uo);
    }
    friend class Node;
    template <class H>
    std::function<void(const VariableUpdate&)> adapt(H&& h) {
        return [f = std::forward<H>(h), c = c_, impl = core_.impl_.get(), nm = core_.name_]
               (const VariableUpdate& u) mutable {
            T v{};
            if (!priv::decode(*c, v, u.value(), u.raw_schema())) {
                if (impl) impl->report_decode(nm, "a variable update did not decode into T");
                return;
            }
            if constexpr (std::is_invocable_v<std::decay_t<H>&, const T&, const VariableUpdate&>)
                f(v, u);
            else
                f(v);
        };
    }
    RemoteVariable<Bytes> core_;
    priv::TypeCodec* c_ = nullptr;
};

#endif /* !RANT_NO_PATTERNS */

/* Publisher<T>: the typed publish side. */
template <class T> class Publisher {
public:
    Publisher() = default;
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    SendStatus send(const T& v, types::Timestamp capture = {}) {
        thread_local std::vector<uint8_t> s;   /* reused: only the non memcpy path fills it */
        auto b = priv::encode(*c_, v, s);
        return b ? core_.send(*b, capture) : SendStatus::Schema;
    }
    int  match_count()   const { return core_.match_count(); }
    bool ready()         const { return core_.ready(); }
    bool drain(std::chrono::milliseconds timeout) { return core_.drain(timeout); }
    SendStatus close()        { return core_.close(); }
    Schema      schema() const { return core_.schema(); }
    std::string name() const   { return core_.name(); }

private:
    Publisher(Node& n, std::string_view name, const Qos& qos) {
        c_ = priv::type_codec<T>(n);
        if (!priv::codec_ok(c_)) { priv::raise_msg("rant::Publisher: RANT_SCHEMA compile failed"); return; }
        Schema sc = c_->schema();
        core_ = Publisher<Bytes>(n, name, &sc, qos);
    }
    friend class Node;
    Publisher<Bytes> core_;
    priv::TypeCodec* c_ = nullptr;
};

/* The typed subscribe side. Handler forms: void(const T&) or void(const T&, const
 * MessageView&), fired per message on the polling thread with the decoded value. */
template <class T> class Subscriber {
public:
    Subscriber() = default;
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /* The oldest waiting message of a pulled subscriber, decoded, or nullopt when none
     * arrived within timeout_ms (0 = check, negative = forever). */
    std::optional<T> take(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        return decoded(core_.take(timeout));
    }
    /* The newest waiting message, dropping the older ones. As take(). */
    std::optional<T> take_latest(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
        return decoded(core_.take_latest(timeout));
    }

    int  match_count() const { return core_.match_count(); }
    SendStatus close()      { return core_.close(); }
    Schema      schema() const { return core_.schema(); }
    std::string name() const   { return core_.name(); }

private:
    template <class H>
    Subscriber(Node& n, std::string_view name, H&& handler, const Qos& qos) {
        c_ = priv::type_codec<T>(n);
        if (!priv::codec_ok(c_)) { priv::raise_msg("rant::Subscriber: RANT_SCHEMA compile failed"); return; }
        Schema sc = c_->schema();
        core_ = Subscriber<Bytes>(n, name, &sc,
                                  adapt(std::forward<H>(handler), c_, n.impl_.get(), std::string(name)), qos);
    }
    /* A pulled subscriber: messages wait until take() or take_latest() reads them. */
    Subscriber(Node& n, std::string_view name, const Qos& qos) {
        c_ = priv::type_codec<T>(n);
        if (!priv::codec_ok(c_)) { priv::raise_msg("rant::Subscriber: RANT_SCHEMA compile failed"); return; }
        Schema sc = c_->schema();
        core_ = Subscriber<Bytes>(n, name, &sc, qos);
    }
    friend class Node;
    std::optional<T> decoded(const std::optional<MessageView>& m) {
        if (!m) return std::nullopt;
        T v{};
        if (!priv::decode(*c_, v, m->data(), m->raw_schema())) {
            priv::raise_msg("rant::Subscriber::take: the message did not decode as T");
            return std::nullopt;
        }
        return v;
    }
    template <class H>
    static Node::MessageHandler adapt(H&& h, priv::TypeCodec* c, priv::NodeImpl* impl, std::string nm) {
        return [f = std::forward<H>(h), c, impl, nm = std::move(nm)](const MessageView& m) mutable {
            T v{};
            if (!priv::decode(*c, v, m.data(), m.raw_schema())) {
                impl->report_decode(nm, "a message did not decode into the subscriber's type");
                return;
            }
            if constexpr (std::is_invocable_v<std::decay_t<H>&, const T&, const MessageView&>)
                f(v, m);
            else
                f(v);
        };
    }
    Subscriber<Bytes> core_;
    priv::TypeCodec* c_ = nullptr;
};

template <class T>
Publisher<T> Node::publisher(std::string_view name, const Qos& qos, const Schema& schema) {
    if constexpr (std::is_same_v<T, Bytes>) {
        return Publisher<Bytes>(*this, name, priv::schema_ptr(schema), qos);
    } else {
        if (!priv::typed_takes_no_schema(bool(schema), qos.reflect_from_mesh,
                "rant::Node::publisher: a typed handle takes its schema from T, use rant::Bytes for a schema or reflect_from_mesh"))
            return {};
        return Publisher<T>(*this, name, qos);
    }
}

template <class T, class H, class>
Subscriber<T> Node::subscriber(std::string_view name, H&& handler, const Qos& qos, const Schema& schema) {
    if constexpr (std::is_same_v<T, Bytes>) {
        return Subscriber<Bytes>(*this, name, priv::schema_ptr(schema),
                                 MessageHandler(std::forward<H>(handler)), qos);
    } else {
        if (!priv::typed_takes_no_schema(bool(schema), qos.reflect_from_mesh,
                "rant::Node::subscriber: a typed handle takes its schema from T, use rant::Bytes for a schema or reflect_from_mesh"))
            return {};
        return Subscriber<T>(*this, name, std::forward<H>(handler), qos);
    }
}

template <class T>
Subscriber<T> Node::subscriber(std::string_view name, const Qos& qos, const Schema& schema) {
    if constexpr (std::is_same_v<T, Bytes>) {
        return Subscriber<Bytes>(*this, name, priv::schema_ptr(schema), qos);
    } else {
        if (!priv::typed_takes_no_schema(bool(schema), qos.reflect_from_mesh,
                "rant::Node::subscriber: a typed handle takes its schema from T, use rant::Bytes for a schema or reflect_from_mesh"))
            return {};
        return Subscriber<T>(*this, name, qos);
    }
}

#ifndef RANT_NO_PATTERNS

template <class Req, class Rsp, class H>
FunctionDefinition<Req, Rsp> Node::function_definition(std::string_view name, H&& handler,
        const FunctionOptions& o, const Schema& req_schema, const Schema& rsp_schema) {
    if constexpr (std::is_same_v<Req, Bytes> && std::is_same_v<Rsp, Bytes>) {
        return FunctionDefinition<Bytes, Bytes>(*this, name, priv::schema_ptr(req_schema),
            priv::schema_ptr(rsp_schema),
            typename FunctionDefinition<Bytes, Bytes>::Handler(std::forward<H>(handler)), o);
    } else {
        if (!priv::typed_takes_no_schema(req_schema || rsp_schema, o.reflect_from_mesh,
                "rant::Node::function_definition: a typed handle takes its schemas from its types, use rant::Bytes for schemas or reflect_from_mesh"))
            return {};
        return FunctionDefinition<Req, Rsp>(*this, name, std::forward<H>(handler), o);
    }
}

template <class Req, class Rsp>
RemoteFunction<Req, Rsp> Node::remote_function(std::string_view name, const FunctionOptions& o,
        const Schema& req_schema, const Schema& rsp_schema) {
    if constexpr (std::is_same_v<Req, Bytes> && std::is_same_v<Rsp, Bytes>) {
        return RemoteFunction<Bytes, Bytes>(*this, name, priv::schema_ptr(req_schema),
                                            priv::schema_ptr(rsp_schema), o);
    } else {
        if (!priv::typed_takes_no_schema(req_schema || rsp_schema, o.reflect_from_mesh,
                "rant::Node::remote_function: a typed handle takes its schemas from its types, use rant::Bytes for schemas or reflect_from_mesh"))
            return {};
        return RemoteFunction<Req, Rsp>(*this, name, o);
    }
}

template <class Req, class Prg, class Rsp, class H>
TaskDefinition<Req, Prg, Rsp> Node::task_definition(std::string_view name, H&& handler,
        const TaskOptions& o, const Schema& req_schema, const Schema& prg_schema,
        const Schema& rsp_schema) {
    if constexpr (std::is_same_v<Req, Bytes> && std::is_same_v<Prg, Bytes> && std::is_same_v<Rsp, Bytes>) {
        return TaskDefinition<Bytes, Bytes, Bytes>(*this, name, priv::schema_ptr(req_schema),
            priv::schema_ptr(prg_schema), priv::schema_ptr(rsp_schema),
            typename TaskDefinition<Bytes, Bytes, Bytes>::Handler(std::forward<H>(handler)), o);
    } else {
        if (!priv::typed_takes_no_schema(req_schema || prg_schema || rsp_schema, o.reflect_from_mesh,
                "rant::Node::task_definition: a typed handle takes its schemas from its types, use rant::Bytes for schemas or reflect_from_mesh"))
            return {};
        return TaskDefinition<Req, Prg, Rsp>(*this, name, std::forward<H>(handler), o);
    }
}

template <class Req, class Prg, class Rsp>
RemoteTask<Req, Prg, Rsp> Node::remote_task(std::string_view name, const TaskOptions& o,
        const Schema& req_schema, const Schema& prg_schema, const Schema& rsp_schema) {
    if constexpr (std::is_same_v<Req, Bytes> && std::is_same_v<Prg, Bytes> && std::is_same_v<Rsp, Bytes>) {
        return RemoteTask<Bytes, Bytes, Bytes>(*this, name, priv::schema_ptr(req_schema),
            priv::schema_ptr(prg_schema), priv::schema_ptr(rsp_schema), o);
    } else {
        if (!priv::typed_takes_no_schema(req_schema || prg_schema || rsp_schema, o.reflect_from_mesh,
                "rant::Node::remote_task: a typed handle takes its schemas from its types, use rant::Bytes for schemas or reflect_from_mesh"))
            return {};
        return RemoteTask<Req, Prg, Rsp>(*this, name, o);
    }
}

template <class T>
VariableDefinition<T> Node::variable_definition(std::string_view name, const VariableOptions<T>& o,
        const Schema& schema) {
    if constexpr (std::is_same_v<T, Bytes>) {
        return VariableDefinition<Bytes>(*this, name, priv::schema_ptr(schema), o);
    } else {
        if (!priv::typed_takes_no_schema(bool(schema), o.reflect_from_mesh,
                "rant::Node::variable_definition: a typed handle takes its schema from T, use rant::Bytes for a schema or reflect_from_mesh"))
            return {};
        return VariableDefinition<T>(*this, name, o);
    }
}

template <class T>
RemoteVariable<T> Node::remote_variable(std::string_view name, const VariableOptions<T>& o,
        const Schema& schema) {
    if constexpr (std::is_same_v<T, Bytes>) {
        return RemoteVariable<Bytes>(*this, name, priv::schema_ptr(schema), o);
    } else {
        if (!priv::typed_takes_no_schema(bool(schema), o.reflect_from_mesh,
                "rant::Node::remote_variable: a typed handle takes its schema from T, use rant::Bytes for a schema or reflect_from_mesh"))
            return {};
        return RemoteVariable<T>(*this, name, o);
    }
}

#endif /* !RANT_NO_PATTERNS */

}   /* namespace rant */

/* RANT_SCHEMA(T, fields...): reflect a struct for the typed codec. Invoke at global scope
 * after the struct with up to 64 members in wire order (docs/cpp.md). */
#define RANT_PP_EXPAND(x) x
#define RANT_PP_CAT2(a, b) a##b
#define RANT_PP_CAT(a, b) RANT_PP_CAT2(a, b)
#define RANT_PP_ARGN( \
    _1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16, \
    _17,_18,_19,_20,_21,_22,_23,_24,_25,_26,_27,_28,_29,_30,_31,_32, \
    _33,_34,_35,_36,_37,_38,_39,_40,_41,_42,_43,_44,_45,_46,_47,_48, \
    _49,_50,_51,_52,_53,_54,_55,_56,_57,_58,_59,_60,_61,_62,_63,_64, N, ...) N
#define RANT_PP_NARG(...) RANT_PP_EXPAND(RANT_PP_ARGN(__VA_ARGS__, \
    64,63,62,61,60,59,58,57,56,55,54,53,52,51,50,49, \
    48,47,46,45,44,43,42,41,40,39,38,37,36,35,34,33, \
    32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17, \
    16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1))
#define RANT_PP_FE_1(M, T, x) M(T, x)
#define RANT_PP_FE_2(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_1(M, T, __VA_ARGS__))
#define RANT_PP_FE_3(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_2(M, T, __VA_ARGS__))
#define RANT_PP_FE_4(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_3(M, T, __VA_ARGS__))
#define RANT_PP_FE_5(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_4(M, T, __VA_ARGS__))
#define RANT_PP_FE_6(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_5(M, T, __VA_ARGS__))
#define RANT_PP_FE_7(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_6(M, T, __VA_ARGS__))
#define RANT_PP_FE_8(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_7(M, T, __VA_ARGS__))
#define RANT_PP_FE_9(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_8(M, T, __VA_ARGS__))
#define RANT_PP_FE_10(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_9(M, T, __VA_ARGS__))
#define RANT_PP_FE_11(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_10(M, T, __VA_ARGS__))
#define RANT_PP_FE_12(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_11(M, T, __VA_ARGS__))
#define RANT_PP_FE_13(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_12(M, T, __VA_ARGS__))
#define RANT_PP_FE_14(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_13(M, T, __VA_ARGS__))
#define RANT_PP_FE_15(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_14(M, T, __VA_ARGS__))
#define RANT_PP_FE_16(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_15(M, T, __VA_ARGS__))
#define RANT_PP_FE_17(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_16(M, T, __VA_ARGS__))
#define RANT_PP_FE_18(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_17(M, T, __VA_ARGS__))
#define RANT_PP_FE_19(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_18(M, T, __VA_ARGS__))
#define RANT_PP_FE_20(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_19(M, T, __VA_ARGS__))
#define RANT_PP_FE_21(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_20(M, T, __VA_ARGS__))
#define RANT_PP_FE_22(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_21(M, T, __VA_ARGS__))
#define RANT_PP_FE_23(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_22(M, T, __VA_ARGS__))
#define RANT_PP_FE_24(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_23(M, T, __VA_ARGS__))
#define RANT_PP_FE_25(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_24(M, T, __VA_ARGS__))
#define RANT_PP_FE_26(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_25(M, T, __VA_ARGS__))
#define RANT_PP_FE_27(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_26(M, T, __VA_ARGS__))
#define RANT_PP_FE_28(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_27(M, T, __VA_ARGS__))
#define RANT_PP_FE_29(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_28(M, T, __VA_ARGS__))
#define RANT_PP_FE_30(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_29(M, T, __VA_ARGS__))
#define RANT_PP_FE_31(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_30(M, T, __VA_ARGS__))
#define RANT_PP_FE_32(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_31(M, T, __VA_ARGS__))
#define RANT_PP_FE_33(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_32(M, T, __VA_ARGS__))
#define RANT_PP_FE_34(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_33(M, T, __VA_ARGS__))
#define RANT_PP_FE_35(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_34(M, T, __VA_ARGS__))
#define RANT_PP_FE_36(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_35(M, T, __VA_ARGS__))
#define RANT_PP_FE_37(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_36(M, T, __VA_ARGS__))
#define RANT_PP_FE_38(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_37(M, T, __VA_ARGS__))
#define RANT_PP_FE_39(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_38(M, T, __VA_ARGS__))
#define RANT_PP_FE_40(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_39(M, T, __VA_ARGS__))
#define RANT_PP_FE_41(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_40(M, T, __VA_ARGS__))
#define RANT_PP_FE_42(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_41(M, T, __VA_ARGS__))
#define RANT_PP_FE_43(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_42(M, T, __VA_ARGS__))
#define RANT_PP_FE_44(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_43(M, T, __VA_ARGS__))
#define RANT_PP_FE_45(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_44(M, T, __VA_ARGS__))
#define RANT_PP_FE_46(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_45(M, T, __VA_ARGS__))
#define RANT_PP_FE_47(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_46(M, T, __VA_ARGS__))
#define RANT_PP_FE_48(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_47(M, T, __VA_ARGS__))
#define RANT_PP_FE_49(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_48(M, T, __VA_ARGS__))
#define RANT_PP_FE_50(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_49(M, T, __VA_ARGS__))
#define RANT_PP_FE_51(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_50(M, T, __VA_ARGS__))
#define RANT_PP_FE_52(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_51(M, T, __VA_ARGS__))
#define RANT_PP_FE_53(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_52(M, T, __VA_ARGS__))
#define RANT_PP_FE_54(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_53(M, T, __VA_ARGS__))
#define RANT_PP_FE_55(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_54(M, T, __VA_ARGS__))
#define RANT_PP_FE_56(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_55(M, T, __VA_ARGS__))
#define RANT_PP_FE_57(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_56(M, T, __VA_ARGS__))
#define RANT_PP_FE_58(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_57(M, T, __VA_ARGS__))
#define RANT_PP_FE_59(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_58(M, T, __VA_ARGS__))
#define RANT_PP_FE_60(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_59(M, T, __VA_ARGS__))
#define RANT_PP_FE_61(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_60(M, T, __VA_ARGS__))
#define RANT_PP_FE_62(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_61(M, T, __VA_ARGS__))
#define RANT_PP_FE_63(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_62(M, T, __VA_ARGS__))
#define RANT_PP_FE_64(M, T, x, ...) M(T, x) RANT_PP_EXPAND(RANT_PP_FE_63(M, T, __VA_ARGS__))
#define RANT_PP_FOR_EACH(M, T, ...) \
    RANT_PP_EXPAND(RANT_PP_CAT(RANT_PP_FE_, RANT_PP_NARG(__VA_ARGS__))(M, T, __VA_ARGS__))

#define RANT_SCHEMA_FIELD(T, f) \
    rant_v(::rant::field_tag<decltype(T::f)>{}, #f, offsetof(T, f));

#define RANT_SCHEMA(T, ...) \
    template <> struct rant::reflect<T> { \
        using is_rant_schema = void; \
        static_assert(std::is_standard_layout<T>::value, \
                      "RANT_SCHEMA: type must be standard-layout"); \
        static constexpr const char* type_name = #T; \
        template <class V> static void visit(V&& rant_v) { \
            RANT_PP_FOR_EACH(RANT_SCHEMA_FIELD, T, __VA_ARGS__) \
        } \
    }

/* RANT_ENUM(E, options...): register an enum class so a member ships as a named enum<uN>
 * with these enumerators. An unregistered enum ships as its backing integer. */
#define RANT_ENUM_ITEM(E, x) rant_v((int64_t)(E::x), #x);
#define RANT_ENUM(E, ...) \
    template <> struct rant::reflect_enum<E> { \
        using is_rant_enum = void; \
        static_assert(std::is_enum<E>::value, "RANT_ENUM: type must be an enum"); \
        static constexpr const char* type_name = #E; \
        template <class V> static void visit(V&& rant_v) { \
            RANT_PP_FOR_EACH(RANT_ENUM_ITEM, E, __VA_ARGS__) \
        } \
    }

/* The standard type registrations at global scope: each mirror gets its wire name here,
 * so a member spells as at: Transform and matches only a Transform. */
RANT_STD_STRUCT(Float2);     RANT_STD_STRUCT(Float3);     RANT_STD_STRUCT(Float4);
RANT_STD_STRUCT(Double2);    RANT_STD_STRUCT(Double3);    RANT_STD_STRUCT(Double4);
RANT_STD_STRUCT(Int2);       RANT_STD_STRUCT(Int3);       RANT_STD_STRUCT(Int4);
RANT_STD_STRUCT(Quaternion);
RANT_STD_STRUCT(Color);      RANT_STD_STRUCT(Rect);       RANT_STD_STRUCT(RectI);
RANT_STD_STRUCT(Transform); RANT_STD_STRUCT(Twist);       RANT_STD_STRUCT(GeoPoint);
RANT_STD_ALIAS(Uuid,        uint8_t[16]);
RANT_STD_ALIAS(Timestamp, int64_t);
RANT_STD_ALIAS(Duration,    int64_t);
RANT_STD_ALIAS(Matrix3x3, float[9]);
RANT_STD_ALIAS(Matrix4x4, float[16]);
RANT_STD_ALIAS(Uri,         rant::String<256>);
/* the video family: the enums are RANT_ENUM-registered so a member of one (in these
 * mirrors or in a user struct) ships as the canonical named integer */
RANT_ENUM(rant::types::ImageFormat, Mono8, Mono16, Rgb8, Rgba8, Bgr8, Yuyv, Nv12, Monof32, Jpeg, Png);
RANT_ENUM(rant::types::VideoCodec, Unknown, Mjpeg, H264, H265, Av1);
RANT_ENUM(rant::types::VideoStreamKind, Rtsp, WebrtcWhep, Hls, Srt, Rtp, HttpMjpeg, Other);
RANT_STD_STRUCT(Image); RANT_STD_STRUCT(VideoFrame); RANT_STD_STRUCT(ExternalVideoStream);
RANT_ENUM(rant::types::DistortionModel, NoDistortion, BrownConrady, Fisheye, Rational);
RANT_STD_STRUCT(CameraIntrinsics);
RANT_STD_STRUCT(JointState); RANT_STD_STRUCT(JointNames);

#endif /* C++ consumer (not the implementation anchor) */
#endif /* RANT_HPP_INCLUDED */
