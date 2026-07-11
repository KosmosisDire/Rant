#include "schema.h"
#include "../common/bytes.h"   /* dart_le_* */
#include "../common/hash.h"    /* i_dart_fnv1a64 (the schema id) */
#include <string.h>            /* memcpy (float bit reinterpret) */

/* The schema wire format (what the builder emits and dart_schema_parse reads):
 *   schema := [u8 version][u8 root_namelen][root_name...][type]   ; root type is a STRUCT
 *   type   := [u8 kind] payload
 *     scalar (U8..BOOL)      : (none; size implied by kind)
 *     ARR                    : [u8 elem][u16 count]               ; elem: a scalar, or
 *                              [u8 STR][u16 count][u16 cap]       ;       a capped string
 *     STRUCT                 : [u8 nfields] ( [u8 namelen][name][type] )*nfields
 *     STR                    : [u16 cap]           ; in a message: [u16 len][cap bytes]
 * Every type is fixed: a field's byte offset is the sum of the preceding field sizes. */

/* The per-field record, one for EVERY field at every depth (flattened depth-first: a
 * struct's members directly follow it). Offsets are message-absolute. */
typedef struct {
    DartString name;      /* field's own name, a view into the wire bytes */
    uint32_t   offset;    /* absolute byte offset in a message */
    uint32_t   size;      /* byte size */
    uint32_t   type_off;  /* wire offset of the field's type encoding (subset compare) */
    uint32_t   type_len;  /* wire length of the type encoding */
    uint16_t   count;     /* ARR element count, else 0 */
    uint16_t   depth;     /* 0 = top level */
    uint16_t   parent;    /* flat index of the enclosing struct field; 0xFFFF = root */
    uint16_t   str_cap;   /* string capacity (STR fields and STR-element arrays), else 0 */
    uint8_t    kind;
    uint8_t    elem;      /* ARR element kind, else 0 */
} i_Field;

struct DartSchema {
    DartBytes   wire;       /* the canonical bytes (a view into the caller's buffer) */
    uint64_t    hash;
    uint32_t    size;       /* exact message size in bytes */
    DartString  name;       /* root type name, a view into the wire bytes */
    uint16_t    nfields;
    i_Field     fields[1];   /* nfields entries, laid out in the caller's buffer */
};

uint32_t dart_schema_scalar_size(DartSchemaTypeKind kind){
    switch (kind){
        case DART_U8: case DART_I8: case DART_BOOL: return 1;
        case DART_U16: case DART_I16:               return 2;
        case DART_U32: case DART_I32: case DART_F32: return 4;
        case DART_U64: case DART_I64: case DART_F64: return 8;
        default: return 0;
    }
}

/* ---- bounds-checked reader over (possibly hostile) wire bytes ---------------------- */
typedef struct { const uint8_t *w; size_t n, pos; int fail; } i_Rd;
static uint8_t  i_dart_rd_u8 (i_Rd *r){ if (r->pos + 1 > r->n){ r->fail = 1; return 0; } return r->w[r->pos++]; }
static uint16_t i_dart_rd_u16(i_Rd *r){ uint16_t v; if (r->pos + 2 > r->n){ r->fail = 1; return 0; } v = i_dart_le_r16(r->w + r->pos); r->pos += 2; return v; }
static void     i_dart_rd_skip(i_Rd *r, size_t k){ if (r->pos + k > r->n){ r->fail = 1; r->pos = r->n; return; } r->pos += k; }

/* Byte size of the type at r->pos; advances r past it. Counts every field it walks
 * (all depths) into *fields when given. Fails on an unknown/variable kind or nesting
 * past DART_SCHEMA_MAX_DEPTH (the read-side twin of the builder's cap: hostile wire
 * must not recurse unboundedly). */
