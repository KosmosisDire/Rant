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
 *     auto node = dart::Node::open("robot1",
 *         [](const dart::MessageIn& m){
 *             std::printf("%.*s > %.*s\n",
 *                 (int)m.publisher_name().size(), m.publisher_name().data(),
 *                 (int)m.text().size(),        m.text().data());
 *         },
 *         [](const dart::Event& e){ std::fprintf(stderr, "event: %s\n", e.to_string().c_str()); },
 *         { .domain = 7 });                  // std::optional
 *     if (!node) return 1;
 *     auto ch = node->create_topic("chat", dart::Role::PubSub, nullptr,
 *                                    { .reliability = dart::Reliability::Reliable });
 *     node->start();                         // background service thread owns the loop
 *     for (;;) ch.send("hello");             // thread-safe; or skip start() and poll(1) yourself
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
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
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
    NameCollision, QosIncompatible, SchemaMismatch, InterestOverflow,
    MetaTruncatedInterest, MetaTruncatedSchema, PeerMetaTooBig, MessageTooBig,
    PeerRefused, EvictedUnsent,
    Oom, Platform, Socket, Bind, McastJoin, Send, Recv, Poll, Waker
};

/* Schema field kinds for reflection (Schema::Field); values match the C wire.
 * Array/String are FIXED (offset-based); VString/VArray/Map are the VARIABLE kinds
 * that ride the message tail (offset/size report 0). */
enum class FieldType : uint8_t {
    U8 = 0, U16, U32, U64, I8, I16, I32, I64, F32, F64, Bool, Array, Struct, String,
    VString, VArray, Map
};

static_assert((int)Reliability::Reliable == detail::DART_RELIABLE, "reliability enum drift");
static_assert((int)Role::Inactive == detail::DART_INACTIVE, "role enum drift");
static_assert((int)SendStatus::NoSys == detail::DART_ERR_NOSYS, "result enum drift");
static_assert((int)EventKind::Error == detail::DART_ERROR, "event enum drift");
static_assert((int)ErrorKind::Waker == detail::DART_E_WAKER, "error enum drift");
static_assert((int)FieldType::Struct == detail::DART_STRUCT, "field-type enum drift");
static_assert((int)FieldType::String == detail::DART_STR, "field-type enum drift");
static_assert((int)FieldType::Map == detail::DART_MAP, "field-type enum drift");

/* forward decls */
class Node;
class Topic;
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
    uint32_t    shm_max_bytes        = 0;   /* pin topic to one same-host SHM size class */
    uint32_t    queue_bytes          = 0;   /* consumer-queue cap for take()/dispatch(); 0 = the
                                               queue appears lazily on first use, 1 MB cap */
};

