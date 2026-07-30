/* DART serialize: a standalone schema + (de)serialization layer. Both ends share the
 * schema, so the wire carries no type tags or field names; a value's meaning is its
 * position in the byte stream. FIXED fields (scalars, capped strings, fixed arrays,
 * structs) pack first with static offsets, so their reads are O(1), zero-copy,
 * zero-allocation. VARIABLE-length fields (`string`, `elem[]`, `map`) live in a tail
 * after the fixed section, each as one [u32 len][payload] frame in schema order, so a
 * message stays one contiguous buffer and locating variable field k is a short
 * type-agnostic walk (read a length, hop, k times). Only the map's payload is
 * self-describing (a tagged value tree: the JSON-style escape from the schema); variable
 * strings and arrays get their typing from the schema like every other field. Building
 * and parsing a schema take a DartAllocFn hook (common/alloc.h); pass a node's and you
 * inherit its static/dynamic memory. The message read/write path allocates nothing.
 * A schema's root need not be a struct: the whole schema may be ONE BARE TYPE (`bool`,
 * `f32[]`, `string<64>`, `map`, an enum), so a topic that publishes a bool is just `bool`
 * and not a one-field wrapper struct. Such a root is ANONYMOUS (no type name: types are
 * structural, so the same bare type in any language is byte-identical wire and the same
 * hash) and compiles to a single field named "", addressed by the empty path.
 * Depends only on common/, so it is usable on its own. */
#ifndef DART_SCHEMA_H
#define DART_SCHEMA_H

#include <stddef.h>
#include <stdint.h>
#include "../common/string.h"   /* DartString, DartBytes */
#include "../common/alloc.h"    /* DartAllocFn */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DART_SCHEMA_WIRE_VERSION
#define DART_SCHEMA_WIRE_VERSION 7u    /* bumped on any schema wire-format change (7: any-kind root) */
#endif
#ifndef DART_SCHEMA_MAX_DEPTH
#define DART_SCHEMA_MAX_DEPTH 8u        /* struct nesting the builder accepts */
#endif

/* The type kinds; the enum value is the kind byte on the wire. Scalars are
 * little-endian with no padding, so a fixed field's offset is the sum of the preceding
 * fixed sizes. A fixed array is exactly-N elements with no hidden framing; a live count,
 * if wanted, is just another field the schema declares. A capped string (`string<10>`)
 * is a fixed slot of [u16 live-length][cap bytes], so it carries its own length yet
 * keeps a static footprint. The VARIABLE kinds (VSTR/VARR/MAP) have no fixed footprint:
 * each occupies one [u32 len][payload] frame in the message tail (frames in schema
 * order), is allowed at the top level only (not inside a nested struct, which stays one
 * contiguous fixed block), and reports offset/size 0 in DartSchemaFieldInfo. */
typedef enum {
    DART_U8 = 0, DART_U16 = 1, DART_U32 = 2, DART_U64 = 3,
    DART_I8 = 4, DART_I16 = 5, DART_I32 = 6, DART_I64 = 7,
    DART_F32 = 8, DART_F64 = 9, DART_BOOL = 10,   /* 0..10: fixed scalars */
    DART_ARR    = 11,   /* fixed array of a scalar or string: [u8 elem][u16 count] (+[u16 cap] if elem is STR) */
    DART_STRUCT = 12,   /* [u8 nfields] ( [u8 namelen][name][type] )*                                          */
    DART_STR    = 13,   /* capped string: [u16 cap]; in a message, [u16 len][cap bytes]                        */
    DART_VSTR   = 14,   /* variable string (`string`): its bytes are the field's tail frame                    */
    DART_VARR   = 15,   /* variable array (`elem[]`): [u8 elem] (+[u16 cap] if elem is STR); the frame holds a
                           live count of packed elements (count = frame len / element size)                    */
    DART_MAP    = 16,   /* self-describing map (`map`): the frame holds a tagged value tree (see the map API) */
    DART_ENUM   = 17    /* named integer: [u8 backing (a U8..I64 kind)][u16 n]( [value:backing][u8 namelen][name] )*
                           FIXED field, on the wire just its backing scalar; the name table is schema-only.
                           dart_schema_field_at reports elem = the backing kind, count = the variant count.    */
} DartSchemaTypeKind;