static uint32_t i_dart_rd_type_size(i_Rd *r, uint32_t *fields, uint16_t depth){
    uint8_t k = i_dart_rd_u8(r);
    if (r->fail) return 0;
    switch (k){
        case DART_U8: case DART_I8: case DART_BOOL: return 1;
        case DART_U16: case DART_I16:               return 2;
        case DART_U32: case DART_I32: case DART_F32: return 4;
        case DART_U64: case DART_I64: case DART_F64: return 8;
        case DART_STR:
            return 2u + (uint32_t)i_dart_rd_u16(r);              /* [u16 len][cap bytes] */
        case DART_ARR: {
            uint8_t  elem  = i_dart_rd_u8(r);
            uint16_t count = i_dart_rd_u16(r);
            uint64_t es    = dart_schema_scalar_size((DartSchemaTypeKind)elem);
            if (elem == DART_STR) es = 2u + (uint64_t)i_dart_rd_u16(r);   /* whole string slots */
            if (r->fail || es == 0){ r->fail = 1; return 0; }    /* elem: a scalar or a string */
            if ((uint64_t)count * es > 0xFFFFFFFFu){ r->fail = 1; return 0; }
            return (uint32_t)((uint64_t)count * es);
        }
        case DART_STRUCT: {
            uint8_t nf; uint64_t sum = 0; uint16_t i;
            if (depth >= DART_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
            nf = i_dart_rd_u8(r);
            for (i = 0; i < nf && !r->fail; i++){
                uint8_t fl = i_dart_rd_u8(r);
                i_dart_rd_skip(r, fl);
                if (fields) (*fields)++;
                sum += i_dart_rd_type_size(r, fields, (uint16_t)(depth + 1));
            }
            if (sum > 0xFFFFFFFFu){ r->fail = 1; return 0; }
            return (uint32_t)sum;
        }
        default: r->fail = 1; return 0;                          /* unknown/variable kind: reject */
    }
}

/* Offset of the compiled handle in buf: past the wire bytes, 8-aligned for the handle. */
static size_t i_dart_schema_handle_off(const uint8_t *buf, size_t wire_len){
    uintptr_t addr = (uintptr_t)buf + wire_len;
    size_t pad = (size_t)((8u - (addr & 7u)) & 7u);
    return wire_len + pad;
}

/* Count every field (all depths) of the schema wire's root struct into *n; 1, or 0 on
 * malformed wire (an empty-but-valid schema is 1 with *n == 0). */
static int i_dart_schema_wire_fields(const void *wire, size_t wire_len, uint32_t *n){
    i_Rd r; uint8_t ver, rl;
    *n = 0;
    r.w = (const uint8_t *)wire; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_dart_rd_u8(&r); if (r.fail || ver != DART_SCHEMA_WIRE_VERSION) return 0;
    rl = i_dart_rd_u8(&r); i_dart_rd_skip(&r, rl);
    if (r.fail || (size_t)r.pos >= r.n || r.w[r.pos] != DART_STRUCT) return 0;   /* root: a struct */
    i_dart_rd_type_size(&r, n, 0);
    return r.fail ? 0 : 1;
}

/* Emit the fields of the struct body at r->pos (its nfields byte) into the flat table,
 * depth-first. Returns the struct's byte size; sets r->fail on malformed wire. */
static uint32_t i_dart_schema_emit(uint8_t *buf, size_t wire_len, i_Rd *r, DartSchema *s,
                                   uint16_t *emitted, uint32_t total,
                                   uint16_t depth, uint16_t parent, uint32_t base){
    uint8_t nf = i_dart_rd_u8(r); uint16_t i;
    uint32_t running = base;
    if (depth >= DART_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
    for (i = 0; i < nf && !r->fail; i++){
        i_Field *f;
        uint8_t fl = i_dart_rd_u8(r); const char *fn = (const char *)(buf + r->pos);
        size_t kpos; uint16_t idx; uint32_t sz;
        i_dart_rd_skip(r, fl);
        kpos = r->pos;
        if (*emitted >= total){ r->fail = 1; return 0; }
        idx = (*emitted)++;
        f = &s->fields[idx];
        f->name = dart_string(fn, fl);
        f->kind = (kpos < r->n) ? buf[kpos] : 0;
        f->count = 0; f->elem = 0; f->str_cap = 0;
        f->depth = depth; f->parent = parent;
        f->offset = running;
        if (f->kind == DART_STRUCT){
            i_dart_rd_u8(r);                                   /* consume the kind byte */
            sz = i_dart_schema_emit(buf, wire_len, r, s, emitted, total,
                                    (uint16_t)(depth + 1), idx, running);
        } else {
            sz = i_dart_rd_type_size(r, NULL, depth);
            if (f->kind == DART_ARR || f->kind == DART_STR){   /* capture elem/count/cap for readers */
                i_Rd q; q.w = buf; q.n = wire_len; q.pos = kpos + 1; q.fail = 0;
                if (f->kind == DART_ARR){
                    f->elem = i_dart_rd_u8(&q);
                    f->count = i_dart_rd_u16(&q);
                }
                if (f->kind == DART_STR || f->elem == DART_STR)
                    f->str_cap = i_dart_rd_u16(&q);
            }
        }
        if (r->fail) return 0;
        f->type_off = (uint32_t)kpos;                          /* type extents: subset compare */
        f->type_len = (uint32_t)(r->pos - kpos);
        f->size = sz;
        running += sz;
    }
    return running - base;
}

/* Compile wire bytes already sitting at buf[0..wire_len] into a DartSchema placed after
 * them in buf. Returns NULL on a malformed blob or if buf[cap] is too small. */
static DartSchema *i_dart_schema_compile(uint8_t *buf, size_t wire_len, size_t cap){
    i_Rd r; uint8_t ver, root_kind, root_namelen;
    const char *root_name; size_t hoff, need; DartSchema *s;
    uint32_t total; uint16_t emitted = 0;

    if (!i_dart_schema_wire_fields(buf, wire_len, &total) || total > 0xFFFFu) return NULL;

    r.w = buf; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_dart_rd_u8(&r);
    if (r.fail || ver != DART_SCHEMA_WIRE_VERSION) return NULL;
    root_namelen = i_dart_rd_u8(&r);
    root_name = (const char *)(buf + r.pos);
    i_dart_rd_skip(&r, root_namelen);
    root_kind = i_dart_rd_u8(&r);
    if (r.fail || root_kind != DART_STRUCT) return NULL;       /* the root must be a struct */

    hoff = i_dart_schema_handle_off(buf, wire_len);
    need = hoff + sizeof(DartSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
    if (need > cap) return NULL;

    s = (DartSchema *)(buf + hoff);
    s->wire = dart_bytes(buf, wire_len);
    s->name = dart_string(root_name, root_namelen);
    s->nfields = (uint16_t)total;
    s->hash = i_dart_fnv1a64(buf, wire_len);
    s->size = i_dart_schema_emit(buf, wire_len, &r, s, &emitted, (uint32_t)total, 0, 0xFFFFu, 0);
    if (r.fail || emitted != (uint16_t)total) return NULL;
    return s;
}

/* ---- builder ----------------------------------------------------------------------- */
/* ensure room for `extra` more bytes, growing the wire buffer through the hook */
static int i_dart_schema_builder_reserve(DartSchemaBuilder *b, size_t extra){
    size_t newcap; uint8_t *nb;
    if (b->err) return 0;
    if (b->len + extra <= b->cap) return 1;
    newcap = b->cap ? b->cap : 64u;
    while (newcap < b->len + extra){
        if (newcap > ((size_t)-1) / 2u){ newcap = b->len + extra; break; }
        newcap *= 2u;
    }
    nb = (uint8_t *)b->alloc(b->user, b->buf, newcap);
    if (!nb){ b->err = -1; return 0; }              /* realloc failure leaves b->buf intact */
    b->buf = nb; b->cap = newcap;
    return 1;
}
static void i_dart_schema_builder_put(DartSchemaBuilder *b, uint8_t v){
    if (!i_dart_schema_builder_reserve(b, 1)) return;
    b->buf[b->len++] = v;
}
static void i_dart_schema_builder_put_u16(DartSchemaBuilder *b, uint16_t v){
    if (!i_dart_schema_builder_reserve(b, 2)) return;
    i_dart_le_w16(b->buf + b->len, v); b->len += 2;
}
static void i_dart_schema_builder_put_name(DartSchemaBuilder *b, const char *name){
    size_t n = 0, i; if (name) while (name[n]) n++;
    if (n > 255){ b->err = -4; return; }
    if (!i_dart_schema_builder_reserve(b, 1 + n)) return;
    b->buf[b->len++] = (uint8_t)n;
    for (i = 0; i < n; i++) b->buf[b->len++] = (uint8_t)name[i];
}
/* count a field on the current innermost open struct */
static void i_dart_schema_builder_count(DartSchemaBuilder *b){
    uint16_t *c;
    if (b->err) return;
    if (b->depth == 0){ b->err = -3; return; }
    c = &b->field_count[b->depth - 1];
    if (*c >= 255){ b->err = -5; return; }   /* nfields is a u8 */
    (*c)++;
}
/* open a struct type: [u8 STRUCT][u8 nfields placeholder], push a nesting level */
static void i_dart_schema_builder_open_struct(DartSchemaBuilder *b){
    if (b->err) return;
    if (b->depth >= DART_SCHEMA_MAX_DEPTH){ b->err = -2; return; }
    i_dart_schema_builder_put(b, (uint8_t)DART_STRUCT);
    b->count_pos[b->depth] = b->len;
    i_dart_schema_builder_put(b, 0);
    b->field_count[b->depth] = 0;
    b->depth++;
}

DartSchemaBuilder dart_schema_begin(DartAllocFn alloc, void *user, const char *root_name){
    DartSchemaBuilder b;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user;
    if (!alloc){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; return b; }
    i_dart_schema_builder_put(&b, (uint8_t)DART_SCHEMA_WIRE_VERSION);
    i_dart_schema_builder_put_name(&b, root_name);
    i_dart_schema_builder_open_struct(&b);              /* the root is a struct; depth -> 1 */
    return b;
}

void dart_schema_field(DartSchemaBuilder *b, const char *name, DartSchemaTypeKind kind){
    if (!b || b->err) return;
    if (dart_schema_scalar_size(kind) == 0){ b->err = -6; return; }  /* fixed scalars only */
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name); i_dart_schema_builder_put(b, (uint8_t)kind);
}

void dart_schema_field_array(DartSchemaBuilder *b, const char *name,
                             DartSchemaTypeKind elem_scalar, uint16_t count){
    if (!b || b->err) return;
    if (dart_schema_scalar_size(elem_scalar) == 0){ b->err = -6; return; }  /* scalar elements only */
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_ARR); i_dart_schema_builder_put(b, (uint8_t)elem_scalar); i_dart_schema_builder_put_u16(b, count);
}

void dart_schema_field_string(DartSchemaBuilder *b, const char *name, uint16_t cap){
    if (!b || b->err) return;
    if (cap == 0){ b->err = -6; return; }
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_STR); i_dart_schema_builder_put_u16(b, cap);
}

