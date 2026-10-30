/* The schema and serialization layer. Both ends share the schema, so the wire carries no
 * tags or names. Depends only on common/. The rules are in spec/schema.md. */
#ifndef RANT_SCHEMA_H
#define RANT_SCHEMA_H

#include <stddef.h>
#include <stdint.h>
#include "../common/api.h"
#include "../common/string.h"
#include "../common/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RANT_SCHEMA_WIRE_VERSION
#define RANT_SCHEMA_WIRE_VERSION 8u      /* bumped on any schema wire change */
#endif
#ifndef RANT_SCHEMA_MAX_DEPTH
#define RANT_SCHEMA_MAX_DEPTH 8u          /* struct nesting the parser accepts */
#endif

/* The kind byte on the wire. Fixed kinds pack first at static offsets, the variable kinds
 * ride tail frames. The encodings are in spec/schema.md. */
typedef enum {
    RANT_U8 = 0, RANT_U16 = 1, RANT_U32 = 2, RANT_U64 = 3,
    RANT_I8 = 4, RANT_I16 = 5, RANT_I32 = 6, RANT_I64 = 7,
    RANT_F32 = 8, RANT_F64 = 9, RANT_BOOL = 10,         /* 0 to 10: fixed scalars */
    RANT_ARR      = 11,   /* fixed count array, a tail frame when its element is variable */
    RANT_STRUCT = 12,
    RANT_STR      = 13,   /* capped string, a slot of [u16 len][cap bytes] */
    RANT_VSTR     = 14,   /* variable string, a tail frame */
    RANT_VARR     = 15,   /* variable array, a tail frame */
    RANT_MAP      = 16,   /* self describing map, a tail frame */
    RANT_ENUM     = 17,   /* a named integer, on the wire just its backing scalar */
    RANT_NAMED    = 18    /* a name tag on another type, zero message bytes, never a root */
} RantSchemaTypeKind;

/* Bytes of a fixed scalar kind, 0 otherwise. */
RANT_API uint32_t rant_schema_scalar_size(RantSchemaTypeKind kind);

/* The compiled schema: wire bytes, hash, size and a flat field table in one block. A node
 * makes them from text or wire, owns them and frees them at close: rant_node_schema. */
typedef struct RantSchema RantSchema;

/* One row of the flat depth first table, one row per type. An array's next row is its
 * element, named "", and a struct's members follow it. See spec/schema.md. */
typedef struct {
    RantString name;        /* the field's own name, a view into the wire, "" for an element */
    RantString type_name;   /* the type's name, {NULL,0} when anonymous */
    uint8_t      kind;      /* RantSchemaTypeKind, a NAMED wrapper is unwrapped into type_name */
    uint8_t      backing;   /* ENUM: the backing kind, else 0 */
    uint16_t     count;     /* ARR element count, or ENUM variant count, else 0 */
    uint16_t     depth;     /* 0 = top level, n = n types down */
    uint16_t     str_cap;   /* STR capacity, else 0 */
    uint16_t     arr_parent;/* flat index of the nearest enclosing array, or 0xFFFF */
    uint16_t     arr_depth; /* the arrays around the row, the indices an access takes */
    uint32_t     offset;    /* from the start of the nearest enclosing element or the message */
    uint32_t     size;      /* fixed bytes, a struct counts its fixed members, 0 if variable */
    uint32_t     min_len;   /* a default value's bytes, what one more element at least costs */
} RantSchemaFieldInfo;

RANT_API RantBytes       rant_schema_wire(const RantSchema *s);            /* the canonical bytes to advertise */
RANT_API uint64_t        rant_schema_hash(const RantSchema *s);            /* FNV-1a over the wire */
RANT_API RantString      rant_schema_name(const RantSchema *s);       /* the root name, "" if anonymous */
/* Spells the schema back as DSL text, the inverse of compile. Returns the full length
 * excluding the NUL, so a NULL buffer measures. */
RANT_API uint32_t      rant_schema_print(const RantSchema *s, char *buf, size_t cap);
/* The fixed section size, where the variable tail starts. */
RANT_API uint32_t      rant_schema_size(const RantSchema *s);
/* The length of the default message, rant_schema_message_default. */
RANT_API uint32_t      rant_schema_msg_min(const RantSchema *s);
/* The live length to send. 0 on a malformed or uninitialized buffer, and also the true
 * length of an empty message, so test 0 only when rant_schema_msg_min is above 0. */
