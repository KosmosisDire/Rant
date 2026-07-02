/* DART serialize: a standalone schema + (de)serialization layer. Both ends share the
 * schema, so the wire carries no type tags or field names; a value's meaning is its
 * position in the byte stream. Every schema is fully FIXED for now (variable-length and
 * map types are deferred), so a message has an exact size and every field a static offset:
 * reads are O(1), zero-copy, zero-allocation. Building and parsing a schema take a
 * DartAllocFn hook (common/alloc.h); pass a node's and you inherit its static/dynamic
 * memory. Depends only on common/, so it is usable on its own. */
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
#define DART_SCHEMA_WIRE_VERSION 2u    /* bumped on any schema wire-format change */
#endif
#ifndef DART_SCHEMA_MAX_DEPTH
#define DART_SCHEMA_MAX_DEPTH 8u        /* struct nesting the builder accepts */
#endif

/* The type kinds; the enum value is the kind byte on the wire. All fixed: scalars are
 * little-endian with no padding, so a field's offset is the sum of the preceding sizes.
 * An array is exactly-N scalars with no hidden framing; a live count, if wanted, is just
 * another field the schema declares (variable-length strings/arrays are deferred). */
typedef enum {
    DART_U8 = 0, DART_U16 = 1, DART_U32 = 2, DART_U64 = 3,
    DART_I8 = 4, DART_I16 = 5, DART_I32 = 6, DART_I64 = 7,
    DART_F32 = 8, DART_F64 = 9, DART_BOOL = 10,   /* 0..10: fixed scalars */
    DART_ARR    = 11,   /* fixed array of a scalar (uuid u8[16], cov f64[36]): [u8 elem][u16 count] */
    DART_STRUCT = 12    /* [u8 nfields] ( [u8 namelen][name][type] )*                               */
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
    uint8_t    elem;      /* ARR element kind, else 0 */
    uint16_t   count;     /* ARR element count, else 0 */
    uint16_t   depth;     /* 0 = top level; n = member of the struct n levels up */
    uint32_t   offset;    /* absolute byte offset of this field in a message */
    uint32_t   size;      /* byte size of this field */
} DartSchemaFieldInfo;

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
 *   "    velocity: { dx: f32, dy: f32 }     -- nested struct\n"
 *   "}"
 *
 * Scalars: u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 bool. `name: elem[N]` is a fixed
 * array of exactly N scalars; `name: { ... }` nests a struct. Commas between fields
 * are optional (fields self-delimit), whitespace is free, `--` comments run to end of
 * line (so C string literals using comments need their `\n`s, as above). Types are
 * structural: there are no named type references, because a peer's schema can only be
 * trusted by its shape (variable-length `string` is deferred).
 * A field's index is its order in the text. Identical text compiles to identical wire
 * bytes, hence the same hash on both ends. Free with dart_schema_free. On any error
 * returns NULL and points *err (optional, may be NULL) at the offending character. */
DartSchema *dart_schema_compile(DartAllocFn alloc, void *user, const char *text, const char **err);

DartSchemaBuilder dart_schema_begin(DartAllocFn alloc, void *user, const char *root_name);
/* A fixed scalar field (U8..BOOL). */
void        dart_schema_field(DartSchemaBuilder *b, const char *name, DartSchemaTypeKind kind);
/* Fixed array of exactly `count` scalar elements. */
void        dart_schema_field_array(DartSchemaBuilder *b, const char *name,
                                    DartSchemaTypeKind elem_scalar, uint16_t count);
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
DartString  dart_schema_name(const DartSchema *s);        /* root type name */
uint32_t    dart_schema_size(const DartSchema *s);        /* exact message size in bytes */
/* Fields in the flattened depth-first table (EVERY depth; a field's index is its order
 * of appearance in the schema text, nested members included). */
uint16_t    dart_schema_field_count(const DartSchema *s);
int         dart_schema_field_at(const DartSchema *s, uint16_t i, DartSchemaFieldInfo *out); /* 1 + fills out, else 0 */
/* Resolve a field by name; nested members by dotted path ("velocity.dx"). Returns the
 * flat index, or -1. Fields are addressed by NAME everywhere (the getters/setters take
 * the same paths); resolve once and use dart_get/set_value if a hot path measures it. */
int         dart_schema_field_index(const DartSchema *s, const char *path);

int       dart_schema_validate(const DartSchema *s, DartBytes msg); /* 1 if msg.len == schema size */

/* Reader/writer structural compatibility: 1 if a reader declaring `sub` can read
 * messages written with `pub`. Same root name, and every sub field must exist in pub
 * under the same name with the same type (a nested struct field must match exactly).
 * The subset applies at the top level: field order and extra pub fields are free. */
int dart_schema_subset(const DartSchema *sub, const DartSchema *pub);
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
/* ARR: a zero-copy view of the whole array (count * element size bytes); {NULL,0} on
 * mismatch. Element kind/count via dart_schema_field_at. */
DartBytes dart_get_array(DartBytes msg, const DartSchema *s, const char *field);

/* The canonical default message: every field is zero (numeric 0, false, zeroed arrays
 * and structs). Writes exactly dart_schema_size bytes into buf; 1, or 0 if cap is too
 * small. Start a message from this, then set the fields you care about. */
int dart_schema_message_default(const DartSchema *s, void *buf, size_t cap);

/* Setters (the writer mirror of the getters): write one field BY NAME (dotted paths for
 * nested members) of a message being built in buf[0..cap); cap must cover the field, so
 * a buffer of dart_schema_size bytes always works. Values narrow like a C cast. Zero the
 * buffer first (dart_schema_message_default) unless you set every field (dart_set_array
 * zero-fills its own tail), so the bytes are canonical. Returns 1; 0 on a kind mismatch,
 * unknown field, or short buffer. */
int dart_set_uint(void *buf, size_t cap, const DartSchema *s, const char *field, uint64_t v); /* U8..U64, BOOL */
int dart_set_int (void *buf, size_t cap, const DartSchema *s, const char *field, int64_t v);  /* I8..I64        */
int dart_set_f64 (void *buf, size_t cap, const DartSchema *s, const char *field, double v);   /* F64 (or F32)   */
int dart_set_f32 (void *buf, size_t cap, const DartSchema *s, const char *field, float v);    /* F32            */
/* ARR: copy elems over the front of the array and zero the rest. elems.len is bytes, must
 * be a multiple of the element size and fit the field (never silently truncated). */
int dart_set_array(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes elems);

/* Reflection access by flat index (tools walking a schema they've never seen: the
 * explorer, loggers, bridges). One tagged value covers every kind; the typed name-based
 * getters/setters above stay the API for code that knows its fields. */
typedef struct {
    uint8_t  kind;        /* DartSchemaTypeKind */
    uint8_t  elem;        /* ARR element kind */
    uint16_t count;       /* ARR element count */
    union { uint64_t u; int64_t i; double f; } v;   /* scalar value (BOOL in u as 0/1) */
    DartBytes bytes;      /* ARR/STRUCT raw bytes (get: view into msg; set: source, may be
                             shorter than the field: the rest is zeroed) */
} DartValue;
int dart_get_value(DartBytes msg, const DartSchema *s, uint16_t field, DartValue *out);
int dart_set_value(void *buf, size_t cap, const DartSchema *s, uint16_t field, const DartValue *val);

#ifdef __cplusplus
}
#endif
#endif /* DART_SCHEMA_H */