void dart_schema_field_string_array(DartSchemaBuilder *b, const char *name,
                                    uint16_t cap, uint16_t count){
    if (!b || b->err) return;
    if (cap == 0){ b->err = -6; return; }
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_ARR); i_dart_schema_builder_put(b, (uint8_t)DART_STR);
    i_dart_schema_builder_put_u16(b, count); i_dart_schema_builder_put_u16(b, cap);
}

void dart_schema_begin_struct(DartSchemaBuilder *b, const char *name){
    if (!b || b->err) return;
    i_dart_schema_builder_count(b);                 /* a field of the parent */
    i_dart_schema_builder_put_name(b, name);        /* the field's name */
    i_dart_schema_builder_open_struct(b);           /* the field's type: a struct */
}

void dart_schema_end_struct(DartSchemaBuilder *b){
    if (!b || b->err) return;
    if (b->depth <= 1){ b->err = -3; return; }   /* the root closes in finish */
    b->depth--;
    b->buf[b->count_pos[b->depth]] = (uint8_t)b->field_count[b->depth];
}

DartSchema *dart_schema_finish(DartSchemaBuilder *b){
    DartSchema *s = NULL;
    if (b && !b->err && b->depth == 1){                       /* depth != 1 = unbalanced begin/end */
        size_t need; uint8_t *nb; uint32_t total = 0;
        b->buf[b->count_pos[0]] = (uint8_t)b->field_count[0]; /* backpatch the root field count */
        i_dart_schema_wire_fields(b->buf, b->len, &total);    /* every field, all depths */
        need = b->len + 7u + sizeof(DartSchema)
             + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
        nb = (uint8_t *)b->alloc(b->user, b->buf, need);      /* resize the block to hold the handle */
        if (nb){ b->buf = nb; b->cap = need; s = i_dart_schema_compile(b->buf, b->len, b->cap); }
    }
    if (!s && b && b->buf) b->alloc(b->user, b->buf, 0);      /* free on any failure */
    if (b) b->buf = NULL;                                     /* owned by s now, or freed */
    return s;
}