RANT_API uint32_t      rant_schema_msg_len(const RantSchema *s, const void *buf, size_t cap);
/* The flat depth first table. A bare root is one field named "". */
RANT_API uint16_t      rant_schema_field_count(const RantSchema *s);
/* 1 and fills out, else 0 */
RANT_API int           rant_schema_field_at(const RantSchema *s, uint16_t i, RantSchemaFieldInfo *out);
/* Resolves a dotted or indexed path ("velocity.dx", "corners[2].x", "grid[1]") to the flat
 * index, or -1. Each name may be in any spelling, it is matched by its wire spelling. */
RANT_API int           rant_schema_field_index(const RantSchema *s, const char *path);
/* The one wire spelling of a name: fields snake_case (frame_id, FrameId and frameId are all
 * frame_id), types and enum options PascalCase (robot_pose is RobotPose, IDLE is Idle). An
 * underscore, a case change and the last capital of a run split words, so HTTPServer is
 * http_server, and a digit stays with its word, so Imu9Dof is imu9_dof. The parser applies
 * this to every name in a text, and every binding derives a member's wire name with it.
 * The length written, 0 when nothing is left, it starts with a digit or out is too small. */
RANT_API size_t        rant_field_name(const char *name, char *out, size_t cap);
RANT_API size_t        rant_type_name(const char *name, char *out, size_t cap);
/* One field's type encoding, a view into the wire. Two fields have the same type exactly
 * when these agree. {NULL,0} for an unknown index. */
RANT_API RantBytes       rant_schema_field_type_wire(const RantSchema *s, uint16_t field);

/* The options of an enum field by flat index. count is 0 for a non enum field. */
RANT_API uint16_t      rant_schema_enum_count    (const RantSchema *s, uint16_t field);
RANT_API int           rant_schema_enum_variant(const RantSchema *s, uint16_t field, uint16_t i,
                                              int64_t *value, RantString *name);
/* Resolution over the schema alone. name_of gives {NULL,0} for an unknown number, which
 * is safe, not an error. value_of returns 1 and *out for a known name, given in any spelling. */
RANT_API RantString      rant_enum_name_of (const RantSchema *s, uint16_t field, int64_t value);
RANT_API int             rant_enum_value_of(const RantSchema *s, uint16_t field, const char *name, int64_t *out);

/* A schema text read for a tool such as a code generator: every type it defines, in order,
 * with its comments, and no node needed. spec/schema.md, Docs, says where a comment goes. */
typedef struct RantSchemaDoc RantSchemaDoc;

/* Why a text was refused and where, line and column from 1. 0 when there is no place, as
 * when memory ran out. */
typedef struct { const char *why; uint32_t line, column; } RantSchemaError;

/* One definition. Name { } is a struct, Name = enum<u8> { } an enum, any other Name = type
 * an alias. */
typedef struct {
    const RantSchema *schema;   /* the type by its name, for the whole reflection API */
    uint8_t           kind;     /* RANT_STRUCT, RANT_ENUM, or the kind an alias names */
    RantString        about;    /* the comment lines right above it, joined by newlines */
    uint32_t          line;     /* where its name is */
} RantSchemaDef;

/* A member's comment split by the convention [unit] text: "[m] any point" is unit "m". */
typedef struct { RantString unit, text; } RantSchemaNote;

/* Reads text into a doc on alloc, which it copies and resets at free, so use one per doc.
 * NULL with err filled in when the text is refused. */
RANT_API RantSchemaDoc *rant_schema_doc_parse(RantAllocator *alloc, const char *text,
                                              RantSchemaError *err);
RANT_API uint32_t       rant_schema_doc_count(const RantSchemaDoc *d);
/* 1 and fills out, else 0 */
RANT_API int            rant_schema_doc_at(const RantSchemaDoc *d, uint32_t i, RantSchemaDef *out);
/* member counts as written: a struct's field, an enum's option, or 0 for an alias. Empty
 * when it has no comment or does not exist. */
