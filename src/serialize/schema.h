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
#define DART_SCHEMA_WIRE_VERSION 1u    /* bumped on any schema wire-format change */
#endif
#ifndef DART_SCHEMA_MAX_DEPTH
#define DART_SCHEMA_MAX_DEPTH 8u        /* struct nesting the builder accepts */
#endif

/* The type kinds; the enum value is the kind byte on the wire. All fixed: scalars are
 * little-endian with no padding, so a field's offset is the sum of the preceding sizes. */
typedef enum {
    DART_U8 = 0, DART_U16 = 1, DART_U32 = 2, DART_U64 = 3,
    DART_I8 = 4, DART_I16 = 5, DART_I32 = 6, DART_I64 = 7,
    DART_F32 = 8, DART_F64 = 9, DART_BOOL = 10,   /* 0..10: fixed scalars */
    DART_FIX    = 11,   /* fixed-size raw blob (UUID, covariance[72]): [u16 size]            */
    DART_CAPARR = 12,   /* capped array, fixed footprint: [u16 cap][elem]; stores [u16 count] inline */
    DART_STRUCT = 13    /* [u8 nfields] ( [u8 namelen][name][type] )*                        */
} DartSchemaTypeKind;

/* Bytes of a fixed scalar kind (U8..BOOL); 0 otherwise. */
uint32_t dart_schema_scalar_size(DartSchemaTypeKind kind);

/* The compiled schema (hot-path form): wire bytes, identity hash, message size, and a
 * precomputed offset per top-level field. One block allocated via the caller's hook; free
 * it with dart_schema_free. */
typedef struct DartSchema DartSchema;

/* Read-only view of one top-level field, filled by dart_schema_field_at. */
typedef struct {
    DartString name;      /* field name (into the schema's wire bytes) */
    uint8_t    kind;      /* DartSchemaTypeKind */
    uint8_t    elem;      /* CAPARR element kind, else 0 */
    uint16_t   cap;       /* CAPARR capacity, else 0 */
    uint32_t   offset;    /* byte offset of this field in a message */
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
 *   dart_schema_field_fix(&b, "id", 16);
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

DartSchemaBuilder dart_schema_begin(DartAllocFn alloc, void *user, const char *root_name);
/* A fixed scalar field (U8..BOOL). */
void        dart_schema_field(DartSchemaBuilder *b, const char *name, DartSchemaTypeKind kind);
/* Fixed raw blob of exactly `size` bytes. */
void        dart_schema_field_fix(DartSchemaBuilder *b, const char *name, uint16_t size);
/* Capped array of a fixed scalar element: fixed footprint, real length stored inline. */
void        dart_schema_field_caparr(DartSchemaBuilder *b, const char *name,
                                     DartSchemaTypeKind elem_scalar, uint16_t cap);
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
uint16_t    dart_schema_field_count(const DartSchema *s);
int         dart_schema_field_at(const DartSchema *s, uint16_t i, DartSchemaFieldInfo *out); /* 1 + fills out, else 0 */
int         dart_schema_field_index(const DartSchema *s, const char *name);  /* index, or -1 */

int       dart_schema_validate(const DartSchema *s, DartBytes msg); /* 1 if msg.len == schema size */

/* Scalar getters: read the field by index, widened. Pick the family matching its kind; a
 * mismatch or out-of-range field yields 0. Read in place, no copy. */
uint64_t  dart_get_uint(DartBytes msg, const DartSchema *s, uint16_t field);  /* U8..U64, BOOL */
int64_t   dart_get_int (DartBytes msg, const DartSchema *s, uint16_t field);  /* I8..I64        */
double    dart_get_f64 (DartBytes msg, const DartSchema *s, uint16_t field);  /* F64 (or F32)   */
float     dart_get_f32 (DartBytes msg, const DartSchema *s, uint16_t field);  /* F32            */
/* FIX blob: a zero-copy view into the message ({NULL,0} on mismatch). */
DartBytes dart_get_fix (DartBytes msg, const DartSchema *s, uint16_t field);
/* CAPARR: a zero-copy view of the `*count` used elements (clamped to capacity). *count may
 * be NULL. {NULL,0} on mismatch. */
DartBytes dart_get_caparr(DartBytes msg, const DartSchema *s, uint16_t field, uint16_t *count);

#ifdef __cplusplus
}
#endif
#endif /* DART_SCHEMA_H */