/* ---- parse a received schema ------------------------------------------------------- */
/* bytes a compiled schema needs for `wire`: copy + alignment pad + handle + the full
   flat field table. 0 if the wire is malformed. */
static size_t i_dart_schema_compiled_size(const void *wire, size_t wire_len){
    uint32_t total;
    if (!i_dart_schema_wire_fields(wire, wire_len, &total) || total > 0xFFFFu) return 0;
    return wire_len + 7u + sizeof(DartSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
}

DartSchema *dart_schema_parse(const void *wire, size_t wire_len, DartAllocFn alloc, void *user){
    size_t need, i; uint8_t *buf; DartSchema *s;
    if (!wire || !alloc || wire_len == 0) return NULL;
    need = i_dart_schema_compiled_size(wire, wire_len);
    if (need == 0) return NULL;                              /* malformed header */
    buf = (uint8_t *)alloc(user, NULL, need);
    if (!buf) return NULL;
    for (i = 0; i < wire_len; i++) buf[i] = ((const uint8_t *)wire)[i];   /* persist the bytes */
    s = i_dart_schema_compile(buf, wire_len, need);
    if (!s) alloc(user, buf, 0);                             /* malformed body: free, no leak */
    return s;
}

void dart_schema_free(DartSchema *s, DartAllocFn alloc, void *user){
    if (s && alloc) alloc(user, (void *)s->wire.data, 0);    /* wire.data is the block base */
}

/* ---- queries ----------------------------------------------------------------------- */
DartBytes dart_schema_wire(const DartSchema *s){
    DartBytes b; if (s) return s->wire;
    b.data = NULL; b.len = 0; return b;
}
uint64_t   dart_schema_hash(const DartSchema *s){ return s ? s->hash : 0; }
DartString dart_schema_name(const DartSchema *s){
    DartString n; if (s) return s->name;
    n.data = NULL; n.len = 0; return n;
}
uint32_t dart_schema_size(const DartSchema *s){ return s ? s->size : 0; }
uint16_t dart_schema_field_count(const DartSchema *s){ return s ? s->nfields : 0; }

int dart_schema_field_at(const DartSchema *s, uint16_t i, DartSchemaFieldInfo *out){
    const i_Field *f;
    if (!s || i >= s->nfields) return 0;
    f = &s->fields[i];
    if (out){
        out->name = f->name;
        out->kind = f->kind; out->elem = f->elem;
        out->count = f->count; out->depth = f->depth;
        out->str_cap = f->str_cap;
        out->offset = f->offset; out->size = f->size;
    }
    return 1;
}

/* Match a dotted path against a field: the last segment is its own name, the ones
 * before it its ancestors, and the first segment must sit at the root. */
static int i_dart_schema_path_match(const DartSchema *s, const i_Field *f, const char *path,
                                    size_t path_len){
    const char *end = path + path_len;
    for (;;){
        const char *seg = end;
        while (seg > path && seg[-1] != '.') seg--;
        if (f->name.len != (size_t)(end - seg) ||
            (f->name.len && memcmp(f->name.data, seg, f->name.len) != 0)) return 0;
        if (f->parent == 0xFFFFu) return seg == path;          /* root: all segments consumed */
        if (seg == path) return 0;                             /* segments ran out early */
        end = seg - 1;                                         /* past the '.' */
        f = &s->fields[f->parent];
    }
}

static const i_Field *i_dart_schema_field_by_path(const DartSchema *s, const char *path){
    uint16_t i; size_t n;
    if (!s || !path) return NULL;
    n = strlen(path);
    for (i = 0; i < s->nfields; i++)
        if (i_dart_schema_path_match(s, &s->fields[i], path, n)) return &s->fields[i];
    return NULL;
}

int dart_schema_field_index(const DartSchema *s, const char *path){
    const i_Field *f = i_dart_schema_field_by_path(s, path);
    return f ? (int)(f - s->fields) : -1;
}

/* ---- reader/writer compatibility ----------------------------------------------------- */
/* find a TOP-LEVEL field by name (the match predicate works on top-level fields; a
 * nested struct is compared as one exact-encoded unit) */
static const i_Field *i_dart_schema_find(const DartSchema *s, DartString name){
    uint16_t i;
    for (i = 0; i < s->nfields; i++){
        const i_Field *f = &s->fields[i];
        if (f->depth == 0 && f->name.len == name.len &&
            (name.len == 0 || memcmp(f->name.data, name.data, name.len) == 0)) return f;
    }
    return NULL;
}

/* bounded appenders for the subset-why text (no stdio; a NULL buffer skips all text) */
static char *i_dart_why_str(char *p, char *end, const char *s){
    if (!p) return NULL;
    while (*s && p < end) *p++ = *s++;
    return p;
}
static char *i_dart_why_view(char *p, char *end, DartString s){
    size_t i;
    if (!p) return NULL;
    for (i = 0; i < s.len && p < end; i++) *p++ = s.data[i];
    return p;
}
static char *i_dart_why_u(char *p, char *end, uint32_t v){
    char tmp[10]; int n = 0;
    if (!p) return NULL;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
static const char *i_dart_why_kind(uint8_t k){
    switch (k){
        case DART_U8:  return "u8";  case DART_U16: return "u16";
        case DART_U32: return "u32"; case DART_U64: return "u64";
        case DART_I8:  return "i8";  case DART_I16: return "i16";
        case DART_I32: return "i32"; case DART_I64: return "i64";
        case DART_F32: return "f32"; case DART_F64: return "f64";
        case DART_BOOL: return "bool"; case DART_STRUCT: return "struct";
        default: return "?";
    }
}
/* a field's type in DSL form: "f32[8]", "string<33>", "string<16>[4]", "struct" */
static char *i_dart_why_type(char *p, char *end, const i_Field *f){
    uint8_t elem = f->kind == DART_ARR ? f->elem : f->kind;
    if (elem == DART_STR){
        p = i_dart_why_str(p, end, "string<");
        p = i_dart_why_u(p, end, f->str_cap);
        p = i_dart_why_str(p, end, ">");
    } else {
        p = i_dart_why_str(p, end, i_dart_why_kind(elem));
    }
    if (f->kind == DART_ARR){
        p = i_dart_why_str(p, end, "[");
        p = i_dart_why_u(p, end, f->count);
        p = i_dart_why_str(p, end, "]");
    }
    return p;
}
static int i_dart_why_done(char *buf, char *p){   /* NUL-terminate the reason, refuse */
    if (buf) *p = '\0';
    return 0;
}

int dart_schema_subset_why(const DartSchema *sub, const DartSchema *pub,
                           char *buf, size_t cap){
    uint16_t i;
    char *p = (buf && cap) ? buf : NULL, *end = p ? buf + cap - 1 : NULL;
    if (p) *p = '\0';
    if (!sub || !pub)
        return i_dart_why_done(p, i_dart_why_str(p, end, "schema missing"));
    if (sub->name.len != pub->name.len ||
        (sub->name.len && memcmp(sub->name.data, pub->name.data, sub->name.len) != 0)){
        p = i_dart_why_str(p, end, "reader type '"); p = i_dart_why_view(p, end, sub->name);
        p = i_dart_why_str(p, end, "' != writer type '"); p = i_dart_why_view(p, end, pub->name);
        return i_dart_why_done(buf, i_dart_why_str(p, end, "'"));
    }
    for (i = 0; i < sub->nfields; i++){
        const i_Field *a = &sub->fields[i], *b;
        if (a->depth != 0) continue;                          /* members ride their struct */
        b = i_dart_schema_find(pub, a->name);
        if (!b){
            p = i_dart_why_str(p, end, "field '"); p = i_dart_why_view(p, end, a->name);
            return i_dart_why_done(buf, i_dart_why_str(p, end, "' missing from writer"));
        }
        if (a->kind != b->kind ||
            (a->kind == DART_ARR &&
             (a->elem != b->elem || a->count != b->count || a->str_cap != b->str_cap)) ||
            (a->kind == DART_STR && a->str_cap != b->str_cap)){
            p = i_dart_why_str(p, end, "field '"); p = i_dart_why_view(p, end, a->name);
            p = i_dart_why_str(p, end, "': reader "); p = i_dart_why_type(p, end, a);
            p = i_dart_why_str(p, end, ", writer ");
            return i_dart_why_done(buf, i_dart_why_type(p, end, b));
        }
        if (a->kind == DART_STRUCT &&                         /* nested: exact type encoding */
            (a->type_len != b->type_len ||
             memcmp(sub->wire.data + a->type_off, pub->wire.data + b->type_off, a->type_len) != 0)){
            p = i_dart_why_str(p, end, "field '"); p = i_dart_why_view(p, end, a->name);
            return i_dart_why_done(buf, i_dart_why_str(p, end, "': nested struct layout differs"));
        }
    }
    return 1;
}

int dart_schema_subset(const DartSchema *sub, const DartSchema *pub){
    return dart_schema_subset_why(sub, pub, NULL, 0);
}

DartSchema *dart_schema_rebase(const DartSchema *sub, const DartSchema *pub,
                               DartAllocFn alloc, void *user){
    DartSchema *r; uint16_t i; uint32_t delta = 0;
    if (!alloc || !dart_schema_subset(sub, pub)) return NULL;
    r = dart_schema_parse(sub->wire.data, sub->wire.len, alloc, user);
    if (!r) return NULL;
    for (i = 0; i < r->nfields; i++){                         /* the writer's layout... */
        i_Field *f = &r->fields[i];
        if (f->depth == 0)                                    /* members shift with their struct */
            delta = i_dart_schema_find(pub, f->name)->offset - f->offset;
        f->offset += delta;
    }
    r->size = pub->size;                                      /* ...and the writer's message size */
    return r;
}

/* ---- read a message ---------------------------------------------------------------- */
int dart_schema_validate(const DartSchema *s, DartBytes msg){
    return s ? (msg.len == s->size) : 0;
}

/* Resolve a field by dotted path and bounds-check it against the message; NULL if
 * unknown or the message is too short. */
static const i_Field *i_dart_schema_read_lookup(const DartSchema *s, DartBytes msg, const char *field){
    const i_Field *f = i_dart_schema_field_by_path(s, field);
    if (!f || (size_t)f->offset + f->size > msg.len) return NULL;
    return f;
}

static uint64_t i_dart_schema_read_uint(const i_Field *f, const uint8_t *p){
    switch (f->kind){
        case DART_U8: case DART_BOOL: return p[0];
        case DART_U16: return i_dart_le_r16(p);
        case DART_U32: return i_dart_le_r32(p);
        case DART_U64: return i_dart_le_r64(p);
        default: return 0;
    }
}
static int64_t i_dart_schema_read_int(const i_Field *f, const uint8_t *p){
    switch (f->kind){
        case DART_I8:  return (int8_t)p[0];
        case DART_I16: return (int16_t)i_dart_le_r16(p);
        case DART_I32: return (int32_t)i_dart_le_r32(p);
        case DART_I64: return (int64_t)i_dart_le_r64(p);
        default: return 0;
    }
}
static double i_dart_schema_read_f64(const i_Field *f, const uint8_t *p){
    if (f->kind == DART_F64){ uint64_t b = i_dart_le_r64(p); double d; memcpy(&d, &b, 8); return d; }
    if (f->kind == DART_F32){ uint32_t b = i_dart_le_r32(p); float  x; memcpy(&x, &b, 4); return (double)x; }
    return 0.0;
}

uint64_t dart_get_uint(DartBytes msg, const DartSchema *s, const char *field){
    const i_Field *f = i_dart_schema_read_lookup(s, msg, field);
    return f ? i_dart_schema_read_uint(f, msg.data + f->offset) : 0;
}

int64_t dart_get_int(DartBytes msg, const DartSchema *s, const char *field){
    const i_Field *f = i_dart_schema_read_lookup(s, msg, field);
    return f ? i_dart_schema_read_int(f, msg.data + f->offset) : 0;
}

double dart_get_f64(DartBytes msg, const DartSchema *s, const char *field){
    const i_Field *f = i_dart_schema_read_lookup(s, msg, field);
    return f ? i_dart_schema_read_f64(f, msg.data + f->offset) : 0.0;
}

float dart_get_f32(DartBytes msg, const DartSchema *s, const char *field){
    const i_Field *f = i_dart_schema_read_lookup(s, msg, field);
    if (!f || f->kind != DART_F32) return 0.0f;
    { uint32_t b = i_dart_le_r32(msg.data + f->offset); float x; memcpy(&x, &b, 4); return x; }
}

DartBytes dart_get_array(DartBytes msg, const DartSchema *s, const char *field){
    DartBytes out; const i_Field *f = i_dart_schema_read_lookup(s, msg, field);
    out.data = NULL; out.len = 0;
    if (!f || f->kind != DART_ARR) return out;
    out.data = msg.data + f->offset; out.len = f->size;
    return out;
}

/* live-string view of one slot ([u16 len][cap bytes]); the length is clamped to the cap
   so a hostile message can never over-read */
static DartString i_dart_schema_str_view(const uint8_t *slot, uint16_t cap){
    uint16_t len = i_dart_le_r16(slot);
    if (len > cap) len = cap;
    return dart_string((const char *)(slot + 2), len);
}

DartString dart_get_string(DartBytes msg, const DartSchema *s, const char *field){
    const i_Field *f = i_dart_schema_read_lookup(s, msg, field);
    if (!f || f->kind != DART_STR) return dart_string(NULL, 0);
    return i_dart_schema_str_view(msg.data + f->offset, f->str_cap);
}

DartString dart_get_string_at(DartBytes msg, const DartSchema *s, const char *field,
                              uint16_t index){
    const i_Field *f = i_dart_schema_read_lookup(s, msg, field);
    if (!f || f->kind != DART_ARR || f->elem != DART_STR || index >= f->count)
        return dart_string(NULL, 0);
    return i_dart_schema_str_view(msg.data + f->offset + (size_t)index * (2u + f->str_cap),
                                  f->str_cap);
}

int dart_get_value(DartBytes msg, const DartSchema *s, uint16_t field, DartValue *out){
    const i_Field *f; const uint8_t *p;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if ((size_t)f->offset + f->size > msg.len) return 0;
    p = msg.data + f->offset;
    out->kind = f->kind; out->elem = f->elem; out->count = f->count; out->str_cap = f->str_cap;
    switch (f->kind){
        case DART_U8: case DART_U16: case DART_U32: case DART_U64: case DART_BOOL:
            out->v.u = i_dart_schema_read_uint(f, p); break;
        case DART_I8: case DART_I16: case DART_I32: case DART_I64:
            out->v.i = i_dart_schema_read_int(f, p); break;
        case DART_F32: case DART_F64:
            out->v.f = i_dart_schema_read_f64(f, p); break;
        case DART_ARR: case DART_STRUCT:
            out->bytes = dart_bytes(p, f->size); break;
        case DART_STR: {
            DartString sv = i_dart_schema_str_view(p, f->str_cap);
            out->bytes = dart_bytes(sv.data, sv.len); break;
        }
        default: return 0;
    }
    return 1;
}

/* ---- write a message ----------------------------------------------------------------- */
int dart_schema_message_default(const DartSchema *s, void *buf, size_t cap){
    if (!s || !buf || cap < s->size) return 0;
    memset(buf, 0, s->size);
    return 1;
}

static const i_Field *i_dart_schema_set_lookup(const DartSchema *s, const void *buf, size_t cap,
                                               const char *field){
    const i_Field *f;
    if (!buf) return NULL;
    f = i_dart_schema_field_by_path(s, field);
    if (!f || (size_t)f->offset + f->size > cap) return NULL;
    return f;
}

static int i_dart_schema_write_uint(const i_Field *f, uint8_t *p, uint64_t v){
    switch (f->kind){
        case DART_U8:   p[0] = (uint8_t)v;  return 1;
        case DART_BOOL: p[0] = v ? 1u : 0u; return 1;
        case DART_U16: i_dart_le_w16(p, (uint16_t)v); return 1;
        case DART_U32: i_dart_le_w32(p, (uint32_t)v); return 1;
        case DART_U64: i_dart_le_w64(p, v); return 1;
        default: return 0;
    }
}
static int i_dart_schema_write_int(const i_Field *f, uint8_t *p, int64_t v){
    switch (f->kind){
        case DART_I8:  p[0] = (uint8_t)v; return 1;
        case DART_I16: i_dart_le_w16(p, (uint16_t)v); return 1;
        case DART_I32: i_dart_le_w32(p, (uint32_t)v); return 1;
        case DART_I64: i_dart_le_w64(p, (uint64_t)v); return 1;
        default: return 0;
    }
}
static int i_dart_schema_write_f64(const i_Field *f, uint8_t *p, double v){
    if (f->kind == DART_F64){ uint64_t b; memcpy(&b, &v, 8); i_dart_le_w64(p, b); return 1; }
    if (f->kind == DART_F32){ float x = (float)v; uint32_t b; memcpy(&b, &x, 4); i_dart_le_w32(p, b); return 1; }
    return 0;
}
/* write one string slot: [u16 len][bytes][zeroed tail]; refuses v.len > cap */
static int i_dart_schema_write_str_slot(uint8_t *p, uint16_t cap, DartString v){
    if (v.len > cap || (v.len && !v.data)) return 0;             /* never silently truncate */
    i_dart_le_w16(p, (uint16_t)v.len);
    if (v.len) memcpy(p + 2, v.data, v.len);
    memset(p + 2 + v.len, 0, (size_t)cap - v.len);
    return 1;
}
/* copy elems over the front of the array, zero the rest; refuses misfits */
static int i_dart_schema_write_array(const i_Field *f, uint8_t *p, DartBytes elems){
    uint32_t esz;
    if (f->kind != DART_ARR) return 0;
    esz = (f->elem == DART_STR) ? 2u + f->str_cap
                                : dart_schema_scalar_size((DartSchemaTypeKind)f->elem);
    if (elems.len > f->size || (esz && elems.len % esz != 0)) return 0;  /* never silently truncate */
    if (elems.len && !elems.data) return 0;
    if (f->elem == DART_STR){                       /* whole slots: every length must fit its cap */
        size_t off;
        for (off = 0; off + esz <= elems.len; off += esz)
            if (i_dart_le_r16(elems.data + off) > f->str_cap) return 0;
    }
    if (elems.len) memcpy(p, elems.data, elems.len);
    memset(p + elems.len, 0, f->size - elems.len);
    return 1;
}

int dart_set_uint(void *buf, size_t cap, const DartSchema *s, const char *field, uint64_t v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field);
    return f ? i_dart_schema_write_uint(f, (uint8_t *)buf + f->offset, v) : 0;
}

int dart_set_int(void *buf, size_t cap, const DartSchema *s, const char *field, int64_t v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field);
    return f ? i_dart_schema_write_int(f, (uint8_t *)buf + f->offset, v) : 0;
}