/* Bytes of a fixed scalar kind (U8..BOOL); 0 otherwise. */
uint32_t dart_schema_scalar_size(DartSchemaTypeKind kind);

/* The compiled schema (hot-path form): wire bytes, identity hash, message size, and a
 * precomputed offset per top-level field. One block allocated via the caller's hook; free
 * it with dart_schema_free. */
typedef struct DartSchema DartSchema;

/* Read-only view of one field, filled by dart_schema_field_at. The compiled schema
 * flattens EVERY field at every depth into one depth-first table (a struct's members
 * directly follow it, one level deeper), so reflection walks nested structures without
 * recursion and offsets are always message-absolute. */
typedef struct {
    DartString name;      /* field's own name (into the schema's wire bytes) */
    uint8_t    kind;      /* DartSchemaTypeKind */
    uint8_t    elem;      /* ARR element kind, or ENUM backing kind, else 0 */
    uint16_t   count;     /* ARR element count, or ENUM variant count, else 0 */
    uint16_t   depth;     /* 0 = top level; n = member of the struct n levels up */
    uint16_t   str_cap;   /* string capacity (STR fields and STR-element arrays), else 0 */
    uint32_t   offset;    /* absolute byte offset of this field in a message; 0 for variable kinds */
    uint32_t   size;      /* byte size of this field; 0 for variable kinds (live, per message) */
} DartSchemaFieldInfo;

/* One option of an enum field: a wire number and its human name. Passed to the builder;
 * read back with dart_schema_enum_variant. */
typedef struct {
    int64_t     value;
    const char *name;     /* NUL-terminated (builder input); <= 255 bytes */
} DartEnumVariant;

/* Builder: define a schema and get back a compiled DartSchema. It grows its wire buffer
 * through the alloc hook as you add fields; finish resizes that block to hold the compiled
 * schema and returns it (free with dart_schema_free, same hook). A field's index is its
 * creation order (its id on read). On OOM or misuse the builder latches an error and finish
 * returns NULL (freeing anything it allocated). Always finish a builder you began.
 *
 *   DartSchemaBuilder b = dart_schema_begin(alloc, user, "Pose");
 *   dart_schema_field(&b, "x", DART_F64);
 *   dart_schema_field(&b, "y", DART_F64);
 *   dart_schema_field_array(&b, "id", DART_U8, 16);
 *   DartSchema *pose = dart_schema_finish(&b);
 */
typedef struct {
    DartAllocFn alloc; void *user;                 /* the memory hook and its context */
    uint8_t *buf;
    size_t   cap;
    size_t   len;                              /* wire bytes written so far */
    int      err;                              /* 0 ok; nonzero latches failure */
    uint8_t  value_root;                       /* 1 = a bare-type root (dart_schema_begin_value) */
    uint16_t depth;                            /* open structs (1 = root only) */
    size_t   count_pos[DART_SCHEMA_MAX_DEPTH]; /* wire offset of each open struct's nfields byte */
    uint16_t field_count[DART_SCHEMA_MAX_DEPTH];
} DartSchemaBuilder;