RANT_API RantSchemaNote rant_schema_doc_note(const RantSchemaDoc *d, uint32_t def, uint16_t member);
RANT_API void           rant_schema_doc_free(RantSchemaDoc *d);

/* 1 if msg is exactly one message of this schema. */
RANT_API int         rant_schema_validate(const RantSchema *s, RantBytes msg);

/* 1 if a reader declaring sub can read messages written with pub. The rules are in
 * spec/schema.md. */
RANT_API int rant_schema_subset(const RantSchema *sub, const RantSchema *pub);
/* The same, with the first incompatibility written to buf as one line on refusal. */
RANT_API int rant_schema_subset_why(const RantSchema *sub, const RantSchema *pub,
                                    char *buf, size_t cap);

/* Getters by name, nested members by dotted path and array elements by indexed path
 * ("names[2]", "segments[1].pts[0].x"), widened. A mismatch or unknown field yields 0. */
RANT_API uint64_t    rant_get_uint(RantBytes msg, const RantSchema *s, const char *field);        /* and BOOL */
RANT_API int64_t     rant_get_int (RantBytes msg, const RantSchema *s, const char *field);        /* I8 to I64 */
RANT_API double      rant_get_f64 (RantBytes msg, const RantSchema *s, const char *field);        /* F64 or F32 */
RANT_API float       rant_get_f32 (RantBytes msg, const RantSchema *s, const char *field);        /* F32 */
/* The packed live elements of an array whose element is fixed, {NULL,0} otherwise. */
RANT_API RantBytes rant_get_array(RantBytes msg, const RantSchema *s, const char *field);
/* STR or VSTR: the live bytes. A hostile length prefix is clamped to the cap. */
RANT_API RantString rant_get_string(RantBytes msg, const RantSchema *s, const char *field);
/* The map body, to feed to rant_map_get. An empty body is an empty map. */
RANT_API RantBytes rant_get_map(RantBytes msg, const RantSchema *s, const char *field);
/* The current option name, {NULL,0} if the stored number has no option. */
RANT_API RantString rant_get_enum(RantBytes msg, const RantSchema *s, const char *field);
/* The live element count of an array, 0 if the field is not an array. */
RANT_API uint32_t    rant_get_array_count(RantBytes msg, const RantSchema *s, const char *field);

/* The canonical default message: zeros, empty variable values and fixed count arrays at
 * their count. Always start from it, since the variable setters need every frame. */
RANT_API int rant_schema_message_default(const RantSchema *s, void *buf, size_t cap);

/* Setters by name, nested values by indexed path too. Values narrow like a C cast. A
 * variable setter resizes in place and refuses past cap. 1 ok, 0 refused and untouched. */
/* set_uint also takes BOOL */
RANT_API int rant_set_uint(void *buf, size_t cap, const RantSchema *s, const char *field, uint64_t v);
RANT_API int rant_set_int (void *buf, size_t cap, const RantSchema *s, const char *field, int64_t v);
/* set_f64 also takes an F32 field */
RANT_API int rant_set_f64 (void *buf, size_t cap, const RantSchema *s, const char *field, double v);
RANT_API int rant_set_f32 (void *buf, size_t cap, const RantSchema *s, const char *field, float v);
/* An array of fixed elements. ARR: elems over the front, the rest zeroed. VARR: becomes
 * exactly elems. Whole elements only, never silently truncated. */
RANT_API int rant_set_array(void *buf, size_t cap, const RantSchema *s, const char *field, RantBytes elems);
/* Grows or shrinks a variable array. New elements take their default. */
RANT_API int rant_set_array_count(void *buf, size_t cap, const RantSchema *s, const char *field,
                                  uint32_t count);
/* STR: the live bytes, the slot tail zeroed, v.len must fit the cap. VSTR: becomes v. */
RANT_API int rant_set_string(void *buf, size_t cap, const RantSchema *s, const char *field, RantString v);
/* The map becomes this body, validated first. */
RANT_API int rant_set_map(void *buf, size_t cap, const RantSchema *s, const char *field, RantBytes map);
/* Writes the option by name, 0 for an unknown one. */
RANT_API int rant_set_enum(void *buf, size_t cap, const RantSchema *s, const char *field, const char *name);

