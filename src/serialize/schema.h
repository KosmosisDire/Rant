/* The schema and serialization layer. Both ends share the schema, so the wire carries no
 * tags or names. Depends only on common/. The rules are in spec/schema.md. */
#ifndef RAMBLE_SCHEMA_H
#define RAMBLE_SCHEMA_H

#include <stddef.h>
#include <stdint.h>
#include "../common/api.h"
#include "../common/string.h"
#include "../common/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RAMBLE_SCHEMA_WIRE_VERSION
#define RAMBLE_SCHEMA_WIRE_VERSION 8u    /* bumped on any schema wire change */
#endif
#ifndef RAMBLE_SCHEMA_MAX_DEPTH
#define RAMBLE_SCHEMA_MAX_DEPTH 8u        /* struct nesting the builder accepts */
#endif

/* The kind byte on the wire. Fixed kinds pack first at static offsets, the variable kinds
 * ride tail frames. The encodings are in spec/schema.md. */
typedef enum {
    RAMBLE_U8 = 0, RAMBLE_U16 = 1, RAMBLE_U32 = 2, RAMBLE_U64 = 3,
    RAMBLE_I8 = 4, RAMBLE_I16 = 5, RAMBLE_I32 = 6, RAMBLE_I64 = 7,
    RAMBLE_F32 = 8, RAMBLE_F64 = 9, RAMBLE_BOOL = 10,   /* 0 to 10: fixed scalars */
    RAMBLE_ARR    = 11,   /* fixed array, the element must be fixed */
    RAMBLE_STRUCT = 12,
    RAMBLE_STR    = 13,   /* capped string, a slot of [u16 len][cap bytes] */
    RAMBLE_VSTR   = 14,   /* variable string, a tail frame */
    RAMBLE_VARR   = 15,   /* variable array, a tail frame of packed elements */
    RAMBLE_MAP    = 16,   /* self describing map, a tail frame */
    RAMBLE_ENUM   = 17,   /* a named integer, on the wire just its backing scalar */
    RAMBLE_NAMED  = 18    /* a name tag on another type, zero message bytes, never a root */
} RambleSchemaTypeKind;

/* Bytes of a fixed scalar kind, 0 otherwise. */
RAMBLE_API uint32_t ramble_schema_scalar_size(RambleSchemaTypeKind kind);

/* The compiled schema: wire bytes, hash, size and a flat field table in one block from
 * the hook. Free it with ramble_schema_free. */
typedef struct RambleSchema RambleSchema;

/* One field of the flat depth first table. A struct array flattens its element once as
 * an element 0 template under the array field. See spec/schema.md. */
typedef struct {
    RambleString name;      /* the field's own name, a view into the wire */
    RambleString type_name; /* the field type's name, {NULL,0} when anonymous */
    RambleString elem_name; /* an array element type's name, {NULL,0} when anonymous */
    uint8_t      kind;      /* RambleSchemaTypeKind, a NAMED wrapper is unwrapped into type_name */
    uint8_t      elem;      /* the array element kind, or the enum backing kind, else 0 */
    uint16_t     count;     /* ARR element count, or ENUM variant count, else 0 */
    uint16_t     depth;     /* 0 = top level, n = a member of the struct n levels up */
    uint16_t     str_cap;   /* string capacity for STR fields and STR elements, else 0 */
    uint16_t     arr_parent;/* flat index of the enclosing struct array, or 0xFFFF */
    uint32_t     offset;    /* absolute byte offset, 0 for variable kinds */
    uint32_t     size;      /* byte size, 0 for variable kinds */
    uint32_t     elem_size; /* ARR and VARR: bytes of one element, else 0 */
} RambleSchemaFieldInfo;

/* One enum option. */
typedef struct {
    int64_t     value;
    const char *name;     /* NUL terminated builder input, at most 255 bytes */
} RambleEnumVariant;

/* The builder grows its wire buffer through the hook as fields are added. finish returns
 * the compiled schema, or NULL after any latched error. Always finish a begun builder. */