/* Compile a schema from its text form: one string, pasted verbatim on the writer and
 * every reader (a reader may declare just the subset of fields it uses).
 *
 *   "Pose\n"
 *   "{\n"
 *   "    stamp:    u64,\n"
 *   "    x:        f64,\n"
 *   "    y:        f64,\n"
 *   "    uuid:     u8[16],                  -- fixed array: exactly 16 bytes\n"
 *   "    tags:     u8[8],\n"
 *   "    tagCount: u8,                      -- a live count is just another field\n"
 *   "    frame:    string<24>,              -- capped string: up to 24 bytes, live length\n"
 *   "    labels:   string<16>[4],           -- array of 4 capped strings\n"
 *   "    velocity: { dx: f32, dy: f32 },    -- nested struct\n"
 *   "    note:     string,                  -- variable string: unbounded, in the tail\n"
 *   "    samples:  f32[],                   -- variable array: live element count\n"
 *   "    names:    string<16>[],            -- variable array of capped strings\n"
 *   "    extras:   map,                     -- self-describing tagged values (see map API)\n"
 *   "    mode:     enum<u8> { Idle=0, Run=1, Fault=2 }  -- named integer; wire = the u8\n"
 *   "}"
 *
 * Scalars: u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 bool. `name: elem[N]` is a fixed
 * array of exactly N elements; `name: string<C>` is a capped string (up to C bytes,
 * carrying its live length; `string<C>[N]` an array of them); `name: { ... }` nests a
 * struct. `name: enum<uN> { A=0, B, C=9 }` is a named integer with backing width uN (any
 * U8..I64 kind); values may be omitted (auto-increment from the previous, starting 0) and
 * the backing is part of the type's identity, like a string cap. The variable forms drop
 * the bound: `string` (unbounded string), `elem[]` and
 * `string<C>[]` (live element count), `map` (tagged value tree). Variable fields may be
 * declared anywhere but always live at the END of a message, in schema order, so every
 * fixed field keeps a static offset; they are refused inside nested structs (`string[]`
 * is also refused: ragged element sizes belong in a map). Commas between fields are
 * optional (fields self-delimit), whitespace is free, `--` comments run to end of line
 * (so C string literals using comments need their `\n`s, as above). Types are
 * structural: there are no named type references, because a peer's schema can only be
 * trusted by its shape.
 * The whole text may instead be ONE BARE TYPE, in the exact same spellings, for a topic
 * whose payload is a single value: "bool", "u8", "f64", "string", "string<64>", "f32[]",
 * "u8[16]", "string<8>[4]", "map", "enum<u8> { Idle, Run }". That root is anonymous:
 * naming it ("Temperature: f32" as the whole schema) is an error, because a bare type has
 * no identity beyond its shape, so the same bare type from any language is the same wire
 * bytes and the same hash. Its single field is named "" (see dart_schema_field_count).
 * A field's index is its order in the text. Identical text compiles to identical wire
 * bytes, hence the same hash on both ends. Free with dart_schema_free. On any error
 * returns NULL and points *err (optional, may be NULL) at the offending character. */
DartSchema *dart_schema_compile(DartAllocFn alloc, void *user, const char *text, const char **err);

DartSchemaBuilder dart_schema_begin(DartAllocFn alloc, void *user, const char *root_name);
/* Begin a BARE-TYPE schema: the root is one anonymous value instead of a struct. Add
 * EXACTLY ONE field with an empty name (any of the field functions below except
 * dart_schema_begin_struct: a struct root comes from dart_schema_begin), then finish.
 *
 *   DartSchemaBuilder b = dart_schema_begin_value(alloc, user);
 *   dart_schema_field(&b, "", DART_BOOL);
 *   DartSchema *flag = dart_schema_finish(&b);
 */
DartSchemaBuilder dart_schema_begin_value(DartAllocFn alloc, void *user);
/* A fixed scalar field (U8..BOOL). */
void        dart_schema_field(DartSchemaBuilder *b, const char *name, DartSchemaTypeKind kind);
/* Fixed array of exactly `count` scalar elements. */
void        dart_schema_field_array(DartSchemaBuilder *b, const char *name,
                                    DartSchemaTypeKind elem_scalar, uint16_t count);
/* Capped string: up to `cap` bytes plus a live length (`string<cap>`; cap >= 1). */
void        dart_schema_field_string(DartSchemaBuilder *b, const char *name, uint16_t cap);
/* Fixed array of exactly `count` capped strings (`string<cap>[count]`). */
void        dart_schema_field_string_array(DartSchemaBuilder *b, const char *name,
                                           uint16_t cap, uint16_t count);
/* Variable string (`string`): unbounded, rides the message tail. Top level only. */
void        dart_schema_field_var_string(DartSchemaBuilder *b, const char *name);
/* Variable array (`elem[]`): a live count of packed scalar elements. Top level only. */
void        dart_schema_field_var_array(DartSchemaBuilder *b, const char *name,
                                        DartSchemaTypeKind elem_scalar);
/* Variable array of capped strings (`string<cap>[]`): whole [u16 len][cap] slots. */
void        dart_schema_field_var_string_array(DartSchemaBuilder *b, const char *name,
                                               uint16_t cap);