/* One tagged value for reflection by flat index. */
typedef struct {
    uint8_t  kind;        /* RantSchemaTypeKind */
    uint32_t count;       /* read: ARR and VARR live elements, MAP entries, else 0 */
    union { uint64_t u; int64_t i; double f; } v;   /* the scalar value, BOOL in u, ENUM in i */
    RantBytes bytes;      /* the live string, the map body, a struct's fixed bytes, or an
                             array's packed elements when its element is fixed */
} RantValue;
RANT_API int rant_get_value(RantBytes msg, const RantSchema *s, uint16_t field, RantValue *out);
RANT_API int rant_set_value(void *buf, size_t cap, const RantSchema *s, uint16_t field, const RantValue *val);
/* The same under arrays: elems holds one index per enclosing array, outermost first, and
 * n_elems must equal the field's arr_depth. An array of fixed elements is set from its packed
 * elements, one of variable elements element by element once rant_set_array_count_at sized it. */
RANT_API int rant_get_value_at(RantBytes msg, const RantSchema *s, uint16_t field,
                               const uint32_t *elems, uint16_t n_elems, RantValue *out);
RANT_API int rant_set_value_at(void *buf, size_t cap, const RantSchema *s, uint16_t field,
                               const uint32_t *elems, uint16_t n_elems, const RantValue *val);
/* rant_set_array_count by flat index, elems as rant_set_value_at takes them. */
RANT_API int rant_set_array_count_at(void *buf, size_t cap, const RantSchema *s, uint16_t field,
                                     const uint32_t *elems, uint16_t n_elems, uint32_t count);

/* The map is a self describing tagged value tree, the escape from the schema. Its body
 * grammar is in spec/schema.md. The writer fills a caller buffer with no allocation. */
typedef struct {
    uint8_t *buf; size_t cap, len;               /* the caller's buffer and write position */
    int      err;                                /* 0 ok, nonzero latches failure */
    uint16_t depth;                              /* open bodies, 1 = the root map only */
    size_t   count_pos[RANT_SCHEMA_MAX_DEPTH]; /* offset of each open body's count */
    uint16_t count[RANT_SCHEMA_MAX_DEPTH];
    uint8_t  is_arr[RANT_SCHEMA_MAX_DEPTH];
} RantMapWriter;

RANT_API RantMapWriter rant_map_begin(void *buf, size_t cap);         /* opens the root map */
RANT_API int rant_map_put_uint      (RantMapWriter *w, const char *key, uint64_t v);
RANT_API int rant_map_put_int       (RantMapWriter *w, const char *key, int64_t v);
RANT_API int rant_map_put_f64       (RantMapWriter *w, const char *key, double v);
RANT_API int rant_map_put_f32       (RantMapWriter *w, const char *key, float v);
RANT_API int rant_map_put_bool      (RantMapWriter *w, const char *key, int v);
RANT_API int rant_map_put_string(RantMapWriter *w, const char *key, RantString v);
RANT_API int rant_map_open_map      (RantMapWriter *w, const char *key);     /* a nested map, close it */
RANT_API int rant_map_open_array(RantMapWriter *w, const char *key);         /* elements pass key NULL */
RANT_API int rant_map_close         (RantMapWriter *w);                      /* closes the innermost open body */
RANT_API uint32_t rant_map_finish(RantMapWriter *w);                         /* the body length, 0 on any error */

/* Readers over a possibly hostile body: every walk is bounds checked and a nested value
 * comes back in RantValue.bytes ready for these same functions. */
RANT_API uint16_t rant_map_count(RantBytes map);                             /* 0 if empty or short */
RANT_API int rant_map_at (RantBytes map, uint16_t index, RantString *key, RantValue *out);
RANT_API int rant_map_get(RantBytes map, const char *key, RantValue *out);           /* 1 and fills out, else 0 */
RANT_API uint16_t rant_map_array_count(RantBytes arr);
RANT_API int rant_map_array_at(RantBytes arr, uint16_t index, RantValue *out);
RANT_API int rant_map_valid(RantBytes map);         /* the full structural check, rant_set_map runs it */

#ifdef __cplusplus
}
#endif
#endif /* RANT_SCHEMA_H */