typedef struct {
    RambleAllocFn alloc; void *user;
    uint8_t *buf;
    size_t   cap;
    size_t   len;                                /* wire bytes written so far */
    int      err;                                /* 0 ok, nonzero latches failure */
    uint8_t  value_root;                         /* a bare root: 1 awaits its type, 2 written */
    uint8_t  raw_type;                           /* internal: type bytes only, no header */
    uint16_t base_depth;                         /* the struct depth the root sits at */
    uint16_t arr_depth;                          /* the open array element depth, 0xFFFF = none */
    uint16_t depth;                              /* open structs, 1 = a struct root only */
    size_t   count_pos[RAMBLE_SCHEMA_MAX_DEPTH]; /* wire offset of each open struct's nfields byte */
    uint16_t field_count[RAMBLE_SCHEMA_MAX_DEPTH];
} RambleSchemaBuilder;

/* Compiles DSL text, pasted verbatim on the writer and every reader. The DSL is in
 * docs/stdtypes.md and spec/schema.md. NULL on error, with *err at the offending character. */
RAMBLE_API RambleSchema *ramble_schema_compile(RambleAllocFn alloc, void *user, const char *text, const char **err);
/* compile with an environment: each env schema is referenceable by its root name. An
 * anonymous env root is ignored. */
RAMBLE_API RambleSchema *ramble_schema_compile_env(RambleAllocFn alloc, void *user, const char *text,
                                             const RambleSchema *const *env, size_t n_env,
                                             const char **err);

RAMBLE_API RambleSchemaBuilder ramble_schema_begin(RambleAllocFn alloc, void *user, const char *root_name);
/* A bare type root: add exactly one field with an empty name, then finish. */
RAMBLE_API RambleSchemaBuilder ramble_schema_begin_value(RambleAllocFn alloc, void *user);
/* A named alias root, like Uuid = u8[16]: one empty named field, then finish. */
RAMBLE_API RambleSchemaBuilder ramble_schema_begin_alias(RambleAllocFn alloc, void *user, const char *name);
/* A fixed scalar field. */
RAMBLE_API void        ramble_schema_field(RambleSchemaBuilder *b, const char *name, RambleSchemaTypeKind kind);
/* A fixed array of count scalars. */
RAMBLE_API void        ramble_schema_field_array(RambleSchemaBuilder *b, const char *name,
                                             RambleSchemaTypeKind elem_scalar, uint16_t count);
/* A capped string, cap at least 1. */
RAMBLE_API void        ramble_schema_field_string(RambleSchemaBuilder *b, const char *name, uint16_t cap);
/* A fixed array of count capped strings. */
RAMBLE_API void        ramble_schema_field_string_array(RambleSchemaBuilder *b, const char *name,
                                                    uint16_t cap, uint16_t count);
/* A variable string. Any depth outside an array element. */
RAMBLE_API void        ramble_schema_field_var_string(RambleSchemaBuilder *b, const char *name);
/* A variable array of scalars. */
RAMBLE_API void        ramble_schema_field_var_array(RambleSchemaBuilder *b, const char *name,
                                                 RambleSchemaTypeKind elem_scalar);
/* A variable array of capped strings. */
RAMBLE_API void        ramble_schema_field_var_string_array(RambleSchemaBuilder *b, const char *name,
                                                        uint16_t cap);
/* A self describing map. Write it with RambleMapWriter, read it with ramble_map_get. */
RAMBLE_API void        ramble_schema_field_map(RambleSchemaBuilder *b, const char *name);
/* A named integer with an integer backing and n options, n at most 65535. Latches an
 * error on a non integer backing or a value that does not fit it. */
RAMBLE_API void        ramble_schema_field_enum(RambleSchemaBuilder *b, const char *name,
                                            RambleSchemaTypeKind backing,
                                            const RambleEnumVariant *variants, uint16_t n);
/* A field of a compiled named type. type need not outlive the call. */
RAMBLE_API void        ramble_schema_field_named(RambleSchemaBuilder *b, const char *name,
                                             const RambleSchema *type);
/* An array of a named type, variable when count is 0. The element must be fixed. */
RAMBLE_API void        ramble_schema_field_named_array(RambleSchemaBuilder *b, const char *name,
                                                   const RambleSchema *type, uint16_t count);
/* Opens a nested struct. Add its fields, then end_struct. */
RAMBLE_API void        ramble_schema_begin_struct(RambleSchemaBuilder *b, const char *name);
/* An array of anonymous structs, variable when count is 0. Add the element's fields,
 * then end_struct. The element must be fixed. */