/* Self-describing map (`map`): write with DartMapWriter, read with dart_map_get. */
void        dart_schema_field_map(DartSchemaBuilder *b, const char *name);
/* Named integer (`enum<uN>{...}`): a fixed field carrying `backing` (a U8..I64 kind) on
 * the wire, with a schema-side table of `n` {value, name} options (n <= 65535). Read the
 * number with dart_get_int/uint, the label with dart_get_enum; enumerate the options with
 * dart_schema_enum_count / dart_schema_enum_variant. Latches an error if backing is not an
 * integer kind or a value does not fit it. */
void        dart_schema_field_enum(DartSchemaBuilder *b, const char *name,
                                   DartSchemaTypeKind backing,
                                   const DartEnumVariant *variants, uint16_t n);
/* Nested struct field: open it, add its fields, close it. */
void        dart_schema_begin_struct(DartSchemaBuilder *b, const char *name);
void        dart_schema_end_struct(DartSchemaBuilder *b);
/* Close the root struct, compile, and return the schema (NULL on any latched error). */
DartSchema *dart_schema_finish(DartSchemaBuilder *b);

/* Deserialize a received schema into a compiled one, allocated via the hook. wire may be
 * malformed, so this bounds-checks and returns NULL on overrun or version mismatch. The
 * bytes are copied in, so wire need not outlive the call. Free with dart_schema_free. */
DartSchema *dart_schema_parse(const void *wire, size_t wire_len, DartAllocFn alloc, void *user);
/* Free a schema from dart_schema_finish / dart_schema_parse (same hook it was made with). */
void        dart_schema_free(DartSchema *s, DartAllocFn alloc, void *user);

DartBytes   dart_schema_wire(const DartSchema *s);        /* canonical bytes (advertise these) */
uint64_t    dart_schema_hash(const DartSchema *s);        /* 64-bit identity (FNV-1a over wire) */
DartString  dart_schema_name(const DartSchema *s);        /* root type name ("" for a bare type) */
/* Spell the schema back as compile-ready DSL text (the inverse of dart_schema_compile):
 * "Name {\n  field: type,\n  nested: {\n    ...\n  }\n}\n", or just "bool\n" for a bare-type
 * root. Recompiles to the same wire
 * (hence hash). Writes up to cap bytes, always NUL-terminated when cap > 0, and returns the
 * FULL length excluding the NUL, so dart_schema_print(s, NULL, 0) measures for sizing. */
uint32_t    dart_schema_print(const DartSchema *s, char *buf, size_t cap);
/* Fixed-section size: the exact message size when the schema has no variable fields,
 * otherwise where the variable tail starts. */
uint32_t    dart_schema_size(const DartSchema *s);
/* Smallest valid message: the fixed section plus one empty frame per variable field
 * (== dart_schema_size with none). Size a message buffer as this plus room for the
 * variable content you will set. */
uint32_t    dart_schema_msg_min(const DartSchema *s);
/* Live length of the message in buf (fixed section + its frames): what you hand to
 * send. 0 on a malformed or uninitialized buffer (always start a message from
 * dart_schema_message_default, which writes every frame). */
uint32_t    dart_schema_msg_len(const DartSchema *s, const void *buf, size_t cap);
/* Fields in the flattened depth-first table (EVERY depth; a field's index is its order
 * of appearance in the schema text, nested members included). A bare-type root is one
 * field, named "" (so the getters/setters address it with the empty path). */
uint16_t    dart_schema_field_count(const DartSchema *s);
int         dart_schema_field_at(const DartSchema *s, uint16_t i, DartSchemaFieldInfo *out); /* 1 + fills out, else 0 */
/* Resolve a field by name; nested members by dotted path ("velocity.dx"). Returns the
 * flat index, or -1. Fields are addressed by NAME everywhere (the getters/setters take
 * the same paths); resolve once and use dart_get/set_value if a hot path measures it. */
int         dart_schema_field_index(const DartSchema *s, const char *path);

/* Enum options (by flat field index, as from dart_schema_field_index / dart_schema_field_at).
 * enum_count is the option count (0 if the field is not an enum); enum_variant fills option
 * i's wire number + name (a view into the schema bytes), returning 1, or 0 if i is out of
 * range / not an enum. Together they enumerate an enum's choices (e.g. to build a dropdown). */