int dart_set_f64(void *buf, size_t cap, const DartSchema *s, const char *field, double v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field);
    return f ? i_dart_schema_write_f64(f, (uint8_t *)buf + f->offset, v) : 0;
}

int dart_set_f32(void *buf, size_t cap, const DartSchema *s, const char *field, float v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field); uint32_t b;
    if (!f || f->kind != DART_F32 || (size_t)f->offset + 4 > cap) return 0;
    memcpy(&b, &v, 4); i_dart_le_w32((uint8_t *)buf + f->offset, b);
    return 1;
}

int dart_set_array(void *buf, size_t cap, const DartSchema *s, const char *field, DartBytes elems){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field);
    return f ? i_dart_schema_write_array(f, (uint8_t *)buf + f->offset, elems) : 0;
}

int dart_set_string(void *buf, size_t cap, const DartSchema *s, const char *field, DartString v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field);
    if (!f || f->kind != DART_STR) return 0;
    return i_dart_schema_write_str_slot((uint8_t *)buf + f->offset, f->str_cap, v);
}

int dart_set_string_at(void *buf, size_t cap, const DartSchema *s, const char *field,
                       uint16_t index, DartString v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field);
    if (!f || f->kind != DART_ARR || f->elem != DART_STR || index >= f->count) return 0;
    return i_dart_schema_write_str_slot((uint8_t *)buf + f->offset + (size_t)index * (2u + f->str_cap),
                                        f->str_cap, v);
}