RAMBLE_API void        ramble_schema_begin_struct_array(RambleSchemaBuilder *b, const char *name,
                                                    uint16_t count);
RAMBLE_API void        ramble_schema_end_struct(RambleSchemaBuilder *b);
/* Closes the root, compiles, and returns the schema. NULL on any latched error. */
RAMBLE_API RambleSchema *ramble_schema_finish(RambleSchemaBuilder *b);

/* Compiles received wire, bounds checked. NULL on an overrun or a version mismatch. The
 * bytes are copied in. */
RAMBLE_API RambleSchema *ramble_schema_parse(const void *wire, size_t wire_len, RambleAllocFn alloc, void *user);
/* The same hook the schema was made with. */
RAMBLE_API void        ramble_schema_free(RambleSchema *s, RambleAllocFn alloc, void *user);
/* An owned copy of any schema, safe to keep and to hand to create. */
RAMBLE_API RambleSchema *ramble_schema_copy(const RambleSchema *s, RambleAllocFn alloc, void *user);

RAMBLE_API RambleBytes   ramble_schema_wire(const RambleSchema *s);        /* the canonical bytes to advertise */
RAMBLE_API uint64_t      ramble_schema_hash(const RambleSchema *s);        /* FNV-1a over the wire */
RAMBLE_API RambleString  ramble_schema_name(const RambleSchema *s);   /* the root name, "" if anonymous */
/* Spells the schema back as DSL text, the inverse of compile. Returns the full length
 * excluding the NUL, so a NULL buffer measures. */
RAMBLE_API uint32_t    ramble_schema_print(const RambleSchema *s, char *buf, size_t cap);
/* The fixed section size, where the variable tail starts. */
RAMBLE_API uint32_t    ramble_schema_size(const RambleSchema *s);
/* The smallest valid message: the fixed section plus one empty frame per variable field. */
RAMBLE_API uint32_t    ramble_schema_msg_min(const RambleSchema *s);
/* The live length to send. 0 on a malformed or uninitialized buffer. */
RAMBLE_API uint32_t    ramble_schema_msg_len(const RambleSchema *s, const void *buf, size_t cap);
/* The flat depth first table. A bare root is one field named "". */
RAMBLE_API uint16_t    ramble_schema_field_count(const RambleSchema *s);
/* 1 and fills out, else 0 */
RAMBLE_API int         ramble_schema_field_at(const RambleSchema *s, uint16_t i, RambleSchemaFieldInfo *out);
/* Resolves a dotted or indexed path ("velocity.dx", "corners[2].x") to the flat index, or -1. */
RAMBLE_API int         ramble_schema_field_index(const RambleSchema *s, const char *path);
/* One field's type encoding, a view into the wire. Two fields have the same type exactly
 * when these agree. {NULL,0} for an unknown index. */
RAMBLE_API RambleBytes   ramble_schema_field_type_wire(const RambleSchema *s, uint16_t field);

/* The options of an enum field by flat index. count is 0 for a non enum field. */
RAMBLE_API uint16_t    ramble_schema_enum_count  (const RambleSchema *s, uint16_t field);
RAMBLE_API int         ramble_schema_enum_variant(const RambleSchema *s, uint16_t field, uint16_t i,
                                              int64_t *value, RambleString *name);
/* Resolution over the schema alone. name_of gives {NULL,0} for an unknown number, which
 * is safe, not an error. value_of returns 1 and *out for a known name. */
RAMBLE_API RambleString  ramble_enum_name_of (const RambleSchema *s, uint16_t field, int64_t value);
RAMBLE_API int           ramble_enum_value_of(const RambleSchema *s, uint16_t field, const char *name, int64_t *out);

/* 1 if msg is exactly one message of this schema. */
RAMBLE_API int       ramble_schema_validate(const RambleSchema *s, RambleBytes msg);

/* 1 if a reader declaring sub can read messages written with pub. The rules are in
 * spec/schema.md. */
RAMBLE_API int ramble_schema_subset(const RambleSchema *sub, const RambleSchema *pub);
/* The same, with the first incompatibility written to buf as one line on refusal. */
RAMBLE_API int ramble_schema_subset_why(const RambleSchema *sub, const RambleSchema *pub,
                                    char *buf, size_t cap);
/* The reader's fields with the writer's offsets and size. Requires subset, NULL
 * otherwise or on OOM. Free with ramble_schema_free. */