uint16_t    dart_schema_enum_count  (const DartSchema *s, uint16_t field);
int         dart_schema_enum_variant(const DartSchema *s, uint16_t field, uint16_t i,
                                     int64_t *value, DartString *name);
/* The two resolution directions, over the schema alone (no message). name_of returns the
 * option name for a wire number ({NULL,0} if no option has it: an unknown/newer value is
 * safe, not an error); value_of returns 1 + *out for a known name, else 0. */
DartString  dart_enum_name_of (const DartSchema *s, uint16_t field, int64_t value);
int         dart_enum_value_of(const DartSchema *s, uint16_t field, const char *name, int64_t *out);

/* 1 if msg is exactly one message of this schema: its length equals the schema size,
 * or, with variable fields, the tail frames exactly consume it. */
int       dart_schema_validate(const DartSchema *s, DartBytes msg);

/* Reader/writer structural compatibility: 1 if a reader declaring `sub` can read
 * messages written with `pub`. Same root name, and every sub field must exist in pub
 * under the same name with the same type (a nested struct field must match exactly).
 * The subset applies at the top level: field order and extra pub fields are free.
 * With a bare-type root the two roots' types must match by the same per-kind rules a
 * field uses; a bare type against a struct is a mismatch (there is nothing to subset). */
int dart_schema_subset(const DartSchema *sub, const DartSchema *pub);
/* dart_schema_subset with a reason: on refusal (returns 0) writes the first
 * incompatibility into buf as one line, e.g. "field 'position': reader f32[8],
 * writer f64[8]" (always NUL-terminated, truncated to cap; buf may be NULL to skip
 * the text). Returns 1 with buf untouched when sub can read pub. */
int dart_schema_subset_why(const DartSchema *sub, const DartSchema *pub,
                           char *buf, size_t cap);
/* The reader's view of a publisher's layout: sub's fields (names, order, indices) with
 * pub's offsets, and pub's message size (so dart_schema_validate matches the
 * publisher's messages). Requires dart_schema_subset(sub, pub); NULL otherwise or on
 * OOM. Free with dart_schema_free. */
DartSchema *dart_schema_rebase(const DartSchema *sub, const DartSchema *pub,
                               DartAllocFn alloc, void *user);

/* Scalar getters: read a field BY NAME (nested members by dotted path: "velocity.dx"),
 * widened. Pick the family matching its kind; a mismatch or unknown field yields 0.
 * Read in place, no copy. */
uint64_t  dart_get_uint(DartBytes msg, const DartSchema *s, const char *field);  /* U8..U64, BOOL */
int64_t   dart_get_int (DartBytes msg, const DartSchema *s, const char *field);  /* I8..I64        */
double    dart_get_f64 (DartBytes msg, const DartSchema *s, const char *field);  /* F64 (or F32)   */
float     dart_get_f32 (DartBytes msg, const DartSchema *s, const char *field);  /* F32            */
/* ARR: a zero-copy view of the whole array (count * element size bytes). VARR: a view
 * of the LIVE elements (its length is a whole multiple of the element size; divide for
 * the count). {NULL,0} on mismatch. Element kind via dart_schema_field_at. */
DartBytes dart_get_array(DartBytes msg, const DartSchema *s, const char *field);
/* STR or VSTR: a zero-copy view of the live bytes (capped slots clamp a hostile length
 * prefix so a hostile message can never over-read); {NULL,0} on mismatch. */
DartString dart_get_string(DartBytes msg, const DartSchema *s, const char *field);
/* One element of a string array (fixed or variable); {NULL,0} on mismatch or
 * index >= the (live) count. */
DartString dart_get_string_at(DartBytes msg, const DartSchema *s, const char *field,
                              uint16_t index);
/* MAP: a zero-copy view of the map body (feed it to dart_map_get / dart_map_at);
 * {NULL,0} on mismatch. An empty body is an empty map. */
DartBytes dart_get_map(DartBytes msg, const DartSchema *s, const char *field);
/* ENUM: the current value's option name ({NULL,0} if the stored number has no option, or
 * the field is not an enum). The raw number is dart_get_int/dart_get_uint on the same field. */