int dart_set_value(void *buf, size_t cap, const DartSchema *s, uint16_t field, const DartValue *val){
    const i_Field *f; uint8_t *p;
    if (!buf || !val || !s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if ((size_t)f->offset + f->size > cap) return 0;
    p = (uint8_t *)buf + f->offset;
    switch (f->kind){
        case DART_U8: case DART_U16: case DART_U32: case DART_U64: case DART_BOOL:
            return i_dart_schema_write_uint(f, p, val->v.u);
        case DART_I8: case DART_I16: case DART_I32: case DART_I64:
            return i_dart_schema_write_int(f, p, val->v.i);
        case DART_F32: case DART_F64:
            return i_dart_schema_write_f64(f, p, val->v.f);
        case DART_ARR:
            return i_dart_schema_write_array(f, p, val->bytes);
        case DART_STR:
            return i_dart_schema_write_str_slot(p, f->str_cap,
                       dart_string((const char *)val->bytes.data, val->bytes.len));
        case DART_STRUCT:                                  /* raw bytes, short = zero-filled tail */
            if (val->bytes.len > f->size || (val->bytes.len && !val->bytes.data)) return 0;
            if (val->bytes.len) memcpy(p, val->bytes.data, val->bytes.len);
            memset(p + val->bytes.len, 0, f->size - val->bytes.len);
            return 1;
        default: return 0;
    }
}

/* ---- the schema DSL ------------------------------------------------------------------ */
/* dart_schema_compile input (see schema.h for the full doc):
 *   schema := name '{' fields '}'
 *   field  := name ':' type  (',')?          fields self-delimit; commas optional
 *   type   := base | base '[' count ']' | '{' fields '}'
 *   base   := scalar | 'string' '<' cap '>'
 * The parser is a thin front end over the builder, so all structural limits (name
 * lengths, field counts, nesting depth) are the builder's. */
typedef struct { const char *p; const char *err; } i_DartDsl;

static void i_dart_dsl_ws(i_DartDsl *d){
    for (;;){
        while (*d->p==' ' || *d->p=='\t' || *d->p=='\r' || *d->p=='\n') d->p++;
        if (d->p[0]=='-' && d->p[1]=='-'){ while (*d->p && *d->p!='\n') d->p++; continue; }
        return;
    }
}
static void i_dart_dsl_fail(i_DartDsl *d, const char *at){ if (!d->err) d->err = at; }
/* identifier into out[256] (NUL-terminated); 0 + err on missing/overlong */
static int i_dart_dsl_ident(i_DartDsl *d, char out[256]){
    const char *q = d->p; size_t n;
    if (!((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || *q=='_')){ i_dart_dsl_fail(d, q); return 0; }
    while ((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || (*q>='0'&&*q<='9') || *q=='_') q++;
    n = (size_t)(q - d->p);
    if (n > 255){ i_dart_dsl_fail(d, d->p); return 0; }
    memcpy(out, d->p, n); out[n] = '\0';
    d->p = q;
    return 1;
}
static int i_dart_dsl_expect(i_DartDsl *d, char c){
    if (*d->p == c){ d->p++; return 1; }
    i_dart_dsl_fail(d, d->p);
    return 0;
}
/* decimal array count, 1..65535 */
static int i_dart_dsl_count(i_DartDsl *d, uint16_t *out){
    const char *at = d->p; uint32_t v = 0;
    while (*d->p>='0' && *d->p<='9'){
        v = v*10u + (uint32_t)(*d->p - '0');
        if (v > 0xFFFFu){ i_dart_dsl_fail(d, at); return 0; }
        d->p++;
    }
    if (d->p == at || v == 0){ i_dart_dsl_fail(d, at); return 0; }
    *out = (uint16_t)v;
    return 1;
}
static int i_dart_dsl_kind(const char *s, DartSchemaTypeKind *k){
    static const struct { const char *word; uint8_t kind; } table[] = {
        {"u8",DART_U8},{"u16",DART_U16},{"u32",DART_U32},{"u64",DART_U64},
        {"i8",DART_I8},{"i16",DART_I16},{"i32",DART_I32},{"i64",DART_I64},
        {"f32",DART_F32},{"f64",DART_F64},{"bool",DART_BOOL} };
    size_t i;
    for (i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(s, table[i].word) == 0){ *k = (DartSchemaTypeKind)table[i].kind; return 1; }
    return 0;
}

/* fields of one struct body, up to (not consuming) the closing '}' */
static void i_dart_dsl_fields(i_DartDsl *d, DartSchemaBuilder *b){
    char name[256], tname[256];
    for (;;){
        i_dart_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) return;
        if (!i_dart_dsl_ident(d, name)) return;
        i_dart_dsl_ws(d);
        if (!i_dart_dsl_expect(d, ':')) return;
        i_dart_dsl_ws(d);
        if (*d->p == '{'){                                   /* nested struct */
            d->p++;
            dart_schema_begin_struct(b, name);
            i_dart_dsl_fields(d, b);
            if (!i_dart_dsl_expect(d, '}')) return;
            dart_schema_end_struct(b);
        } else {
            const char *at = d->p; DartSchemaTypeKind k;
            int is_str = 0; uint16_t str_cap = 0;
            if (!i_dart_dsl_ident(d, tname)) return;
            if (strcmp(tname, "string") == 0){               /* string<cap> */
                is_str = 1;
                i_dart_dsl_ws(d);
                if (!i_dart_dsl_expect(d, '<')) return;
                i_dart_dsl_ws(d);
                if (!i_dart_dsl_count(d, &str_cap)) return;
                i_dart_dsl_ws(d);
                if (!i_dart_dsl_expect(d, '>')) return;
            } else if (!i_dart_dsl_kind(tname, &k)){ i_dart_dsl_fail(d, at); return; }   /* unknown type */
            i_dart_dsl_ws(d);
            if (*d->p == '['){
                uint16_t count;
                d->p++;
                i_dart_dsl_ws(d);
                if (!i_dart_dsl_count(d, &count)) return;
                i_dart_dsl_ws(d);
                if (!i_dart_dsl_expect(d, ']')) return;
                if (is_str) dart_schema_field_string_array(b, name, str_cap, count);
                else        dart_schema_field_array(b, name, k, count);
            } else {
                if (is_str) dart_schema_field_string(b, name, str_cap);
                else        dart_schema_field(b, name, k);
            }
        }
        i_dart_dsl_ws(d);
        if (*d->p == ',') d->p++;                            /* optional separator */
    }
}

DartSchema *dart_schema_compile(DartAllocFn alloc, void *user, const char *text, const char **err){
    i_DartDsl d; char root[256]; DartSchemaBuilder b; DartSchema *s;
    if (err) *err = NULL;
    if (!alloc || !text){ return NULL; }
    d.p = text; d.err = NULL;
    i_dart_dsl_ws(&d);
    if (!i_dart_dsl_ident(&d, root)){ if (err) *err = d.err; return NULL; }
    i_dart_dsl_ws(&d);
    b = dart_schema_begin(alloc, user, root);
    if (i_dart_dsl_expect(&d, '{')){
        i_dart_dsl_fields(&d, &b);
        if (i_dart_dsl_expect(&d, '}')){
            i_dart_dsl_ws(&d);
            if (*d.p) i_dart_dsl_fail(&d, d.p);              /* trailing garbage */
        }
    }
    if (d.err && !b.err) b.err = -7;                         /* parse error: make finish fail */
    s = dart_schema_finish(&b);                              /* frees everything on any error */
    if (!s && err) *err = d.err ? d.err : d.p;
    return s;
}
