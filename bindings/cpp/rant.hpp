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
    MetaTruncatedInterest, MetaTruncatedSchema, PeerMetaTooBig, MessageTooBig,
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
class MessageView;
class Event;
class Schema;
namespace priv { class TopicCore; }
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

/* Pass it where a constructor takes a schema pointer and the handle types itself from the
 * mesh (docs/reflection.md). refresh() re types it later. */
struct reflect_from_mesh_t { explicit reflect_from_mesh_t() = default; };
inline constexpr reflect_from_mesh_t reflect_from_mesh{};

/* Qos / NodeOptions: plain structs mirroring the C config, all zero = default. */
struct Qos {
    Reliability reliability          = Reliability::BestEffort;
    uint16_t    keep_last            = 0;   /* recent messages retained (late join / repair) */
    uint16_t    catch_up             = 0;   /* recent messages a new subscriber replays */
    uint32_t    max_message_bytes    = 0;   /* 0 = one fragment, or grow-to-fit */
    uint32_t    heartbeat_us         = 0;   /* reliable idle-writer ping (0 = 100ms) */
    uint32_t    repair_delay_us      = 0;   /* reader re ask bound, 0 = adaptive from the RTT */
    uint32_t    backpressure_wait_us = 0;   /* reliable: send pause for a slow reader (0 = none) */
    uint32_t    shm_max_bytes        = 0;   /* pin topic to one same-host SHM size class */
    uint32_t    queue_bytes          = 0;   /* take and dispatch queue cap, 0 = 1 MB */
    uint16_t    max_rate_hz          = 0;   /* best effort sub: kept samples per second, 0 = all */
    bool        no_timestamp         = false;   /* omit the source stamp, written_us() reads 0 */
};

struct NodeOptions {
    uint16_t                 domain               = 0;   /* logical-network selector */
    uint16_t                 max_topics           = 8;   /* how many topics may be created */
    bool                     disable_shm          = false;
    bool                     fetch_details        = false;   /* fetch every peer topic's schema */
    int32_t                  match_wait_ms        = 0;   /* 0 = 1 s, negative = drop loudly */
    /* networking (all optional) */
    /* built in observability, all on by default (Node::log, Node::on_log, Node::meta_request) */
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
    uint32_t                 announce_interval_us = 0;   /* 0 = 1s */
    uint32_t                 peer_timeout_us      = 0;   /* 0 = 3.5s */
    uint16_t                 max_peers            = 0;   /* 0 = 16 */
    /* Leave memory null for the dynamic allocator. A fixed buffer means static mode: no heap,
     * no growth, shared memory off, and construction fails if it is too small (docs/cpp.md). */
    void*                    memory      = nullptr;
    size_t                   memory_size = 0;
};

#ifndef RANT_NO_PATTERNS
/* Per-pattern options (all zero = defaults). */
struct FunctionOptions {
    uint32_t backpressure_wait_us = 0;   /* 0 = 1s (patterns are low-rate, loss unacceptable) */
    uint32_t timeout_us           = 0;   /* remote call timeout, 0 = 5 s */
    uint16_t keep_last            = 0;   /* req and rsp ring depth, 0 = 10 */
};
/* Task options, mirrors RantTaskOpts (docs/tasks.md). */
struct TaskOptions {
    bool     progress_best_effort = false;   /* the definition offers, a remote requests */
    uint16_t progress_keep_last   = 0;   /* progress ring depth, 0 = the pattern default */
    bool     no_cancel            = false;   /* cancel not honored, remotes get BadRole */
    bool     exclusive            = false;   /* declared serialization, handler enforced */
    bool     multi                = false;   /* redundant providers, one executor each */
    uint32_t timeout_us           = 0;   /* remote: bound until the first response, 0 = 5 s */
    uint32_t backpressure_wait_us = 0;     /* 0 = 1s */
    uint16_t keep_last            = 0;     /* req + rsp ring depth (FunctionOptions::keep_last) */
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
    uint32_t backpressure_wait_us = 0;       /* 0 = 1s */
};
template <> struct VariableOptions<Bytes> {
    Bytes    initial{};
    bool     read_only            = false;
    bool     allow_force          = false;
    uint16_t catch_up             = 0;
    uint16_t keep_last            = 0;
    uint32_t backpressure_wait_us = 0;
};
#endif /* !RANT_NO_PATTERNS */

/* Schema: an owned, compiled message schema (see the DSL in schema.h). */
class Schema {
public:
    /* Compile a schema from DSL text. nullopt on error, and err receives a short message
     * pointing near the offending text. The compiled schema is freed with the Schema. */
    static std::optional<Schema> compile(std::string_view text, std::string* err = nullptr) {
        return compile_with(detail::rant_allocator_heap(0), text, err);
    }
    /* Zero heap variant: compile into scratch, which must outlive this Schema. Once a create
     * has copied the schema into the node the scratch may be reused. nullopt if too small. */
    static std::optional<Schema> compile(std::string_view text, void* scratch, size_t scratch_size,
                                         std::string* err = nullptr) {
        if (!scratch || scratch_size == 0) { if (err) *err = "scratch buffer missing"; return std::nullopt; }
        return compile_with(detail::rant_allocator_static(scratch, scratch_size), text, err);
    }