struct NodeOptions {
    uint16_t                 domain               = 0;   /* logical-network selector */
    uint16_t                 max_topics         = 8;   /* how many topics may be created */
    bool                     disable_shm          = false;
    bool                     fetch_details        = false; /* greedily fetch every peer topic's
                                                              name + schema (observer UIs): fills
                                                              Peer::topics names via the cache */
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
     * max_topics plus message buffers; open returns nullopt if it is too
     * small. A typed topic copies its schema into this buffer, so pair it with
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
     * once create_topic has copied the schema into the node, the Schema and its
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
        uint16_t  str_cap;       /* string capacity (String fields and String-element arrays) */
        uint32_t  offset, size;
    };
    bool field_at(uint16_t i, Field& out) const {
        detail::DartSchemaFieldInfo f;
        if (!detail::dart_schema_field_at(schema_, i, &f)) return false;
        out.name   = { f.name.data, f.name.len };
        out.kind   = static_cast<FieldType>(f.kind);
        out.elem   = static_cast<FieldType>(f.elem);
        out.count  = f.count; out.depth = f.depth;
        out.str_cap = f.str_cap;
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

/* ---- map: the self-describing tagged value tree (a `map` field's content) ------
 * Thin OOP over the C map codec (the real DartMapWriter / dart_map_* live in the
 * embedded library, so this wraps them directly -- no reimplementation). Write a
 * body with MapWriter, hand it to MessageOut::set_map; read one from
 * MessageIn::get_map as a MapReader, or decode a whole map into an owning std::map
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
    friend class MessageIn;
    friend class Topic;
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

/* MessageOut: a mutable message buffer bound to a Schema, for typed encoding.
 * Set fields by name (nested members by dotted path, e.g. "vel.dx"), then
 * pass it straight to Topic::send (it converts to Bytes). Variable-length fields
 * (`string`, `elem[]`, `map`) grow the buffer as needed; over-cap on a capped field
 * is refused and flips ok() to false (never a silent truncation). */
class MessageOut {
public:
    explicit MessageOut(const Schema& s) : schema_(s.raw()), buf_(detail::dart_schema_msg_min(s.raw())) {
        detail::dart_schema_message_default(schema_, buf_.data(), buf_.size());
    }
    MessageOut& set_uint (const char* field, uint64_t v) { ok_ &= detail::dart_set_uint (buf_.data(), buf_.size(), schema_, field, v) != 0; return *this; }
    MessageOut& set_int  (const char* field, int64_t  v) { ok_ &= detail::dart_set_int  (buf_.data(), buf_.size(), schema_, field, v) != 0; return *this; }
    MessageOut& set_f64  (const char* field, double   v) { ok_ &= detail::dart_set_f64  (buf_.data(), buf_.size(), schema_, field, v) != 0; return *this; }
    MessageOut& set_f32  (const char* field, float    v) { ok_ &= detail::dart_set_f32  (buf_.data(), buf_.size(), schema_, field, v) != 0; return *this; }
    MessageOut& set_bool (const char* field, bool     v) { ok_ &= detail::dart_set_uint (buf_.data(), buf_.size(), schema_, field, v ? 1u : 0u) != 0; return *this; }
    /* a capped OR variable string (dart_set_string handles both) */
    MessageOut& set_string(const char* field, std::string_view v) {
        grow_for(v.size());
        ok_ &= detail::dart_set_string(buf_.data(), buf_.size(), schema_, field, detail::dart_string(v.data(), v.size())) != 0;
        return *this;
    }
    /* one element of a string array (a variable string array must be grown first with
       a set_array of empty slots, per the C API) */
    MessageOut& set_string_at(const char* field, uint16_t index, std::string_view v) {
        grow_for(v.size() + 2);
        ok_ &= detail::dart_set_string_at(buf_.data(), buf_.size(), schema_, field, index, detail::dart_string(v.data(), v.size())) != 0;
        return *this;
    }
    /* a fixed OR variable array (raw element bytes; for a string array, whole
       [u16 len][cap] slots) */
    MessageOut& set_array(const char* field, Bytes elems) {
        grow_for(elems.size());
        ok_ &= detail::dart_set_array(buf_.data(), buf_.size(), schema_, field, detail::dart_bytes(elems.data(), elems.size())) != 0;
        return *this;
    }
    /* a `map` field, from a finished map body */
    MessageOut& set_map(const char* field, Bytes map_body) {
        grow_for(map_body.size());
        ok_ &= detail::dart_set_map(buf_.data(), buf_.size(), schema_, field, detail::dart_bytes(map_body.data(), map_body.size())) != 0;
        return *this;
    }
    MessageOut& set_map(const char* field, MapWriter& w) { return set_map(field, w.finish()); }

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

/* MessageIn: a delivered message. Non-owning; valid only inside the handler. */
class MessageIn {
public:
    std::string_view publisher_name()  const { return { msg_->publisher_name.data,  msg_->publisher_name.len  }; }
    uint32_t         publisher_id()    const { return msg_->publisher_id; }
    std::string_view topic_name() const { return { msg_->topic_name.data, msg_->topic_name.len }; }
    uint16_t         topic_index()   const { return msg_->topic_index; }
    Bytes            data()         const { return { msg_->data.data, msg_->data.len }; }
    std::string_view text()         const { return { reinterpret_cast<const char*>(msg_->data.data), msg_->data.len }; }
    bool             has_schema()   const { return msg_->schema != nullptr; }
    /* node monotonic us when the poll RECEIVED it (queued: at enqueue), so paced
     * consumers measure true arrival times, never their own cadence */
    uint64_t         recv_us()      const { return msg_->recv_us; }

    /* Typed field reads (only meaningful when has_schema()); by name / dotted path. */
    uint64_t get_uint (const char* field) const { return detail::dart_get_uint (msg_->data, msg_->schema, field); }
    int64_t  get_int  (const char* field) const { return detail::dart_get_int  (msg_->data, msg_->schema, field); }
    double   get_f64  (const char* field) const { return detail::dart_get_f64  (msg_->data, msg_->schema, field); }
    float    get_f32  (const char* field) const { return detail::dart_get_f32  (msg_->data, msg_->schema, field); }
    bool     get_bool (const char* field) const { return detail::dart_get_uint (msg_->data, msg_->schema, field) != 0; }
    Bytes    get_array(const char* field) const { auto a = detail::dart_get_array(msg_->data, msg_->schema, field); return { a.data, a.len }; }
    /* capped OR variable string; empty view on a mismatch */
    std::string_view get_string(const char* field) const { auto s = detail::dart_get_string(msg_->data, msg_->schema, field); return { s.data, s.len }; }
    std::string_view get_string_at(const char* field, uint16_t index) const { auto s = detail::dart_get_string_at(msg_->data, msg_->schema, field, index); return { s.data, s.len }; }
    /* a `map` field, as a reader over its body (valid for the handler) */
    MapReader get_map(const char* field) const { auto b = detail::dart_get_map(msg_->data, msg_->schema, field); return MapReader(Bytes{ b.data, b.len }); }

private:
    explicit MessageIn(const detail::DartMsg* m) : msg_(m) {}
    const detail::DartMsg* msg_;
    friend class Node;
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
    uint16_t         topic()        const { return ev_->topic; }
    /* our topic name for topic-scoped events, else empty */
    std::string_view topic_name()   const { return ev_->topic_name ? std::string_view(ev_->topic_name) : std::string_view{}; }
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

/* Topic: a lightweight handle (owned by the Node, stable for its life). */
class Topic {
public:
    Topic() = default;
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
    uint16_t index() const {
        if (!ch_) return 0xffff;
        return detail::dart_topic_index(ch_);
    }
    int match_count() const {
        if (!ch_) return 0;
        return detail::dart_topic_match_count(ch_);
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

    /* A taken message: owns the DartMsg struct by value; the views inside point into
     * the topic's ring and stay valid until the NEXT take/dispatch on the topic. */
    class TakenMessageIn {
    public:
        TakenMessageIn() = default;
        bool valid() const noexcept { return ok_; }
        explicit operator bool() const noexcept { return ok_; }
        std::string_view publisher_name()  const { return { m_.publisher_name.data,  m_.publisher_name.len  }; }
        uint32_t         publisher_id()    const { return m_.publisher_id; }
        std::string_view topic_name() const { return { m_.topic_name.data, m_.topic_name.len }; }
        uint16_t         topic_index()   const { return m_.topic_index; }
        Bytes            data()         const { return { m_.data.data, m_.data.len }; }
        std::string_view text()         const { return { reinterpret_cast<const char*>(m_.data.data), m_.data.len }; }
        bool             has_schema()   const { return m_.schema != nullptr; }
        uint64_t         recv_us()      const { return m_.recv_us; }   /* arrival stamp (see MessageIn) */
        uint64_t get_uint (const char* field) const { return detail::dart_get_uint (m_.data, m_.schema, field); }
        int64_t  get_int  (const char* field) const { return detail::dart_get_int  (m_.data, m_.schema, field); }
        double   get_f64  (const char* field) const { return detail::dart_get_f64  (m_.data, m_.schema, field); }
        float    get_f32  (const char* field) const { return detail::dart_get_f32  (m_.data, m_.schema, field); }
        bool     get_bool (const char* field) const { return detail::dart_get_uint (m_.data, m_.schema, field) != 0; }
        Bytes    get_array(const char* field) const { auto a = detail::dart_get_array(m_.data, m_.schema, field); return { a.data, a.len }; }
        std::string_view get_string(const char* field) const { auto s = detail::dart_get_string(m_.data, m_.schema, field); return { s.data, s.len }; }
        std::string_view get_string_at(const char* field, uint16_t index) const { auto s = detail::dart_get_string_at(m_.data, m_.schema, field, index); return { s.data, s.len }; }
        MapReader get_map(const char* field) const { auto b = detail::dart_get_map(m_.data, m_.schema, field); return MapReader(Bytes{ b.data, b.len }); }
    private:
        detail::DartMsg m_{};
        bool ok_ = false;
        friend class Topic;
    };

    /* Pop the next queued message. timeout_ms: 0 = just check, >0 = wait up to that
     * long, negative = wait indefinitely. Empty optional = nothing arrived in time.
     *     while (auto msg = scan.take()) render(msg->data()); */
    std::optional<TakenMessageIn> take(int timeout_ms = 0) {
        TakenMessageIn t;
        if (!ch_ || detail::dart_topic_take(ch_, &t.m_, timeout_ms) != 1) return std::nullopt;
        t.ok_ = true;
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

private:
    explicit Topic(detail::DartTopic* c) : ch_(c) {}
    detail::DartTopic* ch_ = nullptr;
    friend class Node;
};

/* Node: owns the DartNode, its memory, and the user callbacks.
 *
 * Thread-safe: every call is serialized by a node-level lock inside the C core.
 * Drive it either by calling poll() from your own loop, or via start(): a C-level
 * background service thread owns the loop and handlers then fire on it (never two
 * at once for one node). From inside a handler, send() and read-only queries are
 * allowed; poll/create_topic/set_role/drain/stop are refused (SendStatus::State
 * / no-op), never corrupting. */
class Node {
public:
    using MessageHandler = std::function<void(const MessageIn&)>;
    using EventHandler   = std::function<void(const Event&)>;

    /* Open a node. name = a human-readable label synced via discovery (empty =>
     * an auto "node-XXXXXXXX"). on_message/on_event are required (pass {} / nullptr
     * for either if truly not needed) so they are wired in before open() even
     * returns -- no early peer/error event is ever missed waiting for a deferred
     * on_message()/on_event() call; those below can still rebind them later.
     * Returns nullopt on failure. */
    static std::optional<Node> open(std::string_view name, MessageHandler on_message,
                                    EventHandler on_event, const NodeOptions& o = {}) {
        std::unique_ptr<Impl> impl(new Impl());
        impl->on_msg   = std::move(on_message);
        impl->on_event = std::move(on_event);
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
        co.domain        = o.domain;
        co.max_topics  = o.max_topics;
        co.disable_shm   = o.disable_shm ? 1 : 0;
        co.fetch_details = o.fetch_details ? 1 : 0;
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

    /* Why the most recent open() returned std::nullopt (there is no Node to query on
       failure): the formatted one-line reason, and its machine-readable ErrorKind. */
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
    ~Node() = default;   /* teardown lives in Impl::~Impl (so a move-assign tears down correctly too) */

    /* Rebind a handler set at open(). Rarely needed: open() already requires initial ones. */
    Node& on_message(MessageHandler h) { impl_->on_msg = std::move(h);   return *this; }
    Node& on_event  (EventHandler   h) { impl_->on_event = std::move(h); return *this; }

    /* Create a topic. schema = an optional typed schema (its bytes are copied
     * into the node, so the Schema need not outlive the topic). */
    Topic create_topic(std::string_view name, Role role = Role::PubSub,
                           const Schema* schema = nullptr, const Qos& qos = {}) {
        std::string nm(name);
        detail::DartTopicOpts co;
        std::memset(&co, 0, sizeof co);
        co.qos = to_c(qos);
        detail::DartTopic* ch = detail::dart_node_create_topic(
            impl_->node, nm.c_str(), static_cast<detail::DartRole>(role),
            schema ? schema->raw() : nullptr, &co);
        return Topic(ch);
    }

    /* Recover an already-created topic handle by its creation index. */
    Topic topic(uint16_t index) const {
        return Topic(detail::dart_node_topic(impl_->node, index));
    }

    /* One loop tick: drives discovery, RX, timers, and flushes queued TX. Blocks
     * up to timeout_ms in the socket wait (wakes early on RX or a send from another
     * thread; 0 = non-blocking). Not needed (and refused) while start() runs. */
    int poll(int timeout_ms = 0) {
        return detail::dart_node_poll(impl_->node, timeout_ms);
    }

    /* Run the C-level background service thread: it owns the loop and fires the
     * handlers; every Node/Topic call stays safe from any thread, and a send is
     * flushed immediately (a waker cuts the service's socket wait short). */
    bool start() { return detail::dart_node_start(impl_->node) == 0; }
    /* Stop and join the service thread (idempotent; implied by node teardown). */
    void stop()  { detail::dart_node_stop(impl_->node); }
    bool is_started() const { return detail::dart_node_is_started(impl_->node) == 1; }

    /* Dispatch every already-queued topic on the calling thread (see Topic::take/
     * dispatch): the one-liner for a frame-paced consumer that owns all the queues.
     * Waits up to timeout_ms for any queued topic to hold data. */
    int dispatch(int max_msgs = 0, int timeout_ms = 0) {
        return detail::dart_node_dispatch(impl_->node, max_msgs, timeout_ms);
    }

    /* A copied snapshot of the live peer table (safe to keep after the poll). */
    std::vector<Peer> peers() const {
        std::vector<Peer> out;
        uint16_t count = 0;
        /* the C view is zero-copy: hold the node lock across the copy so a poller
           on another thread cannot mutate it mid-read (no-op if we are that thread).
           RAII so a bad_alloc mid-copy cannot leak the lock and stall the node. */
        struct LockGuard {
            detail::DartNode* n;
            explicit LockGuard(detail::DartNode* node) : n(node) { detail::dart_node_lock(n); }
            ~LockGuard() { detail::dart_node_unlock(n); }
        } guard(impl_->node);
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
            detail::DartTopicEntry t;
            while (detail::dart_node_peer_interest_next(&p, &it, &t)) {
                /* the fetched name (NodeOptions::fetch_details fills the cache within an
                   RTT); the announce's 32-bit hash as a placeholder until it lands */
                detail::DartString nm = detail::dart_node_peer_topic_name(impl_->node, p.id, t.index);
                std::string name;
                if (nm.data) name.assign(nm.data, nm.len);
                else {
                    char hx[16];
                    std::snprintf(hx, sizeof hx, "0x%08x", (unsigned)t.hash);
                    name = hx;
                }
                peer.topics.push_back({ std::move(name), t.is_pub != 0, t.reliable != 0 });
            }
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
    /* Sends that evicted never-sent history after the bounded wait (the
     * ErrorKind::EvictedUnsent count): the send-burst/overload indicator. */
    uint32_t evicted_unsent() const { return detail::dart_node_evicted_unsent(impl_->node); }

    /* The most recent error this node reported (also delivered via on_event): the
     * formatted one-line message, and its machine-readable ErrorKind. */
    std::string last_error() const {
        detail::DartEvent e = detail::dart_last_error(impl_->node);
        char b[192]; return detail::dart_event_str(&e, b, sizeof b);
    }
    ErrorKind last_error_kind() const {
        return static_cast<ErrorKind>(detail::dart_last_error(impl_->node).error);
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
        c.queue_bytes          = q.queue_bytes;
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