RAMBLE_API RambleSchema *ramble_schema_rebase(const RambleSchema *sub, const RambleSchema *pub,
                                        RambleAllocFn alloc, void *user);

/* Getters by name, nested members by dotted path and array members by indexed path,
 * widened. A mismatch or unknown field yields 0. */
RAMBLE_API uint64_t  ramble_get_uint(RambleBytes msg, const RambleSchema *s, const char *field);  /* and BOOL */
RAMBLE_API int64_t   ramble_get_int (RambleBytes msg, const RambleSchema *s, const char *field);  /* I8 to I64 */
RAMBLE_API double    ramble_get_f64 (RambleBytes msg, const RambleSchema *s, const char *field);  /* F64 or F32 */
RAMBLE_API float     ramble_get_f32 (RambleBytes msg, const RambleSchema *s, const char *field);  /* F32 */
/* ARR: the whole array. VARR: the live elements, a whole multiple of the element size. */
RAMBLE_API RambleBytes ramble_get_array(RambleBytes msg, const RambleSchema *s, const char *field);
/* STR or VSTR: the live bytes. A hostile length prefix is clamped to the cap. */
RAMBLE_API RambleString ramble_get_string(RambleBytes msg, const RambleSchema *s, const char *field);
/* One element of a string array, {NULL,0} past the live count. */
RAMBLE_API RambleString ramble_get_string_at(RambleBytes msg, const RambleSchema *s, const char *field,
                                       uint16_t index);
/* The map body, to feed to ramble_map_get. An empty body is an empty map. */
RAMBLE_API RambleBytes ramble_get_map(RambleBytes msg, const RambleSchema *s, const char *field);
/* The current option name, {NULL,0} if the stored number has no option. */
RAMBLE_API RambleString ramble_get_enum(RambleBytes msg, const RambleSchema *s, const char *field);
/* The live element count of a struct array, 0 if the field is not an array. */
RAMBLE_API uint32_t  ramble_get_array_count(RambleBytes msg, const RambleSchema *s, const char *field);

/* The canonical default message: zeros and one empty frame per variable field. Always
 * start from this, since the variable setters rely on every frame existing. */
RAMBLE_API int ramble_schema_message_default(const RambleSchema *s, void *buf, size_t cap);

/* Setters by name. Values narrow like a C cast. A variable setter resizes its frame in
 * place and returns 0 when the message would exceed cap. 1 ok, 0 on a mismatch. */
/* set_uint also takes BOOL */
RAMBLE_API int ramble_set_uint(void *buf, size_t cap, const RambleSchema *s, const char *field, uint64_t v);
RAMBLE_API int ramble_set_int (void *buf, size_t cap, const RambleSchema *s, const char *field, int64_t v);
/* set_f64 also takes an F32 field */
RAMBLE_API int ramble_set_f64 (void *buf, size_t cap, const RambleSchema *s, const char *field, double v);
RAMBLE_API int ramble_set_f32 (void *buf, size_t cap, const RambleSchema *s, const char *field, float v);
/* ARR: elems over the front, the rest zeroed. VARR: the frame becomes elems. Whole
 * elements only, never silently truncated. */
RAMBLE_API int ramble_set_array(void *buf, size_t cap, const RambleSchema *s, const char *field, RambleBytes elems);
/* Grows or shrinks a variable array, zero filling new elements. */
RAMBLE_API int ramble_set_array_count(void *buf, size_t cap, const RambleSchema *s, const char *field,
                                  uint32_t count);
/* STR: the live bytes, the slot tail zeroed, v.len must fit the cap. VSTR: the frame becomes v. */
RAMBLE_API int ramble_set_string(void *buf, size_t cap, const RambleSchema *s, const char *field, RambleString v);
/* One element of a string array, under the live count for a variable one. */
RAMBLE_API int ramble_set_string_at(void *buf, size_t cap, const RambleSchema *s, const char *field,
                                uint16_t index, RambleString v);
/* The frame becomes the map body, validated first. */
RAMBLE_API int ramble_set_map(void *buf, size_t cap, const RambleSchema *s, const char *field, RambleBytes map);
/* Writes the option by name, 0 for an unknown one. */
RAMBLE_API int ramble_set_enum(void *buf, size_t cap, const RambleSchema *s, const char *field, const char *name);