DartString dart_get_enum(DartBytes msg, const DartSchema *s, const char *field);

/* The canonical default message: every fixed field is zero (numeric 0, false, zeroed
 * arrays and structs) and every variable field an empty frame (empty string, 0
 * elements, empty map). Writes exactly dart_schema_msg_min bytes into buf; 1, or 0 if
 * cap is too small. ALWAYS start a message from this, then set the fields you care
 * about: the variable-field setters rely on every frame existing. */
int dart_schema_message_default(const DartSchema *s, void *buf, size_t cap);

/* Setters (the writer mirror of the getters): write one field BY NAME (dotted paths for
 * nested members) of a message being built in buf[0..cap). Values narrow like a C cast.
 * Start from dart_schema_message_default so the bytes are canonical (and so the
 * variable tail is well-formed). A variable-field setter RESIZES its frame in place,
 * memmoving the rest of the tail (appending in schema order costs nothing; the source
 * must not alias buf); it returns 0 if the new total would exceed cap. Returns 1; 0 on
 * a kind mismatch, unknown field, or short buffer. Send the finished message with
 * length dart_schema_msg_len. */
int dart_set_uint(void *buf, size_t cap, const DartSchema *s, const char *field, uint64_t v); /* U8..U64, BOOL */
int dart_set_int (void *buf, size_t cap, const DartSchema *s, const char *field, int64_t v);  /* I8..I64        */
int dart_set_f64 (void *buf, size_t cap, const DartSchema *s, const char *field, double v);   /* F64 (or F32)   */
int dart_set_f32 (void *buf, size_t cap, const DartSchema *s, const char *field, float v);    /* F32            */
/* ARR: copy elems over the front of the array and zero the rest. VARR: the frame
 * becomes exactly elems (the live count = elems.len / element size). elems.len is
 * bytes, must be a multiple of the element size and fit (never silently truncated).
 * For a string array, elems is whole raw slots ([u16 len][cap bytes] each, len <= cap). */
int dart_set_array(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes elems);
/* STR: write the live length and bytes, zeroing the slot's unused tail; v.len must fit
 * the field's cap (never silently truncated). VSTR: the frame becomes exactly v. */
int dart_set_string(void *buf, size_t cap, const DartSchema *s, const char *field, DartString v);
/* One element of a string array (fixed or variable); a variable array's index must be
 * under its LIVE count (grow it first with dart_set_array). */
int dart_set_string_at(void *buf, size_t cap, const DartSchema *s, const char *field,
                       uint16_t index, DartString v);
/* MAP: the frame becomes the given map body (from dart_map_finish, or another
 * message's dart_get_map). The body is validated first: malformed bytes are refused. */
int dart_set_map(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes map);
/* ENUM: write the option named `name` (resolved to its number via the schema). Returns 1,
 * or 0 for an unknown name / non-enum field. Set by number with dart_set_int/dart_set_uint. */
int dart_set_enum(void *buf, size_t cap, const DartSchema *s, const char *field, const char *name);

/* Reflection access by flat index (tools walking a schema they've never seen: the
 * explorer, loggers, bridges). One tagged value covers every kind; the typed name-based
 * getters/setters above stay the API for code that knows its fields. */
typedef struct {
    uint8_t  kind;        /* DartSchemaTypeKind */
    uint8_t  elem;        /* ARR/VARR element kind, or ENUM backing kind */
    uint16_t count;       /* ARR element count; VARR live count and MAP entry count
                             (saturated at 65535: bytes.len is authoritative); ENUM variant count */
    uint16_t str_cap;     /* string capacity (STR fields and STR-element arrays) */
    union { uint64_t u; int64_t i; double f; } v;   /* scalar value (BOOL in u as 0/1; ENUM value in i, also u) */
    DartBytes bytes;      /* ARR/STRUCT raw bytes (get: view into msg; set: source, may be
                             shorter than the field: the rest is zeroed). STR/VSTR: the
                             LIVE string bytes (get: view; set: content). VARR: the live
                             elements. MAP: the map body (set: validated first) */
} DartValue;
int dart_get_value(DartBytes msg, const DartSchema *s, uint16_t field, DartValue *out);
int dart_set_value(void *buf, size_t cap, const DartSchema *s, uint16_t field, const DartValue *val);