    /* An EMPTY schema (empty() true, raw() null): what an untyped entity reports, and the
     * state a default-constructed member holds until assigned. */
    Schema() = default;
    /* An owned copy of any compiled schema, ours or one reflected off the mesh. */
    static Schema adopt(const detail::RantSchema* raw) {
        Schema s;
        if (!raw) return s;
        s.alloc_ = detail::rant_allocator_heap(0);
        s.schema_ = detail::rant_schema_copy(raw, detail::rant_allocator_alloc, &s.alloc_);
        if (!s.schema_) detail::rant_allocator_reset(&s.alloc_);
        return s;
    }
    Schema(const Schema& o) : Schema(adopt(o.schema_)) {}
    Schema& operator=(const Schema& o) { if (this != &o) *this = adopt(o.schema_); return *this; }
    Schema(Schema&& o) noexcept : alloc_(o.alloc_), schema_(o.schema_) {
        std::memset(&o.alloc_, 0, sizeof o.alloc_);
        o.schema_ = nullptr;
    }
    Schema& operator=(Schema&& o) noexcept {
        if (this != &o) { reset(); alloc_ = o.alloc_; schema_ = o.schema_;
                          std::memset(&o.alloc_, 0, sizeof o.alloc_); o.schema_ = nullptr; }
        return *this;
    }
    ~Schema() { reset(); }

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
     * every named type it uses hoisted to a leading `Name = type` definition. */
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
        uint16_t  arr_parent;    /* flat index of the enclosing struct ARRAY, 0xFFFF for none */
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
        out.str_cap = f.str_cap; out.arr_parent = f.arr_parent;
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
    static std::optional<Schema> compile_with(detail::RantAllocator a, std::string_view text, std::string* err) {
        Schema s;
        s.alloc_ = a;
        std::string t(text);   /* NUL-terminate for the C API */
        const char* e = nullptr;
        s.schema_ = detail::rant_schema_compile(detail::rant_allocator_alloc, &s.alloc_, t.c_str(), &e);
        if (!s.schema_) {
            if (err) *err = e ? (std::string("schema error near: ") + e) : "schema compile failed";
            detail::rant_allocator_reset(&s.alloc_);
            return std::nullopt;
        }
        return s;
    }
    void reset() { if (alloc_.page_realloc || alloc_.shared) detail::rant_allocator_reset(&alloc_); schema_ = nullptr; }
    detail::RantAllocator alloc_{};
    detail::RantSchema*     schema_ = nullptr;
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

/* Give a struct-shaped type a wire name: reflect its members, then name it. */
#define RANT_STD_STRUCT(T, ...) \
    template <> struct rant::reflect<rant::T> { \
        using is_rant_schema = void; \
        static constexpr const char* type_name = #T; \
        template <class V> static void visit(V&& v) { RANT_STD_MEMBERS_##T(v) } \
    }; \
    template <> struct rant::std_type<rant::T> { \
        static constexpr const char* name = #T; using repr = void; }
/* Give an ALIAS-shaped type a wire name: it copies as R (`Uuid = u8[16]`, R = uint8_t[16]). */
#define RANT_STD_ALIAS(T, R) \
    template <> struct rant::std_type<rant::T> { \
        static constexpr const char* name = #T; using repr = R; }

#define RANT_STD_F(T, f) v(::rant::field_tag<decltype(::rant::T::f)>{}, #f, offsetof(::rant::T, f));
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
#define RANT_STD_MEMBERS_Transform(v) v(::rant::field_tag<::rant::Double3>{}, "translation", offsetof(::rant::Transform, translation)); \
                                    v(::rant::field_tag<::rant::Quaternion>{}, "rotation", offsetof(::rant::Transform, rotation)); \
                                    RANT_STD_F(Transform,parent)
#define RANT_STD_MEMBERS_Twist(v)     v(::rant::field_tag<::rant::Double3>{}, "linear", offsetof(::rant::Twist, linear)); \
                                    v(::rant::field_tag<::rant::Double3>{}, "angular", offsetof(::rant::Twist, angular));
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
};

/* One variable member, a length framed section on the message tail reached by dotted path
 * through the C accessors. Access is type erased through function pointers. */
struct Tail {
    uint32_t    struct_off = 0;
    uint8_t     is_string = 0;                  /* std::string member (VSTR frame) */
    uint8_t     elem_kind = 0;                  /* std::vector<E>: E's wire kind */
    std::string path;
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
        leaves.push_back(std::move(l));
    }
    /* a VARIABLE member: no fixed leaf, a Tail reached by path via the C accessors */
    template <class E> void add_vector(const char* name, size_t off) {
        constexpr int k = scalar_kind_of<E>();
        static_assert(k >= 0, "RANT_SCHEMA: std::vector element must be a wire scalar "
                              "(no vectors of structs, strings, or vectors)");
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
        if constexpr (is_reg_enum<U>::value) add_enum<U>(name, off);   /* named options */
        else add_scalar<std::underlying_type_t<U>>(name, off, 1, false); /* a plain int */
    } else if constexpr (is_rant_string<U>::value) {
        add_string<U>(name, off, 1, false);
    } else if constexpr (std::is_array_v<U>) {
        using E = std::remove_extent_t<U>;
        constexpr size_t n = std::extent_v<U>;
        if constexpr (is_rant_string<E>::value) add_string<E>(name, off, n, true);
        else                                    add_scalar<E>(name, off, n, true);
    } else if constexpr (is_std_array<U>::value) {
        using E = typename is_std_array<U>::elem;
        constexpr size_t n = is_std_array<U>::count;
        if constexpr (is_rant_string<E>::value) add_string<E>(name, off, n, true);
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
    } else if constexpr (is_std_vector<U>::value) {
        add_vector<typename is_std_vector<U>::elem>(name, off);
    } else if constexpr (is_var_string<U>::value) {
        add_var_string(name, off);
    } else {
        static_assert(always_false<U>,
            "RANT_SCHEMA: unsupported field type (no pointers/maps; use scalars, "
            "rant::String<N>, fixed arrays, std::vector<scalar>, std::string, or a "
            "nested RANT_SCHEMA struct)");
    }
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
        l.wire_off = fi.offset;
    }
    return true;
}

/* verify every tail member resolves in a compiled schema with the matching variable
 * kind (a tail needs no offset: the accessors walk the frames per message) */
inline bool tails_resolve(const detail::RantSchema* s, const std::vector<Tail>& tv) {
    for (const Tail& t : tv) {
        int idx = detail::rant_schema_field_index(s, t.path.c_str());
        if (idx < 0) return false;
        detail::RantSchemaFieldInfo fi;
        if (!detail::rant_schema_field_at(s, (uint16_t)idx, &fi)) return false;
        if (t.is_string) {
            if (fi.kind != (uint8_t)detail::RANT_VSTR) return false;
        } else {
            if (fi.kind != (uint8_t)detail::RANT_VARR || fi.elem != t.elem_kind) return false;
        }
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
        if (l.kind == (uint8_t)detail::RANT_STR) {
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

/* T is a bare wire type, usable as a handle's whole schema with no RANT_SCHEMA. std::string
 * is the unbounded string root and std::vector<E> the E[] root, both on the tail path. */
template <class U> constexpr bool is_value_type() {
    return scalar_kind_of<U>() >= 0 || std::is_enum_v<U> || is_rant_string<U>::value
        || is_std_array<U>::value || is_std_vector<U>::value || is_var_string<U>::value;
}

/* the per-T registration: schema + copy table + rebase cache, built once, immortal */
struct TypeCodec {
    bool                      ok = false;
    bool                      memcpy_ok = false;      /* our own layout coincides with our wire */
    bool                      memcpy_capable = false; /* trivially copyable + little-endian host */
    std::optional<Schema>     schema;
    const detail::RantSchema* raw = nullptr;
    uint64_t                  hash = 0;
    uint32_t                  wire_size = 0;   /* the fixed section, all of it when no tails */
    uint32_t                  msg_min = 0;            /* fixed section + one empty frame per tail */
    uint32_t                  struct_size = 0;
    std::vector<Leaf>         leaves;
    std::vector<Tail>         tails;                  /* variable members, declaration order */

    /* one incoming schema pointer's resolved offsets. A rebased schema keeps our hash with the
     * publisher's offsets, so offsets are resolved per pointer and never chosen by hash. */
    struct Rebased { std::vector<Leaf> leaves; uint32_t size = 0; bool ok = false, coincide = false; };
    std::mutex                                       mu;
    std::map<const detail::RantSchema*, Rebased>       rebased;

    const Rebased* rebased_for(const detail::RantSchema* s) {
        std::lock_guard<std::mutex> g(mu);
        auto it = rebased.find(s);
        if (it != rebased.end()) return &it->second;
        Rebased r;
        r.leaves = leaves;                    /* keep struct offsets, refill wire offsets */
        r.ok     = fill_offsets(s, r.leaves) && tails_resolve(s, tails);
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

inline std::string strip_namespaces(const char* type_name) {
    std::string n(type_name);
    size_t p = n.rfind(':');
    return p == std::string::npos ? n : n.substr(p + 1);
}

template <class T> TypeCodec* build_codec() {
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
    c->schema = Schema::compile(b.dsl);
    if (!c->schema) return c;
    c->raw         = c->schema->raw();
    c->hash        = c->schema->hash();
    c->wire_size   = detail::rant_schema_size(c->raw);
    c->msg_min     = detail::rant_schema_msg_min(c->raw);
    c->struct_size = (uint32_t)sizeof(T);
    if (!fill_offsets(c->raw, c->leaves)) return c;
    if (!tails_resolve(c->raw, c->tails)) return c;
    c->memcpy_capable = std::is_trivially_copyable<T>::value && host_le();
    bool coincide = c->memcpy_capable && c->tails.empty() && sizeof(T) == c->wire_size;
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

/* the compiled Schema for T, registered on first use. nullptr if the synthesized DSL failed
 * to compile, a codec bug the handle constructors surface */
template <class T> const Schema* schema_of() {
    TypeCodec& c = type_codec<T>();
    return c.ok ? &*c.schema : nullptr;
}

/* struct to wire. The memcpy path returns a view of the struct itself, else the field loop
 * packs into scratch, tails in declaration order. Empty Bytes = codec invalid. */
template <class T> Bytes encode(const T& v, std::vector<uint8_t>& scratch) {
    TypeCodec& c = type_codec<T>();
    if (!c.ok) return Bytes();
    if (c.memcpy_ok) return Bytes(&v, sizeof(T));
    const bool le = host_le();
    const uint8_t* base = reinterpret_cast<const uint8_t*>(&v);
    if (c.tails.empty()) {
        scratch.assign(c.wire_size, 0);
        for (const Leaf& l : c.leaves) leaf_to_wire(l, base, scratch.data(), le);
        return Bytes(scratch.data(), scratch.size());
    }
    size_t need = c.msg_min;
    for (const Tail& t : c.tails) need += t.extra(base + t.struct_off);
    scratch.assign(need, 0);
    if (!detail::rant_schema_message_default(c.raw, scratch.data(), scratch.size()))
        return Bytes();
    for (const Leaf& l : c.leaves) leaf_to_wire(l, base, scratch.data(), le);
    for (const Tail& t : c.tails)
        if (!t.write(base + t.struct_off, scratch.data(), scratch.size(),
                     c.raw, t.path.c_str()))
            return Bytes();
    return Bytes(scratch.data(),
                 detail::rant_schema_msg_len(c.raw, scratch.data(), scratch.size()));
}

/* wire to struct. Offsets always come from the delivered schema and are cached per schema
 * pointer, with the memcpy path when that layout matches. nullptr assumes our own layout. */
template <class T> bool decode(T& out, Bytes data, const detail::RantSchema* schema) {
    TypeCodec& c = type_codec<T>();
    if (!c.ok) return false;
    const detail::RantSchema* sch = c.raw;
    const std::vector<Leaf>* lv = &c.leaves;
    uint32_t need = c.wire_size;
    bool fast = c.memcpy_ok;
    if (schema && schema != c.raw) {
        const TypeCodec::Rebased* rb = c.rebased_for(schema);
        if (!rb->ok) return false;
        lv   = &rb->leaves;
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
    if (!c.tails.empty()) {                     /* frames walked per message, by path */
        detail::RantBytes mb; mb.data = data.data(); mb.len = data.size();
        for (const Tail& t : c.tails)
            if (!t.read(base + t.struct_off, mb, sch, t.path.c_str()))
                return false;
    }
    return true;
}

}   /* namespace priv */

namespace priv {

/* The name slot under Publisher<Bytes> and Subscriber<Bytes>: one C topic per name, its
 * role widened as handles join. Thin and non owning. */
class TopicCore {
public:
    TopicCore() = default;
    /* Create (or share) a topic on `node`. Throws rant::Error on failure (with
     * -fno-exceptions: check valid()). schema is copied into the node. */
    TopicCore(Node& node, std::string_view name, Role role = Role::PubSub,
              const Schema* schema = nullptr, const Qos& qos = {});
    /* The same, typed by the mesh (see reflect_from_mesh). */
    TopicCore(Node& node, std::string_view name, Role role, reflect_from_mesh_t, const Qos& qos = {});

    bool valid() const noexcept { return ch_ != nullptr; }
    /* A reflect_from_mesh topic: re read the mesh and re type in place if what it took has
     * moved. true = re typed, false = current or not a reflect handle. */
    bool refresh() { return ch_ && detail::rant_topic_refresh(ch_) == 1; }
    explicit operator bool() const noexcept { return valid(); }

    /* capture is when the data was true, as against when it was sent. The default is
     * unstated, which costs no wire bytes. */
    SendStatus send(Bytes data, Timestamp capture = {}) {
        detail::RantSendOpts o;
        if (!ch_) return SendStatus::NoTopic;
        o.capture_us = static_cast<uint64_t>(capture.us);
        return static_cast<SendStatus>(
            detail::rant_topic_send(ch_, detail::rant_bytes(data.data(), data.size()), &o));
    }
    /* Retire the topic so the name can be re created with another schema (docs/topics.md).
     * Every handle sharing the slot is invalid after Ok. Refused with State from a callback. */
    SendStatus retire();
    uint16_t index() const {
        if (!ch_) return 0xffff;
        return detail::rant_topic_index(ch_);
    }
    int match_count() const {
        if (!ch_) return 0;
        return detail::rant_topic_match_count(ch_);
    }
    /* 1 when a send would not wait: a subscriber is matched, or matching has converged
     * so there is nobody to wait for. The async form of the send-path match wait. */
    bool ready() const {
        if (!ch_) return false;
        return detail::rant_topic_ready(ch_) == 1;
    }
    /* Pump until every reader has acked, or timeout_ms elapses. Call before
     * closing so a final burst is not cut off by the BYE. */
    bool drain(int timeout_ms) {
        if (!ch_) return true;
        return detail::rant_topic_drain(ch_, timeout_ms) == 1;
    }

    /* Cumulative traffic counters (always on): messages/bytes this node committed to the
     * topic (tx) and delivered from it (rx). Also carried in the @rant/meta snapshot. */
    struct Counts { uint64_t tx_msgs = 0, tx_bytes = 0, rx_msgs = 0, rx_bytes = 0; };
    Counts counts() const {
        Counts c;
        if (ch_) detail::rant_topic_counts(ch_, &c.tx_msgs, &c.tx_bytes, &c.rx_msgs, &c.rx_bytes);
        return c;
    }

private:
    TopicCore(Node& node, std::string_view name, Role role, const Schema* schema, const Qos& qos, bool reflect);

    detail::RantTopic* ch_ = nullptr;
    void* impl_ = nullptr;   /* the owning Node::Impl (Node is incomplete here), so
                                retire() can forget the wrapper's name-cache entry */
    friend class ::rant::Node;
};

}   /* namespace priv */

#ifndef RANT_NO_PATTERNS

/* A parked function reply from Request::defer. Movable and single shot, completed from any
 * thread. Dropping it leaves the caller to its timeout. */
template <> class Deferred<Bytes> {
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

    /* message: optional outcome text, ResponseView::message on the caller, truncated at
     * RANT_CALL_MSG_MAX. On fail it is what a generic consumer displays. */
    bool complete(Bytes rsp = Bytes(), std::string_view message = {}) {
        return finish(detail::RANT_CALL_OK, message, rsp);
    }
    bool fail(std::string_view message = {}, Bytes rsp = Bytes()) {
        return finish(detail::RANT_CALL_APP_ERROR, message, rsp);
    }

private:
    Deferred(detail::RantFunction* fn, uint64_t token) : fn_(fn), token_(token) {}
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
    Deferred<Bytes> defer() { return Deferred<Bytes>(fn_, detail::rant_request_defer(rq_)); }

private:
    Request(detail::RantRequest* rq, detail::RantFunction* fn)
        : FieldView(rq->data, rq->schema), rq_(rq), fn_(fn) {}
    detail::RantRequest*    rq_;
    detail::RantFunction* fn_;
    template <class A, class B> friend class FunctionDefinition;
};

/* A deferred task call in flight from TaskRequest::defer, movable into any thread the app
 * owns. The verb contract is in docs/cpp.md and docs/tasks.md. */
template <> class PendingTask<Bytes, Bytes> {
public:
    PendingTask() = default;
    PendingTask(PendingTask&& o) noexcept : fn_(o.fn_), token_(o.token_) { o.fn_ = nullptr; o.token_ = 0; }
    PendingTask& operator=(PendingTask&& o) noexcept {
        if (this != &o) { fn_ = o.fn_; token_ = o.token_; o.fn_ = nullptr; o.token_ = 0; }
        return *this;
    }
    PendingTask(const PendingTask&) = delete;
    PendingTask& operator=(const PendingTask&) = delete;

    bool valid() const noexcept { return fn_ != nullptr && token_ != 0; }
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
    PendingTask(detail::RantFunction* fn, uint64_t token) : fn_(fn), token_(token) {}
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
    PendingTask<Bytes, Bytes> defer() { return PendingTask<Bytes, Bytes>(fn_, detail::rant_request_defer(rq_)); }

private:
    TaskRequest(detail::RantRequest* rq, detail::RantFunction* fn)
        : FieldView(rq->data, rq->schema), rq_(rq), fn_(fn) {}
    detail::RantRequest*    rq_;
    detail::RantFunction* fn_;
    template <class A, class B, class C> friend class TaskDefinition;
};

/* An owning function call outcome from RemoteFunction<Bytes, Bytes>::call. send_status() carries a
 * synchronous refusal when the call never launched. */
template <> class Response<Bytes> {
public:
    Response() = default;
    CallStatus status()      const { return st_; }
    bool       ok()          const { return st_ == CallStatus::Ok; }
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
};
}

/* Section-mask bits for a @rant/meta request: OR them into Node::meta_request's
 * `sections` (0 = every section). */
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
    static MetaSnapshot decode(const Response<Bytes>& r) {
        return decode(r.status(), r.provider(), r.data(), r.raw_schema());
    }
    static MetaSnapshot decode(const ResponseView<Bytes>& r) {
        return decode(r.status(), r.provider(), r.data(), r.raw_schema());
    }
};

#endif /* !RANT_NO_PATTERNS */

/* Owns the RantNode, its memory and the user callbacks. Every call is serialized by the C
 * node lock. The threading and callback rules are in docs/cpp.md and docs/node.md. */
class Node {
public:
    using MessageHandler = std::function<void(const MessageView&)>;
    using EventHandler   = std::function<void(const Event&)>;

    /* Open a node. An empty name is auto generated. on_message may be empty, on_event is
     * required. Throws rant::Error, or under -fno-exceptions check valid(). */
    Node(std::string_view name, MessageHandler on_message,
         EventHandler on_event, const NodeOptions& o = {}) {
        if (!on_event) { priv::raise_msg("rant::Node: an on_event handler is required"); return; }
        std::unique_ptr<Impl> impl(new Impl());
        impl->on_msg   = std::move(on_message);
        impl->on_event = std::move(on_event);
        /* The node retains the net string/seed pointers, so own that storage. */
        impl->disc_group = o.discovery_group;
        impl->mcast_if   = o.multicast_interface;
        impl->self_ip    = o.self_ip;
        for (const std::string& s : o.seed_peers) {
            detail::RantDiscoveryAddr a;
            if (parse_addr(s, a)) impl->seeds.push_back(a);
        }
        std::string nm(name);

        detail::RantNodeOpts co;
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
        co.net.recv_buffer_bytes   = o.recv_buffer_bytes;
        co.net.send_buffer_bytes   = o.send_buffer_bytes;
        co.discovery.announce_interval_us = o.announce_interval_us;
        co.discovery.peer_timeout_us      = o.peer_timeout_us;
        co.discovery.max_peers            = o.max_peers;

        detail::RantAllocator mem = (o.memory && o.memory_size)
            ? detail::rant_allocator_static(o.memory, o.memory_size)
            : detail::rant_allocator_heap(0);
        detail::RantNode* n = detail::rant_node_open(
            &mem, nm.empty() ? nullptr : nm.c_str(),
            &Node::on_msg_tramp, &Node::on_evt_tramp, &co);
        if (!n) { priv::raise_last(nullptr, "rant::Node open"); return; }
        impl->node = n;
        impl_ = std::move(impl);
    }

    bool valid() const noexcept { return impl_ != nullptr && impl_->node != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    Node(Node&&) noexcept = default;
    Node& operator=(Node&&) noexcept = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    ~Node() = default;   /* teardown lives in Impl::~Impl so a move assign tears down too. It stops
                            the service thread and closes with a BYE */

    /* One loop tick: discovery, receive, timers and queued sends. Blocks up to timeout_ms in
     * the socket wait, 0 = non blocking. Refused while start() runs. */
    int poll(int timeout_ms = 0) {
        if (!valid()) return (int)SendStatus::State;
        return detail::rant_node_poll(impl_->node, timeout_ms);
    }

    /* Run the C service thread. Every call stays safe from any thread and a send wakes it. */
    bool start() { return valid() && detail::rant_node_start(impl_->node) == 0; }
    /* Stop and join the service thread. Idempotent, implied by teardown. */
    void stop()  { if (valid()) detail::rant_node_stop(impl_->node); }
    bool is_started() const { return valid() && detail::rant_node_is_started(impl_->node) == 1; }

    /* Block until discovery and matching settle, so everything sent now reaches everyone on
     * the network. Call after creating the topics. timeout_ms < 0 = 3 announce intervals. */
    bool settle(int timeout_ms = -1) {
        return valid() && detail::rant_node_settle(impl_->node, timeout_ms) == 1;
    }

    /* "Who is here": every discovered peer, a copied snapshot. */
    std::vector<Peer> peers() const {
        std::vector<Peer> out;
        if (!valid()) return out;
        LockGuard guard(impl_->node);
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

    /* "What a node offers": the entities one node advertises, peer 0 for this one. An
     * observer-grade call: it copies every schema. */
    std::vector<Entity> entities(uint32_t peer = 0) const {
        std::vector<Entity> out;
        if (!valid()) return out;
        LockGuard guard(impl_->node);
        detail::RantIter it; std::memset(&it, 0, sizeof it);
        detail::RantEntityInfo ei;
        while (detail::rant_node_entities_next(impl_->node, peer, &it, &ei))
            out.push_back(entity_from(ei));
        return out;
    }

    /* The whole mesh folded: one entity per (kind, name) across every active peer and this
     * node, schemas from the provider, conflict when the endpoints disagree. */
    std::vector<Entity> mesh() const {
        std::vector<Entity> out;
        if (!valid()) return out;
        LockGuard guard(impl_->node);
        detail::RantIter it; std::memset(&it, 0, sizeof it);
        detail::RantEntityInfo ei;
        while (detail::rant_node_mesh_next(impl_->node, &it, &ei))
            out.push_back(entity_from(ei));
        return out;
    }
    std::optional<Entity> mesh_find(EntityKind kind, std::string_view name) const {
        if (!valid()) return std::nullopt;
        LockGuard guard(impl_->node);
        detail::RantEntityInfo ei;
        std::string nm(name);
        if (!detail::rant_node_mesh_find(impl_->node, static_cast<detail::RantEntityKind>(kind),
                                         nm.c_str(), &ei)) return std::nullopt;
        return entity_from(ei);
    }
    /* Bumps on every reflected change anywhere in the mesh: re-walk iff it moved. */
    uint32_t mesh_epoch() const { return valid() ? detail::rant_node_mesh_epoch(impl_->node) : 0; }

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

    /* Subscribe to a level's mesh wide log stream: every other node's lines, decoded to a
     * LogLine, on the polling thread. Call it once per level from setup. false when disabled. */
    bool on_log(LogLevel level, std::function<void(const LogLine&)> cb) {
        if (!valid() || !cb) return false;
        detail::RantTopic* ch = detail::rant_node_log_topic(
            impl_->node, static_cast<detail::RantLogLevel>(level));
        if (!ch) return false;
        if (detail::rant_topic_set_role(ch, detail::RANT_PUBSUB) != 0) return false;
        uint16_t idx = detail::rant_topic_index(ch);
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

#ifndef RANT_NO_PATTERNS
    /* Fetch a peer's snapshot: a directed @rant/meta call decoded into an owning MetaSnapshot.
     * cb fires once on the polling thread. sections is a MetaSection mask, 0 = all. */
    SendStatus meta_request(uint32_t peer, std::function<void(const MetaSnapshot&)> cb,
                            uint32_t sections = 0);
#endif

private:
    struct TopicRec { detail::RantTopic* ch; uint8_t bits; uint64_t schema_hash; };
#ifndef RANT_NO_PATTERNS
    /* the local @rant/meta caller handle behind meta_request, invalid when meta is disabled */
    RemoteFunction<Bytes, Bytes> meta();
#endif

    struct Impl {
        detail::RantNode*          node = nullptr;
        MessageHandler             on_msg;
        EventHandler               on_event;
        std::string                disc_group;
        std::string                mcast_if;
        std::string                self_ip;
        std::vector<detail::RantDiscoveryAddr> seeds;

        /* wrapper registries. create_mu serializes wrapper side creates and is never taken from
         * a callback. reg_mu is a leaf lock, never call into C while holding it. */
        std::mutex               create_mu;
        std::mutex               reg_mu;
        std::map<std::string, TopicRec, std::less<>> topics;   /* name to shared slot */
        std::unordered_map<uint16_t,
            std::shared_ptr<const std::vector<MessageHandler>>> sub_handlers;
        std::vector<std::unique_ptr<priv::HandlerBox>> boxes;  /* pattern handler boxes */
#ifndef RANT_NO_PATTERNS
        std::unordered_set<void*> async_live;                  /* outstanding AsyncBox* */
#endif

        ~Impl() {
            if (node) detail::rant_node_close(node, /*send_bye=*/1);
#ifndef RANT_NO_PATTERNS
            /* the C never fires pending async callbacks at close, so reap the boxes */
            for (void* b : async_live) delete static_cast<priv::AsyncBox*>(b);
#endif
        }
    };
    std::unique_ptr<Impl> impl_;

    struct LockGuard {
        detail::RantNode* n;
        explicit LockGuard(detail::RantNode* node) : n(node) { detail::rant_node_lock(n); }
        ~LockGuard() { detail::rant_node_unlock(n); }
    };

    static void on_msg_tramp(const detail::RantMsg* m) {
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
    static void on_evt_tramp(const detail::RantEvent* e) {
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
        detail::RantEvent e;
        std::memset(&e, 0, sizeof e);
        e.kind = detail::RANT_ERROR;
        e.error = detail::RANT_E_NONE;
        e.user = impl;
        Event ev(&e);
        try { impl->on_event(ev); } catch (...) {}
#else
        (void)impl;
#endif
    }

    /* one entity view (valid under the node lock the walk holds) into an owned Entity */
    static Entity entity_from(const detail::RantEntityInfo& ei) {
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
        e.schema          = Schema::adopt(ei.schema);
        e.rsp_schema      = Schema::adopt(ei.rsp_schema);
        e.progress_schema = Schema::adopt(ei.progress_schema);
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
        c.heartbeat_us         = q.heartbeat_us;
        c.repair_delay_us      = q.repair_delay_us;
        c.backpressure_wait_us = q.backpressure_wait_us;
        c.shm_max_bytes        = q.shm_max_bytes;
        c.queue_bytes          = q.queue_bytes;
        c.max_rate_hz          = q.max_rate_hz;
        c.no_timestamp         = q.no_timestamp ? 1 : 0;
        return c;
    }


    /* Parse "ip" or "ip:port" into a locator, port 0 = discovery_port. Hand rolled so no
     * locale bound scanf is needed. */
    static bool parse_addr(const std::string& s, detail::RantDiscoveryAddr& out) {
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
    template <class A, class B> friend class FunctionDefinition;
    template <class A, class B> friend class RemoteFunction;
    template <class A, class B, class C> friend class TaskDefinition;
    template <class A, class B, class C> friend class RemoteTask;
    template <class A> friend class VariableDefinition;
    template <class A> friend class RemoteVariable;
    template <class A> friend class Publisher;
    template <class A> friend class Subscriber;
};

/* Create, or share the same name slot with a widened role. A live same name topic with a
 * different schema refuses, since two modules disagreeing is a bug. retire() first. */
inline priv::TopicCore::TopicCore(Node& node, std::string_view name, Role role, reflect_from_mesh_t, const Qos& qos)
    : TopicCore(node, name, role, nullptr, qos, /*reflect=*/true) {}

inline priv::TopicCore::TopicCore(Node& node, std::string_view name, Role role,
                                  const Schema* schema, const Qos& qos) : TopicCore(node, name, role, schema, qos, false) {}

inline priv::TopicCore::TopicCore(Node& node, std::string_view name, Role role,
                                  const Schema* schema, const Qos& qos, bool reflect) {
    if (!node.valid()) { priv::raise_msg("rant topic create: node is not valid"); return; }
    Node::Impl* impl = node.impl_.get();
    std::lock_guard<std::mutex> g(impl->create_mu);
    std::string nm(name);
    uint64_t sh = schema ? schema->hash() : 0;
    impl_ = impl;
    auto it = impl->topics.find(nm);
    if (it != impl->topics.end()) {
        if (sh && it->second.schema_hash && sh != it->second.schema_hash) {
            priv::raise_msg("rant topic create: the name already exists with a different schema"
                            " (retire() it to retype the name)");
            return;
        }
        uint8_t bits = (uint8_t)(it->second.bits | priv::role_bits(role));
        if (bits != it->second.bits) {
            detail::rant_topic_set_role(it->second.ch, static_cast<detail::RantRole>(priv::role_from_bits(bits)));
            it->second.bits = bits;
        }
        ch_ = it->second.ch;
        return;
    }
    detail::RantTopicOpts co;
    std::memset(&co, 0, sizeof co);
    co.qos = Node::to_c(qos);
    co.reflect_from_mesh = reflect ? 1 : 0;
    ch_ = detail::rant_node_create_topic(impl->node, nm.c_str(),
              static_cast<detail::RantRole>(role), schema ? schema->raw() : nullptr, &co);
    if (!ch_) { priv::raise_last(impl->node, "rant topic create"); return; }
    impl->topics.emplace(std::move(nm), Node::TopicRec{ ch_, priv::role_bits(role), sh });
}

/* Retire: on success the C handle is freed, the name cache entry and this topic's wrapper
 * subscriptions are forgotten, and every handle sharing the slot goes invalid. */
inline SendStatus priv::TopicCore::retire() {
    if (!ch_) return SendStatus::NoTopic;
    Node::Impl* impl = static_cast<Node::Impl*>(impl_);
    if (!impl) {   /* no registry back-pointer (default-constructed edge): C-level only */
        int bare = detail::rant_topic_retire(ch_);
        if (bare == 0) ch_ = nullptr;
        return static_cast<SendStatus>(bare);
    }
    std::lock_guard<std::mutex> g(impl->create_mu);
    uint16_t idx = detail::rant_topic_index(ch_);
    int rc = detail::rant_topic_retire(ch_);
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

#ifndef RANT_NO_PATTERNS

/* ====================== FUNCTIONS (untyped cores) =========================== */

/* The untyped implementation side of a function. One reply per call, one definition per
 * name on the network. Thin and non owning, the function lives in the node until close. */
template <> class FunctionDefinition<Bytes, Bytes> {
public:
    using Handler = std::function<void(Request<Bytes>&)>;

    FunctionDefinition() = default;
    /* handler == {} answers every call CallStatus::NoHandler (a declared stub). */
    FunctionDefinition(Node& n, std::string_view name,
                       const Schema* req_schema, const Schema* rsp_schema,
                       Handler handler, const FunctionOptions& o = {}) {
        init(n, name, req_schema, rsp_schema, std::move(handler), o, false);
    }
    /* The same, typed by the mesh (see reflect_from_mesh). */
    FunctionDefinition(Node& n, std::string_view name, reflect_from_mesh_t,
                       Handler handler, const FunctionOptions& o = {}) {
        init(n, name, nullptr, nullptr, std::move(handler), o, true);
    }
    /* A reflect_from_mesh handle: re-type in place when the mesh moved (true = re-typed). */
    bool refresh() { return fn_ && detail::rant_function_refresh(fn_) == 1; }

private:
    void init(Node& n, std::string_view name, const Schema* req_schema, const Schema* rsp_schema,
              Handler handler, const FunctionOptions& o, bool reflect) {
        if (!n.valid()) { priv::raise_msg("rant::FunctionDefinition: node is not valid"); return; }
        std::string nm(name);
        detail::RantFunctionOpts co;
        std::memset(&co, 0, sizeof co);
        co.backpressure_wait_us = o.backpressure_wait_us;
        co.timeout_us           = o.timeout_us;
        co.keep_last            = o.keep_last;
        co.reflect_from_mesh    = reflect ? 1 : 0;
        Box* box = nullptr;
        if (handler) { box = new Box(); box->h = std::move(handler); }
        fn_ = detail::rant_node_create_function_definition(n.impl_->node, nm.c_str(),
                  req_schema ? req_schema->raw() : nullptr,
                  rsp_schema ? rsp_schema->raw() : nullptr,
                  box ? &FunctionDefinition::tramp : nullptr, box, &co);
        if (!fn_) {
            delete box;
            priv::raise_last(n.impl_->node, "rant::FunctionDefinition create");
            return;
        }
        if (box) {
            box->fn.store(fn_);
            std::lock_guard<std::mutex> g(n.impl_->reg_mu);
            n.impl_->boxes.emplace_back(box);
        }
    }
public:

    bool valid() const noexcept { return fn_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }
    /* callers currently matched to this definition */
    int match_count() const { return fn_ ? detail::rant_function_match_count(fn_) : 0; }
    /* Retire the definition: park its channels and release the name for a successor. The
     * handle is empty after. Refused with State from a callback, and it stays valid then. */
    SendStatus retire() {
        if (!fn_) return SendStatus::NoTopic;
        int rc = detail::rant_function_retire(fn_);
        if (rc == 0) fn_ = nullptr;
        return static_cast<SendStatus>(rc);
    }

private:
    struct Box : priv::HandlerBox {
        Handler h;
        std::atomic<detail::RantFunction*> fn{ nullptr };
    };
    static void tramp(detail::RantRequest* rq, void* user) {
        Box* b = static_cast<Box*>(user);
        Request<Bytes> r(rq, b->fn.load());
#if defined(__cpp_exceptions)
        try { b->h(r); }
        catch (...) { detail::rant_request_fail(rq, "handler threw", detail::rant_bytes(nullptr, 0)); }
#else
        b->h(r);
#endif
    }
    detail::RantFunction* fn_ = nullptr;
    template <class A, class B> friend class FunctionDefinition;
};

/* RemoteFunction<Bytes, Bytes> (untyped): a reference to a definition on another node. */
template <> class RemoteFunction<Bytes, Bytes> {
public:
    RemoteFunction() = default;
    RemoteFunction(Node& n, std::string_view name,
                   const Schema* req_schema = nullptr, const Schema* rsp_schema = nullptr,
                   const FunctionOptions& o = {}) {
        init(n, name, req_schema, rsp_schema, o, false);
    }
    /* The same, typed by the mesh (see reflect_from_mesh). */
    RemoteFunction(Node& n, std::string_view name, reflect_from_mesh_t, const FunctionOptions& o = {}) {
        init(n, name, nullptr, nullptr, o, true);
    }
    /* A reflect_from_mesh handle: re type in place when the mesh moved. true = re typed, and
     * outstanding calls are answered Cancelled first. */
    bool refresh() { return fn_ && detail::rant_function_refresh(fn_) == 1; }

private:
    void init(Node& n, std::string_view name, const Schema* req_schema, const Schema* rsp_schema,
              const FunctionOptions& o, bool reflect) {
        if (!n.valid()) { priv::raise_msg("rant::RemoteFunction: node is not valid"); return; }
        std::string nm(name);
        detail::RantFunctionOpts co;
        std::memset(&co, 0, sizeof co);
        co.backpressure_wait_us = o.backpressure_wait_us;
        co.timeout_us           = o.timeout_us;
        co.keep_last            = o.keep_last;
        co.reflect_from_mesh    = reflect ? 1 : 0;
        fn_ = detail::rant_node_create_remote_function(n.impl_->node, nm.c_str(),
                  req_schema ? req_schema->raw() : nullptr,
                  rsp_schema ? rsp_schema->raw() : nullptr, &co);
        if (!fn_) { priv::raise_last(n.impl_->node, "rant::RemoteFunction create"); return; }
        impl_ = n.impl_.get();
    }
public:

    bool valid() const noexcept { return fn_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /* Blocking call: waits for the response or timeout_ms, negative = the default, on the
     * service thread's progress under start() and driving the loop otherwise. Refused with
     * State from a callback, use call_async there. */
    Response<Bytes> call(Bytes req, int timeout_ms = -1, const CallOptions& opts = {}) {
        Response<Bytes> r;
        if (!fn_) { r.ss_ = SendStatus::NoTopic; return r; }
        detail::RantResponse out;
        std::memset(&out, 0, sizeof out);
        detail::RantCallOpts co; std::memset(&co, 0, sizeof co);
        co.provider = opts.provider; co.id_out = opts.id_out;
        int rc = detail::rant_function_call(fn_, priv::to_c(req), &out, timeout_ms, &co);
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
    /* Async form: returns once the request is committed. on_response fires once with the
     * outcome on the polling thread. */
    SendStatus call_async(Bytes req, std::function<void(const ResponseView<Bytes>&)> on_response,
                          const CallOptions& opts = {}) {
        if (!fn_) return SendStatus::NoTopic;
        priv::AsyncBox* box = new priv::AsyncBox{ std::move(on_response),
                                                  &impl_->reg_mu, &impl_->async_live, {} };
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
    int  match_count()    const { return fn_ ? detail::rant_function_match_count(fn_) : 0; }
    /* Retire the remote: park its channels and release the name. Every outstanding call
     * completes Cancelled. The handle is empty after. Refused with State from a callback. */
    SendStatus retire() {
        if (!fn_) return SendStatus::NoTopic;
        int rc = detail::rant_function_retire(fn_);
        if (rc == 0) fn_ = nullptr;
        return static_cast<SendStatus>(rc);
    }

private:
    /* wrap a node-owned function handle (the @rant/meta endpoint): callable, never
     * destroyed by us (functions are never torn down before the node). */
    RemoteFunction(detail::RantFunction* fn, Node::Impl* impl) : fn_(fn), impl_(impl) {}
    static void async_tramp(const detail::RantResponse* r) {
        priv::AsyncBox* box = static_cast<priv::AsyncBox*>(r->user);
        {
            std::lock_guard<std::mutex> g(*box->mu);
            box->live->erase(box);
        }
        if (box->cb) {
            ResponseView<Bytes> rv(r);
#if defined(__cpp_exceptions)
            try { box->cb(rv); } catch (...) {}   /* never unwind into the C poll */
#else
            box->cb(rv);
#endif
        }
        delete box;
    }
    detail::RantFunction* fn_ = nullptr;
    Node::Impl*             impl_ = nullptr;
    template <class A, class B> friend class RemoteFunction;
    friend class Node;
};

/* Node::meta / meta_request: out-of-line so RemoteFunction<Bytes, Bytes> is a complete type here. */
inline RemoteFunction<Bytes, Bytes> Node::meta() {
    if (!valid()) return RemoteFunction<Bytes, Bytes>();
    return RemoteFunction<Bytes, Bytes>(detail::rant_node_meta_function(impl_->node), impl_.get());
}
inline SendStatus Node::meta_request(uint32_t peer,
                                     std::function<void(const MetaSnapshot&)> cb, uint32_t sections) {
    RemoteFunction<Bytes, Bytes> m = meta();
    if (!m.valid()) return SendStatus::NoTopic;
    uint8_t buf[4]; Bytes req;   /* the mask lives on the stack: call_async commits it now */
    if (sections) {
        buf[0] = (uint8_t)(sections);       buf[1] = (uint8_t)(sections >> 8);
        buf[2] = (uint8_t)(sections >> 16); buf[3] = (uint8_t)(sections >> 24);
        req = Bytes(buf, 4);
    }
    return m.call_async(req, [cb = std::move(cb)](const ResponseView<Bytes>& r) {
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
    /* handler == {} answers every call CallStatus::NoHandler (a declared stub). */
    TaskDefinition(Node& n, std::string_view name, const Schema* req_schema,
                   const Schema* prg_schema, const Schema* rsp_schema,
                   Handler handler, const TaskOptions& o = {}) {
        init(n, name, req_schema, prg_schema, rsp_schema, std::move(handler), o, false);
    }
    /* The same, typed by the mesh (see reflect_from_mesh). */
    TaskDefinition(Node& n, std::string_view name, reflect_from_mesh_t,
                   Handler handler, const TaskOptions& o = {}) {
        init(n, name, nullptr, nullptr, nullptr, std::move(handler), o, true);
    }
    /* A reflect_from_mesh handle: re-type in place when the mesh moved (true = re-typed). */
    bool refresh() { return fn_ && detail::rant_function_refresh(fn_) == 1; }

private:
    void init(Node& n, std::string_view name, const Schema* req_schema, const Schema* prg_schema,
              const Schema* rsp_schema, Handler handler, const TaskOptions& o, bool reflect) {
        if (!n.valid()) { priv::raise_msg("rant::TaskDefinition: node is not valid"); return; }
        std::string nm(name);
        detail::RantTaskOpts co;
        std::memset(&co, 0, sizeof co);
        co.progress_best_effort = o.progress_best_effort ? 1 : 0;
        co.progress_keep_last   = o.progress_keep_last;
        co.keep_last            = o.keep_last;
        co.no_cancel            = o.no_cancel ? 1 : 0;
        co.exclusive            = o.exclusive ? 1 : 0;
        co.multi                = o.multi ? 1 : 0;
        co.timeout_us           = o.timeout_us;
        co.backpressure_wait_us = o.backpressure_wait_us;
        co.reflect_from_mesh    = reflect ? 1 : 0;
        Box* box = nullptr;
        if (handler) { box = new Box(); box->h = std::move(handler); }
        fn_ = detail::rant_node_create_task_definition(n.impl_->node, nm.c_str(),
                  req_schema ? req_schema->raw() : nullptr,
                  prg_schema ? prg_schema->raw() : nullptr,
                  rsp_schema ? rsp_schema->raw() : nullptr,
                  box ? &TaskDefinition::tramp : nullptr, box, &co);
        if (!fn_) {
            delete box;
            priv::raise_last(n.impl_->node, "rant::TaskDefinition create");
            return;
        }
        impl_ = n.impl_.get();
        if (box) {
            box->fn.store(fn_);
            std::lock_guard<std::mutex> g(impl_->reg_mu);
            impl_->boxes.emplace_back(box);
        }
    }
public:

    bool valid() const noexcept { return fn_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }
    /* callers currently matched to this definition */
    int match_count() const { return fn_ ? detail::rant_function_match_count(fn_) : 0; }

    /* Cancel notification, one slot, {} clears. Fires on the poll thread with the cancelled
     * call's defer token. Optional, since polling PendingTask::cancelled is complete alone. */
    void on_cancel(std::function<void(uint64_t token)> h) {
        if (!fn_) return;
        CancelBox* box = nullptr;
        if (h) { box = new CancelBox(); box->h = std::move(h); }
        detail::rant_function_on_cancel(fn_, box ? &TaskDefinition::cancel_tramp : nullptr, box);
        if (box) {   /* kept alive until node close, like every handler box */
            std::lock_guard<std::mutex> g(impl_->reg_mu);
            impl_->boxes.emplace_back(box);
        }
    }

    /* Retire the definition. Every live deferred call answers Cancelled first, so a RUNNING
     * caller never hangs. Complete or drop PendingTask handles before. Refused from a callback. */
    SendStatus retire() {
        if (!fn_) return SendStatus::NoTopic;
        int rc = detail::rant_function_retire(fn_);
        if (rc == 0) fn_ = nullptr;
        return static_cast<SendStatus>(rc);
    }

private:
    struct Box : priv::HandlerBox {
        Handler h;
        std::atomic<detail::RantFunction*> fn{ nullptr };
    };
    struct CancelBox : priv::HandlerBox {
        std::function<void(uint64_t)> h;
    };
    static void tramp(detail::RantRequest* rq, void* user) {
        Box* b = static_cast<Box*>(user);
        TaskRequest<Bytes, Bytes> r(rq, b->fn.load());
#if defined(__cpp_exceptions)
        try { b->h(r); }
        catch (...) {   /* a no-op if the handler already deferred: one answer per call */
            detail::rant_request_fail(rq, "handler threw", detail::rant_bytes(nullptr, 0));
        }
#else
        b->h(r);
#endif
    }
    static void cancel_tramp(uint64_t token, void* user) {
        CancelBox* b = static_cast<CancelBox*>(user);
#if defined(__cpp_exceptions)
        try { b->h(token); } catch (...) {}   /* never unwind into the C poll */
#else
        b->h(token);
#endif
    }
    detail::RantFunction* fn_ = nullptr;
    Node::Impl*             impl_ = nullptr;
    template <class A, class B, class C> friend class TaskDefinition;
};

/* The untyped reference to a task definition elsewhere. A request is always directed at
 * one provider. The timeout bounds only the first response, cancel() is the tool after. */
template <> class RemoteTask<Bytes, Bytes, Bytes> {
public:
    using ProgressHandler = std::function<void(const ProgressView<Bytes>&)>;

    RemoteTask() = default;
    RemoteTask(Node& n, std::string_view name, const Schema* req_schema = nullptr,
               const Schema* prg_schema = nullptr, const Schema* rsp_schema = nullptr,
               const TaskOptions& o = {}) {
        init(n, name, req_schema, prg_schema, rsp_schema, o, false);
    }
    /* The same, typed by the mesh (see reflect_from_mesh). */
    RemoteTask(Node& n, std::string_view name, reflect_from_mesh_t, const TaskOptions& o = {}) {
        init(n, name, nullptr, nullptr, nullptr, o, true);
    }
    /* A reflect_from_mesh handle: re type in place when the mesh moved. true = re typed, and
     * outstanding calls are answered Cancelled first. */
    bool refresh() { return fn_ && detail::rant_function_refresh(fn_) == 1; }

private:
    void init(Node& n, std::string_view name, const Schema* req_schema, const Schema* prg_schema,
              const Schema* rsp_schema, const TaskOptions& o, bool reflect) {
        if (!n.valid()) { priv::raise_msg("rant::RemoteTask: node is not valid"); return; }
        std::string nm(name);
        detail::RantTaskOpts co;
        std::memset(&co, 0, sizeof co);
        co.progress_best_effort = o.progress_best_effort ? 1 : 0;
        co.progress_keep_last   = o.progress_keep_last;
        co.keep_last            = o.keep_last;
        co.timeout_us           = o.timeout_us;
        co.backpressure_wait_us = o.backpressure_wait_us;
        co.reflect_from_mesh    = reflect ? 1 : 0;
        fn_ = detail::rant_node_create_remote_task(n.impl_->node, nm.c_str(),
                  req_schema ? req_schema->raw() : nullptr,
                  prg_schema ? prg_schema->raw() : nullptr,
                  rsp_schema ? rsp_schema->raw() : nullptr, &co);
        if (!fn_) { priv::raise_last(n.impl_->node, "rant::RemoteTask create"); return; }
        impl_ = n.impl_.get();
    }
public:

    bool valid() const noexcept { return fn_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /* Blocking call: waits for the terminal outcome, with on_progress on this thread, on
     * the service thread's progress under start() and driving the loop otherwise. Refused
     * from a callback. id_out allows a cancel(). */
    Response<Bytes> call(Bytes req, ProgressHandler on_progress = {}, int timeout_ms = -1,
                    const CallOptions& opts = {}) {
        Response<Bytes> r;
        if (!fn_) { r.ss_ = SendStatus::NoTopic; return r; }
        detail::RantResponse out;
        std::memset(&out, 0, sizeof out);
        detail::RantCallOpts co; std::memset(&co, 0, sizeof co);
        co.provider = opts.provider; co.id_out = opts.id_out;
        if (on_progress) {   /* fires only inside rant_function_call: the stack copy holds */
            co.on_progress   = &RemoteTask::blocking_progress_tramp;
            co.progress_user = &on_progress;
        }
        int rc = detail::rant_function_call(fn_, priv::to_c(req), &out, timeout_ms, &co);
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
    /* Async form: returns once committed, with the call id for cancel(). on_progress fires
     * per update and on_response once, on the polling thread, which frees the call state. */
    TaskCall call_async(Bytes req, ProgressHandler on_progress,
                        std::function<void(const ResponseView<Bytes>&)> on_response,
                        const CallOptions& opts = {}) {
        TaskCall tc;
        if (!fn_) return tc;
        priv::AsyncBox* box = new priv::AsyncBox{ std::move(on_response),
                                                  &impl_->reg_mu, &impl_->async_live,
                                                  std::move(on_progress) };
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
        if (!fn_) return SendStatus::NoTopic;
        return static_cast<SendStatus>(detail::rant_function_cancel(fn_, call_id));
    }

    /* definitions currently matched, 0 = nobody provides the name yet */
    int  match_count()    const { return fn_ ? detail::rant_function_match_count(fn_) : 0; }
    /* Retire the remote. Every outstanding call completes Cancelled. The handle is empty
     * after, and refused with State from a callback. */
    SendStatus retire() {
        if (!fn_) return SendStatus::NoTopic;
        int rc = detail::rant_function_retire(fn_);
        if (rc == 0) fn_ = nullptr;
        return static_cast<SendStatus>(rc);
    }

private:
    static void blocking_progress_tramp(const detail::RantProgress* p) {
        ProgressHandler* h = static_cast<ProgressHandler*>(p->user);
        ProgressView<Bytes> pv(p);
#if defined(__cpp_exceptions)
        try { (*h)(pv); } catch (...) {}   /* never unwind into the C poll */
#else
        (*h)(pv);
#endif
    }
    static void async_progress_tramp(const detail::RantProgress* p) {
        priv::AsyncBox* box = static_cast<priv::AsyncBox*>(p->user);
        ProgressView<Bytes> pv(p);
#if defined(__cpp_exceptions)
        try { box->on_progress(pv); } catch (...) {}
#else
        box->on_progress(pv);
#endif
    }
    static void async_response_tramp(const detail::RantResponse* r) {
        priv::AsyncBox* box = static_cast<priv::AsyncBox*>(r->user);
        {
            std::lock_guard<std::mutex> g(*box->mu);
            box->live->erase(box);
        }
        if (box->cb) {
            ResponseView<Bytes> rv(r);
#if defined(__cpp_exceptions)
            try { box->cb(rv); } catch (...) {}   /* never unwind into the C poll */
#else
            box->cb(rv);
#endif
        }
        delete box;
    }
    detail::RantFunction* fn_ = nullptr;
    Node::Impl*             impl_ = nullptr;
    template <class A, class B, class C> friend class RemoteTask;
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
    VariableDefinition(Node& n, std::string_view name, const Schema* schema,
                       const VariableOptions<Bytes>& o = {}) {
        if (!n.valid()) { priv::raise_msg("rant::VariableDefinition: node is not valid"); return; }
        create(n, name, schema, o, /*definition=*/true, false);
    }
    /* The same, typed by the mesh (see reflect_from_mesh). */
    VariableDefinition(Node& n, std::string_view name, reflect_from_mesh_t,
                       const VariableOptions<Bytes>& o = {}) {
        if (!n.valid()) { priv::raise_msg("rant::VariableDefinition: node is not valid"); return; }
        create(n, name, nullptr, o, /*definition=*/true, true);
    }
    /* A reflect_from_mesh handle: re-type in place when the mesh moved (true = re-typed). */
    bool refresh() { return var_ && detail::rant_variable_refresh(var_) == 1; }

    bool valid() const noexcept { return var_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    /* the current value, copied out under the node lock (nullopt = none yet) */
    std::optional<std::vector<uint8_t>> get() const {
        if (!var_) return std::nullopt;
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
        if (!var_) return SendStatus::NoTopic;
        return static_cast<SendStatus>(detail::rant_variable_set(var_, priv::to_c(value)));
    }
    /* Force the value: writes are absorbed into the shadow source until unforce, which
     * restores the latest absorbed set. Requires VariableOptions::allow_force: State on the
     * owner without it, BadRole on a remote whose owner advertises none. */
    SendStatus force(Bytes value) {
        if (!var_) return SendStatus::NoTopic;
        return static_cast<SendStatus>(detail::rant_variable_force(var_, priv::to_c(value)));
    }
    SendStatus unforce() {
        if (!var_) return SendStatus::NoTopic;
        return static_cast<SendStatus>(detail::rant_variable_unforce(var_));
    }
    bool forced() const { return var_ && detail::rant_variable_forced(var_) == 1; }
    /* the handles matched to this one: remotes at a definition, the definition at a remote */
    int match_count() const { return var_ ? detail::rant_variable_match_count(var_) : 0; }

    /* Observe. on_change replays the current value at registration and fires on every state
     * change, on_write on every applied write, both inline on the applying thread. {} clears. */
    void on_change(std::function<void(const VariableUpdate&)> h) { observe(std::move(h), true); }
    void on_write (std::function<void(const VariableUpdate&)> h) { observe(std::move(h), false); }

    /* Retire the handle: park its channels and release the name, else a re created same name
     * handle is shadowed by the live twin. Empty after, refused with State from a callback. */
    SendStatus retire() {
        if (!var_) return SendStatus::NoTopic;
        int rc = detail::rant_variable_retire(var_);
        if (rc == 0) var_ = nullptr;
        return static_cast<SendStatus>(rc);
    }

protected:
    struct UBox : priv::HandlerBox {
        std::function<void(const VariableUpdate&)> h;
        Node::Impl* impl = nullptr;
    };
    static void utramp(const detail::RantVariableUpdate* u, void* user) {
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
        (void)(change ? detail::rant_variable_on_change(var_, box ? &VariableDefinition::utramp : nullptr, box)
                      : detail::rant_variable_on_write (var_, box ? &VariableDefinition::utramp : nullptr, box));
        if (box) {   /* kept alive until node close, like every handler box */
            std::lock_guard<std::mutex> g(impl_->reg_mu);
            impl_->boxes.emplace_back(box);
        }
    }
    void create(Node& n, std::string_view name, const Schema* schema,
                const VariableOptions<Bytes>& o, bool definition, bool reflect) {
        std::string nm(name);
        detail::RantVariableOpts co;
        std::memset(&co, 0, sizeof co);
        co.initial     = priv::to_c(o.initial);
        co.access      = o.read_only ? detail::RANT_VAR_READONLY : detail::RANT_VAR_READWRITE;
        co.allow_force = o.allow_force ? 1 : 0;
        co.catch_up    = o.catch_up;
        co.keep_last   = o.keep_last;
        co.backpressure_wait_us = o.backpressure_wait_us;
        co.reflect_from_mesh = reflect ? 1 : 0;
        node_ = n.impl_->node;
        impl_ = n.impl_.get();
        var_ = definition
            ? detail::rant_node_create_variable_definition(node_, nm.c_str(),
                  schema ? schema->raw() : nullptr, &co)
            : detail::rant_node_create_remote_variable(node_, nm.c_str(),
                  schema ? schema->raw() : nullptr, &co);
        if (!var_) priv::raise_last(node_, definition ? "rant::VariableDefinition create"
                                                      : "rant::RemoteVariable create");
    }
    detail::RantVariable* var_ = nullptr;
    detail::RantNode*       node_ = nullptr;
    Node::Impl*             impl_ = nullptr;
    template <class A> friend class VariableDefinition;
};

/* The untyped accessor of a value owned elsewhere: reads see the cached latest, writes go
 * over the set channel with no response. */
template <> class RemoteVariable<Bytes> : public VariableDefinition<Bytes> {
public:
    RemoteVariable() = default;
    RemoteVariable(Node& n, std::string_view name, const Schema* schema,
                   const VariableOptions<Bytes>& o = {}) {
        if (!n.valid()) { priv::raise_msg("rant::RemoteVariable: node is not valid"); return; }
        create(n, name, schema, o, /*definition=*/false, false);
    }
    /* The same, typed by the mesh (see reflect_from_mesh). */
    RemoteVariable(Node& n, std::string_view name, reflect_from_mesh_t,
                   const VariableOptions<Bytes>& o = {}) {
        if (!n.valid()) { priv::raise_msg("rant::RemoteVariable: node is not valid"); return; }
        create(n, name, nullptr, o, /*definition=*/false, true);
    }
    /* Block (driving the node loop) until a value exists or timeout_ms elapses. */
    bool wait(int timeout_ms) { return var_ && detail::rant_variable_wait(var_, timeout_ms) == 1; }
};

#endif /* !RANT_NO_PATTERNS */

/* ====================== PUB/SUB (raw cores) ================================ */

/* Publisher<Bytes>: the raw publish side. schema is the DSL schema the bytes follow, or
 * null for untyped bytes. Send a MessageBuilder or any Bytes. */
template <> class Publisher<Bytes> {
public:
    Publisher() = default;
    Publisher(Node& n, std::string_view name, const Schema* schema = nullptr,
              const Qos& qos = {})
        : t_(n, name, Role::PubOnly, schema, qos) {}

    bool valid() const noexcept { return t_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /* capture is when the data was true, as against when it was sent. The default is
     * unstated, which costs no wire bytes. */
    SendStatus send(Bytes data, Timestamp capture = {}) { return t_.send(data, capture); }
    /* subscribers currently matched */
    int  match_count()      const { return t_.match_count(); }
    /* true when a send would not wait: a subscriber is matched or matching has converged */
    bool ready()            const { return t_.ready(); }
    /* wait until every reader has acked, the flush before close. false = timeout_ms passed */
    bool drain(int timeout_ms)    { return t_.drain(timeout_ms); }
    /* free the name for another schema. Every handle sharing the name goes invalid */
    SendStatus retire()           { return t_.retire(); }

private:
    priv::TopicCore t_;
};

/* Subscriber<Bytes>: the raw subscribe side. The handler fires per message on the polling
 * thread with a MessageView that reads fields by name through the schema. */
template <> class Subscriber<Bytes> {
public:
    Subscriber() = default;
    Subscriber(Node& n, std::string_view name, const Schema* schema,
               Node::MessageHandler on_message, const Qos& qos = {})
        : t_(n, name, Role::SubOnly, schema, qos) {
        if (t_.valid() && on_message) add_handler(n, t_, std::move(on_message));
    }

    bool valid() const noexcept { return t_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /* publishers currently matched */
    int  match_count()  const { return t_.match_count(); }
    /* free the name for another schema. Every handle sharing the name goes invalid */
    SendStatus retire()       { return t_.retire(); }

private:
    static void add_handler(Node& n, priv::TopicCore& t, Node::MessageHandler h) {
        Node::Impl* impl = n.impl_.get();
        std::lock_guard<std::mutex> g(impl->reg_mu);
        auto& slot = impl->sub_handlers[t.index()];
        auto nv = slot ? std::make_shared<std::vector<Node::MessageHandler>>(*slot)
                       : std::make_shared<std::vector<Node::MessageHandler>>();
        nv->push_back(std::move(h));
        slot = std::move(nv);
    }
    priv::TopicCore t_;
    template <class A> friend class Subscriber;
};

/* Typed sugar: thin template layers over the Bytes cores using the RANT_SCHEMA codec.
 * Every handle stays thin and non owning. */

#ifndef RANT_NO_PATTERNS

/* The typed view of a request inside a full form function handler. Wraps the untyped
 * Request<Bytes> for the callback lifetime and adds the typed reply. */
template <class Rsp> class Request {
public:
    explicit Request(Request<Bytes>& core) : core_(core) {}
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
    Request<Bytes>& core_;
};

/* Deferred<Rsp>: the typed parked reply. */
template <class Rsp> class Deferred {
public:
    Deferred() = default;
    Deferred(Deferred<Bytes>&& core) : core_(std::move(core)) {}
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
    Deferred<Bytes> core_;
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
    /* human-readable outcome text (owned): see Response<Bytes>::message */
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
    template <class A, class B, class C> friend class RemoteTask;
};

/* The typed async outcome, valid for the callback. */
template <class Rsp> class ResponseView {
public:
    CallStatus status()     const { return st_; }
    bool       ok()         const { return st_ == CallStatus::Ok; }
    uint32_t   provider()   const { return provider_; }
    uint64_t   written_us()    const { return written_; }
    /* human-readable outcome text (a view, callback lifetime): see ResponseView<Bytes>::message */
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
    template <class A, class B, class C> friend class RemoteTask;
};

/* The typed implementation side. Handler forms: Rsp(const Req&), the return value is the
 * reply, or void(const Req&, Request<Rsp>&). A throwing handler answers AppError. */
template <class Req, class Rsp> class FunctionDefinition {
public:
    FunctionDefinition() = default;
    template <class H>
    FunctionDefinition(Node& n, std::string_view name, H&& handler,
                       const FunctionOptions& o = {}) {
        const Schema* rq = priv::schema_of<Req>();
        const Schema* rs = priv::schema_of<Rsp>();
        if (!rq || !rs) { priv::raise_msg("rant::FunctionDefinition: RANT_SCHEMA compile failed"); return; }
        core_ = FunctionDefinition<Bytes, Bytes>(n, name, rq, rs, adapt(std::forward<H>(handler)), o);
    }
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }
    int match_count() const { return core_.match_count(); }
    SendStatus retire() { return core_.retire(); }

private:
    template <class H>
    static typename FunctionDefinition<Bytes, Bytes>::Handler adapt(H&& h) {
        if constexpr (std::is_invocable_v<std::decay_t<H>&, const Req&, Request<Rsp>&>) {
            return [f = std::forward<H>(h)](Request<Bytes>& u) mutable {
                Req q{};
                if (!priv::decode(q, u.data(), u.raw_schema())) { u.fail("request decode failed"); return; }
                Request<Rsp> tr(u);
                f(q, tr);
            };
        } else if constexpr (std::is_invocable_v<std::decay_t<H>&, const Req&>) {
            static_assert(std::is_convertible_v<
                              std::invoke_result_t<std::decay_t<H>&, const Req&>, Rsp>,
                          "simple function handler must return Rsp");
            return [f = std::forward<H>(h)](Request<Bytes>& u) mutable {
                Req q{};
                if (!priv::decode(q, u.data(), u.raw_schema())) { u.fail("request decode failed"); return; }
                Rsp r = f(q);
                std::vector<uint8_t> s;
                u.reply(priv::encode(r, s));
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
    RemoteFunction(Node& n, std::string_view name, const FunctionOptions& o = {}) {
        const Schema* rq = priv::schema_of<Req>();
        const Schema* rs = priv::schema_of<Rsp>();
        if (!rq || !rs) { priv::raise_msg("rant::RemoteFunction: RANT_SCHEMA compile failed"); return; }
        core_ = RemoteFunction<Bytes, Bytes>(n, name, rq, rs, o);
    }
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /* Blocking call, see RemoteFunction<Bytes, Bytes>::call. Decodes into the owning Response. */
    Response<Rsp> call(const Req& req, int timeout_ms = -1, const CallOptions& opts = {}) {
        std::vector<uint8_t> s;
        Response<Bytes> ur = core_.call(priv::encode(req, s), timeout_ms, opts);
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
            [cb = std::move(cb)](const ResponseView<Bytes>& uv) {
                ResponseView<Rsp> tv;
                tv.st_ = uv.status(); tv.provider_ = uv.provider(); tv.written_ = uv.written_us();
                tv.message_ = uv.message();
                if (uv.ok()) (void)priv::decode(tv.v_, uv.data(), uv.raw_schema());
                cb(tv);
            }, opts);
    }

    int  match_count()    const { return core_.match_count(); }
    SendStatus retire() { return core_.retire(); }

private:
    RemoteFunction<Bytes, Bytes> core_;
};

/* The typed view of a request inside a task handler. Wraps the untyped TaskRequest<Bytes, Bytes> for
 * the callback lifetime and adds the typed verbs. */
template <class Prg, class Rsp> class TaskRequest {
public:
    explicit TaskRequest(TaskRequest<Bytes, Bytes>& core) : core_(core) {}
    Bytes            data()        const { return core_.data(); }
    uint32_t         caller()      const { return core_.caller(); }
    std::string_view caller_name() const { return core_.caller_name(); }
    std::string_view task_name()   const { return core_.task_name(); }
    uint64_t         recv_us()     const { return core_.recv_us(); }
    uint64_t         written_us()  const { return core_.written_us(); }

    void reply(const Rsp& v) {
        std::vector<uint8_t> s;
        core_.reply(priv::encode(v, s));
    }
    void reply(Bytes raw) { core_.reply(raw); }
    void fail(std::string_view message = {}, Bytes raw = {}) { core_.fail(message, raw); }
    SendStatus start() { return core_.start(); }
    PendingTask<Prg, Rsp> defer() { return PendingTask<Prg, Rsp>(core_.defer()); }

private:
    TaskRequest<Bytes, Bytes>& core_;
};

/* PendingTask<Prg,Rsp>: the typed deferred task (see the untyped core's contract). */
template <class Prg, class Rsp> class PendingTask {
public:
    PendingTask() = default;
    PendingTask(PendingTask<Bytes, Bytes>&& core) : core_(std::move(core)) {}
    PendingTask(PendingTask&&) noexcept = default;
    PendingTask& operator=(PendingTask&&) noexcept = default;
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    SendStatus progress(const Prg& v) {
        std::vector<uint8_t> s;
        return core_.progress(priv::encode(v, s));
    }
    bool cancelled() const { return core_.cancelled(); }
    SendStatus complete(const Rsp& v, std::string_view message = {}) {
        std::vector<uint8_t> s;
        return core_.complete(priv::encode(v, s), message);
    }
    SendStatus fail(std::string_view message = {}) { return core_.fail(message); }
    SendStatus complete_cancelled(std::string_view message = {}) { return core_.complete_cancelled(message); }
    /* honor a cancel and still carry a typed partial result */
    SendStatus complete_cancelled(std::string_view message, const Rsp& partial) {
        std::vector<uint8_t> s;
        return core_.complete_cancelled(message, priv::encode(partial, s));
    }

private:
    PendingTask<Bytes, Bytes> core_;
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
    template <class H>
    TaskDefinition(Node& n, std::string_view name, H&& handler, const TaskOptions& o = {}) {
        const Schema* rq = priv::schema_of<Req>();
        const Schema* pg = priv::schema_of<Prg>();
        const Schema* rs = priv::schema_of<Rsp>();
        if (!rq || !pg || !rs) { priv::raise_msg("rant::TaskDefinition: RANT_SCHEMA compile failed"); return; }
        core_ = TaskDefinition<Bytes, Bytes, Bytes>(n, name, rq, pg, rs, adapt(std::forward<H>(handler)), o);
    }
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }
    int match_count() const { return core_.match_count(); }
    void on_cancel(std::function<void(uint64_t token)> h) { core_.on_cancel(std::move(h)); }
    SendStatus retire() { return core_.retire(); }

private:
    template <class H>
    static typename TaskDefinition<Bytes, Bytes, Bytes>::Handler adapt(H&& h) {
        static_assert(std::is_invocable_v<std::decay_t<H>&, const Req&, TaskRequest<Prg, Rsp>&>,
                      "task handler must be void(const Req&, rant::TaskRequest<Prg,Rsp>&)");
        return [f = std::forward<H>(h)](TaskRequest<Bytes, Bytes>& u) mutable {
            Req q{};
            if (!priv::decode(q, u.data(), u.raw_schema())) { u.fail("request decode failed"); return; }
            TaskRequest<Prg, Rsp> tr(u);
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
    RemoteTask(Node& n, std::string_view name, const TaskOptions& o = {}) {
        const Schema* rq = priv::schema_of<Req>();
        const Schema* pg = priv::schema_of<Prg>();
        const Schema* rs = priv::schema_of<Rsp>();
        if (!rq || !pg || !rs) { priv::raise_msg("rant::RemoteTask: RANT_SCHEMA compile failed"); return; }
        core_ = RemoteTask<Bytes, Bytes, Bytes>(n, name, rq, pg, rs, o);
    }
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    /* Blocking call, see RemoteTask<Bytes, Bytes, Bytes>::call. on_progress fires typed while it waits. */
    Response<Rsp> call(const Req& req, ProgressHandler on_progress = {},
                       int timeout_ms = -1, const CallOptions& opts = {}) {
        std::vector<uint8_t> s;
        Response<Bytes> ur = core_.call(priv::encode(req, s), adapt_progress(std::move(on_progress)),
                                   timeout_ms, opts);
        Response<Rsp> r;
        r.st_ = ur.status(); r.ss_ = ur.send_status(); r.provider_ = ur.provider();
        r.written_ = ur.written_us();
        r.message_.assign(ur.message());   /* own it: ur dies with this frame */
        if (ur.ok()) (void)priv::decode(r.v_, ur.data(), ur.raw_schema());
        return r;
    }
    /* Async form (see RemoteTask<Bytes, Bytes, Bytes>::call_async): returns the send status + call id. */
    TaskCall call_async(const Req& req, ProgressHandler on_progress,
                        std::function<void(const ResponseView<Rsp>&)> on_response,
                        const CallOptions& opts = {}) {
        std::vector<uint8_t> s;
        return core_.call_async(priv::encode(req, s), adapt_progress(std::move(on_progress)),
            [cb = std::move(on_response)](const ResponseView<Bytes>& uv) {
                if (!cb) return;
                ResponseView<Rsp> tv;
                tv.st_ = uv.status(); tv.provider_ = uv.provider(); tv.written_ = uv.written_us();
                tv.message_ = uv.message();
                if (uv.ok()) (void)priv::decode(tv.v_, uv.data(), uv.raw_schema());
                cb(tv);
            }, opts);
    }
    /* Cancel the outstanding call (see RemoteTask<Bytes, Bytes, Bytes>::cancel). */
    SendStatus cancel(uint32_t call_id) { return core_.cancel(call_id); }

    int  match_count()    const { return core_.match_count(); }
    SendStatus retire() { return core_.retire(); }

private:
    static RemoteTask<Bytes, Bytes, Bytes>::ProgressHandler adapt_progress(ProgressHandler h) {
        if (!h) return {};
        return [f = std::move(h)](const ProgressView<Bytes>& up) mutable {
            ProgressView<Prg> tp;
            tp.call_id_ = up.call_id(); tp.provider_ = up.provider();
            tp.written_ = up.written_us(); tp.recv_ = up.recv_us();
            tp.has_ = up.has_value();
            if (tp.has_ && !priv::decode(tp.v_, up.data(), up.raw_schema())) return;
            f(tp);
        };
    }
    RemoteTask<Bytes, Bytes, Bytes> core_;
};

/* VariableDefinition<T>: the typed authoritative value. */
template <class T> class VariableDefinition {
public:
    VariableDefinition() = default;
    VariableDefinition(Node& n, std::string_view name, const VariableOptions<T>& o = {}) {
        const Schema* sc = priv::schema_of<T>();
        if (!sc) { priv::raise_msg("rant::VariableDefinition: RANT_SCHEMA compile failed"); return; }
        std::vector<uint8_t> scratch;
        VariableOptions<Bytes> uo;
        if (o.initial) uo.initial = priv::encode(*o.initial, scratch);
        uo.read_only = o.read_only; uo.allow_force = o.allow_force;
        uo.catch_up = o.catch_up; uo.backpressure_wait_us = o.backpressure_wait_us;
        core_ = VariableDefinition<Bytes>(n, name, sc, uo);
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
    VariableDefinition<Bytes> core_;
};

/* RemoteVariable<T>: the typed accessor of a value owned elsewhere. */
template <class T> class RemoteVariable {
public:
    RemoteVariable() = default;
    RemoteVariable(Node& n, std::string_view name, const VariableOptions<T>& o = {}) {
        const Schema* sc = priv::schema_of<T>();
        if (!sc) { priv::raise_msg("rant::RemoteVariable: RANT_SCHEMA compile failed"); return; }
        VariableOptions<Bytes> uo;
        uo.catch_up = o.catch_up; uo.backpressure_wait_us = o.backpressure_wait_us;
        core_ = RemoteVariable<Bytes>(n, name, sc, uo);
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
    RemoteVariable<Bytes> core_;
};

#endif /* !RANT_NO_PATTERNS */

/* Publisher<T>: the typed publish side. */
template <class T> class Publisher {
public:
    Publisher() = default;
    Publisher(Node& n, std::string_view name, const Qos& qos = {}) {
        const Schema* sc = priv::schema_of<T>();
        if (!sc) { priv::raise_msg("rant::Publisher: RANT_SCHEMA compile failed"); return; }
        core_ = Publisher<Bytes>(n, name, sc, qos);
    }
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    SendStatus send(const T& v, Timestamp capture = {}) {
        std::vector<uint8_t> s;
        return core_.send(priv::encode(v, s), capture);
    }
    int  match_count()   const { return core_.match_count(); }
    bool ready()         const { return core_.ready(); }
    bool drain(int timeout_ms) { return core_.drain(timeout_ms); }
    SendStatus retire()        { return core_.retire(); }

private:
    Publisher<Bytes> core_;
};

/* The typed subscribe side. Handler forms: void(const T&) or void(const T&, const
 * MessageView&), fired per message on the polling thread with the decoded value. */
template <class T> class Subscriber {
public:
    Subscriber() = default;
    template <class H, class = std::enable_if_t<
        std::is_invocable_v<std::decay_t<H>&, const T&> ||
        std::is_invocable_v<std::decay_t<H>&, const T&, const MessageView&>>>
    Subscriber(Node& n, std::string_view name, H&& handler, const Qos& qos = {}) {
        const Schema* sc = priv::schema_of<T>();
        if (!sc) { priv::raise_msg("rant::Subscriber: RANT_SCHEMA compile failed"); return; }
        core_ = Subscriber<Bytes>(n, name, sc, adapt(std::forward<H>(handler)), qos);
    }
    bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }

    int  match_count() const { return core_.match_count(); }
    SendStatus retire()      { return core_.retire(); }

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
    Subscriber<Bytes> core_;
};

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
RANT_ENUM(rant::ImageFormat, Mono8, Mono16, Rgb8, Rgba8, Bgr8, Yuyv, Nv12, Monof32, Jpeg, Png);
RANT_ENUM(rant::VideoCodec, Unknown, Mjpeg, H264, H265, Av1);
RANT_ENUM(rant::VideoStreamKind, Rtsp, WebrtcWhep, Hls, Srt, Rtp, HttpMjpeg, Other);
RANT_STD_STRUCT(Image); RANT_STD_STRUCT(VideoFrame); RANT_STD_STRUCT(ExternalVideoStream);
RANT_ENUM(rant::DistortionModel, NoDistortion, BrownConrady, Fisheye, Rational);
RANT_STD_STRUCT(CameraIntrinsics);
RANT_STD_STRUCT(JointState); RANT_STD_STRUCT(JointNames);

#endif /* C++ consumer (not the implementation anchor) */
#endif /* RANT_HPP_INCLUDED */