/* One tagged value for reflection by flat index. */
typedef struct {
    uint8_t  kind;        /* RambleSchemaTypeKind */
    uint8_t  elem;        /* the array element kind, or the enum backing kind */
    uint16_t count;       /* ARR count, VARR live count, MAP entries (saturated), ENUM variants */
    uint16_t str_cap;     /* string capacity for STR fields and STR elements */
    union { uint64_t u; int64_t i; double f; } v;   /* the scalar value, BOOL in u, ENUM in i */
    RambleBytes bytes;      /* the raw bytes, the live string, the live elements or the map body */
} RambleValue;
RAMBLE_API int ramble_get_value(RambleBytes msg, const RambleSchema *s, uint16_t field, RambleValue *out);
RAMBLE_API int ramble_set_value(void *buf, size_t cap, const RambleSchema *s, uint16_t field, const RambleValue *val);
/* The same under a struct array, elem selects the element. Ignored for other fields. */
RAMBLE_API int ramble_get_value_at(RambleBytes msg, const RambleSchema *s, uint16_t field, uint32_t elem,
                               RambleValue *out);
RAMBLE_API int ramble_set_value_at(void *buf, size_t cap, const RambleSchema *s, uint16_t field, uint32_t elem,
                               const RambleValue *val);
/* The live count of the struct array at flat index field, 0 if it is not an array. */
RAMBLE_API uint32_t ramble_array_count_at(RambleBytes msg, const RambleSchema *s, uint16_t field);

/* The map is a self describing tagged value tree, the escape from the schema. Its body
 * grammar is in spec/schema.md. The writer fills a caller buffer with no allocation. */
typedef struct {
    uint8_t *buf; size_t cap, len;               /* the caller's buffer and write position */
    int      err;                                /* 0 ok, nonzero latches failure */
    uint16_t depth;                              /* open bodies, 1 = the root map only */
    size_t   count_pos[RAMBLE_SCHEMA_MAX_DEPTH]; /* offset of each open body's count */
    uint16_t count[RAMBLE_SCHEMA_MAX_DEPTH];
    uint8_t  is_arr[RAMBLE_SCHEMA_MAX_DEPTH];
} RambleMapWriter;

RAMBLE_API RambleMapWriter ramble_map_begin(void *buf, size_t cap);   /* opens the root map */
RAMBLE_API int ramble_map_put_uint  (RambleMapWriter *w, const char *key, uint64_t v);
RAMBLE_API int ramble_map_put_int   (RambleMapWriter *w, const char *key, int64_t v);
RAMBLE_API int ramble_map_put_f64   (RambleMapWriter *w, const char *key, double v);
RAMBLE_API int ramble_map_put_f32   (RambleMapWriter *w, const char *key, float v);
RAMBLE_API int ramble_map_put_bool  (RambleMapWriter *w, const char *key, int v);
RAMBLE_API int ramble_map_put_string(RambleMapWriter *w, const char *key, RambleString v);
RAMBLE_API int ramble_map_open_map  (RambleMapWriter *w, const char *key);   /* a nested map, close it */
RAMBLE_API int ramble_map_open_array(RambleMapWriter *w, const char *key);   /* elements pass key NULL */
RAMBLE_API int ramble_map_close     (RambleMapWriter *w);                    /* closes the innermost open body */
RAMBLE_API uint32_t ramble_map_finish(RambleMapWriter *w);                   /* the body length, 0 on any error */

/* Readers over a possibly hostile body: every walk is bounds checked and a nested value
 * comes back in RambleValue.bytes ready for these same functions. */
RAMBLE_API uint16_t ramble_map_count(RambleBytes map);                       /* 0 if empty or short */
RAMBLE_API int ramble_map_at (RambleBytes map, uint16_t index, RambleString *key, RambleValue *out);
RAMBLE_API int ramble_map_get(RambleBytes map, const char *key, RambleValue *out);   /* 1 and fills out, else 0 */
RAMBLE_API uint16_t ramble_map_array_count(RambleBytes arr);
RAMBLE_API int ramble_map_array_at(RambleBytes arr, uint16_t index, RambleValue *out);
RAMBLE_API int ramble_map_valid(RambleBytes map);   /* the full structural check, ramble_set_map runs it */

#ifdef __cplusplus
}
#endif
#endif /* RAMBLE_SCHEMA_H */