/* ---- the map: a self-describing tagged value tree (the escape from the schema) ------
 * A `map` field's frame holds a MAP BODY; everything little-endian, no padding:
 *   body  := [u16 n] entry*n          entry := [u8 keylen][key bytes] value
 *   value := [u8 kind] payload
 *     U8..BOOL : the scalar's bytes (size implied by the kind)
 *     VSTR     : [u16 len][bytes]
 *     VARR     : [u16 n] value*n      (heterogeneous, like a JSON array)
 *     MAP      : a nested body
 * Self-describing like JSON (a reader needs no schema for the content), but binary,
 * unambiguous, and one bounded pass to parse; no other kind byte may appear. An empty
 * frame is an empty map. Nesting is capped at DART_SCHEMA_MAX_DEPTH.
 *
 * Write a body with the cursor below (caller buffer, no allocation; errors latch and
 * finish returns 0): key order is yours, duplicate keys are not checked. Inside an
 * open array pass key = NULL. Integers store in the smallest kind that fits; readers
 * widen. Then dart_set_map the finished body into a message.
 *
 *   DartMapWriter w = dart_map_begin(tmp, sizeof tmp);
 *   dart_map_put_uint(&w, "battery", 87);
 *   dart_map_put_string(&w, "state", dart_cstr("docked"));
 *   dart_map_open_array(&w, "temps");
 *       dart_map_put_f64(&w, NULL, 36.2); dart_map_put_f64(&w, NULL, 34.9);
 *   dart_map_close(&w);
 *   dart_set_map(msg, sizeof msg, s, "extras", dart_bytes(tmp, dart_map_finish(&w)));
 */
typedef struct {
    uint8_t *buf; size_t cap, len;             /* the caller's buffer and write position */
    int      err;                              /* 0 ok; nonzero latches failure */
    uint16_t depth;                            /* open bodies (1 = root map only) */
    size_t   count_pos[DART_SCHEMA_MAX_DEPTH]; /* offset of each open body's count */
    uint16_t count[DART_SCHEMA_MAX_DEPTH];
    uint8_t  is_arr[DART_SCHEMA_MAX_DEPTH];
} DartMapWriter;

DartMapWriter dart_map_begin(void *buf, size_t cap);   /* opens the root map */
int dart_map_put_uint  (DartMapWriter *w, const char *key, uint64_t v);
int dart_map_put_int   (DartMapWriter *w, const char *key, int64_t v);
int dart_map_put_f64   (DartMapWriter *w, const char *key, double v);
int dart_map_put_f32   (DartMapWriter *w, const char *key, float v);
int dart_map_put_bool  (DartMapWriter *w, const char *key, int v);
int dart_map_put_string(DartMapWriter *w, const char *key, DartString v);
int dart_map_open_map  (DartMapWriter *w, const char *key);   /* nested map: open/close */
int dart_map_open_array(DartMapWriter *w, const char *key);   /* array: elements keyless */
int dart_map_close     (DartMapWriter *w);                    /* closes the innermost open */
uint32_t dart_map_finish(DartMapWriter *w);                   /* body length; 0 on any error */

/* Readers over a (possibly hostile) map body: every walk is bounds-checked, unknown
 * kinds and over-deep nesting fail cleanly. A nested map/array value comes back in
 * DartValue.bytes ready to feed to these same functions. */
uint16_t dart_map_count(DartBytes map);                       /* entry count (0 if empty/short) */
int dart_map_at (DartBytes map, uint16_t index, DartString *key, DartValue *out);
int dart_map_get(DartBytes map, const char *key, DartValue *out);   /* 1 + fills out, else 0 */
uint16_t dart_map_array_count(DartBytes arr);                 /* element count of an array value */
int dart_map_array_at(DartBytes arr, uint16_t index, DartValue *out);
int dart_map_valid(DartBytes map);   /* full structural check (dart_set_map runs this) */

#ifdef __cplusplus
}
#endif
#endif /* DART_SCHEMA_H */
