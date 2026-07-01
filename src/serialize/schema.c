#include "schema.h"
#include "../common/bytes.h"   /* dart_le_* */
#include "../common/hash.h"    /* i_dart_fnv1a64 (the schema id) */
#include <string.h>            /* memcpy (float bit reinterpret) */

/* The schema wire format (what the builder emits and dart_schema_parse reads):
 *   schema := [u8 version][u8 root_namelen][root_name...][type]   ; root type is a STRUCT
 *   type   := [u8 kind] payload
 *     scalar (U8..BOOL)      : (none; size implied by kind)
 *     FIX                    : [u16 size]
 *     CAPARR                 : [u16 cap][type elem]               ; elem must be fixed
 *     STRUCT                 : [u8 nfields] ( [u8 namelen][name][type] )*nfields
 * Every type is fixed: a field's byte offset is the sum of the preceding field sizes. */

/* The per-field record the compiled schema stores for every top-level field. */
typedef struct {
    DartString name;      /* field name, a view into the wire bytes */
    uint32_t   offset;    /* byte offset in a message */
    uint32_t   size;      /* byte size */
    uint16_t   cap;       /* CAPARR capacity, else 0 */
    uint8_t    kind;
    uint8_t    elem;      /* CAPARR element kind, else 0 */
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

/* Byte size of the type at r->pos; advances r past it. Fails on an unknown/variable kind. */
static uint32_t i_dart_rd_type_size(i_Rd *r){
    uint8_t k = i_dart_rd_u8(r);
    if (r->fail) return 0;
    switch (k){
        case DART_U8: case DART_I8: case DART_BOOL: return 1;
        case DART_U16: case DART_I16:               return 2;
        case DART_U32: case DART_I32: case DART_F32: return 4;
        case DART_U64: case DART_I64: case DART_F64: return 8;
        case DART_FIX: return i_dart_rd_u16(r);
        case DART_CAPARR: {
            uint16_t cap = i_dart_rd_u16(r); uint32_t es = i_dart_rd_type_size(r);
            if (r->fail || es == 0){ r->fail = 1; return 0; }    /* needs a fixed element */
            return (uint32_t)2 + (uint32_t)cap * es;              /* [u16 count] + cap elements */
        }
        case DART_STRUCT: {
            uint8_t nf = i_dart_rd_u8(r); uint32_t sum = 0; uint16_t i;
            for (i = 0; i < nf && !r->fail; i++){
                uint8_t fl = i_dart_rd_u8(r);
                i_dart_rd_skip(r, fl);
                sum += i_dart_rd_type_size(r);
            }
            return sum;
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

/* Compile wire bytes already sitting at buf[0..wire_len] into a DartSchema placed after
 * them in buf. Returns NULL on a malformed blob or if buf[cap] is too small. */
static DartSchema *i_dart_schema_compile(uint8_t *buf, size_t wire_len, size_t cap){
    i_Rd r; uint8_t ver, root_kind, root_namelen, nfields;
    const char *root_name; size_t hoff, need; DartSchema *s;
    uint16_t i; uint32_t running;

    r.w = buf; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_dart_rd_u8(&r);
    if (r.fail || ver != DART_SCHEMA_WIRE_VERSION) return NULL;
    root_namelen = i_dart_rd_u8(&r);
    root_name = (const char *)(buf + r.pos);
    i_dart_rd_skip(&r, root_namelen);
    root_kind = i_dart_rd_u8(&r);
    if (r.fail || root_kind != DART_STRUCT) return NULL;    /* the root must be a struct */
    nfields = i_dart_rd_u8(&r);
    if (r.fail) return NULL;

    hoff = i_dart_schema_handle_off(buf, wire_len);
    need = hoff + sizeof(DartSchema) + (size_t)(nfields ? nfields - 1u : 0u) * sizeof(i_Field);
    if (need > cap) return NULL;

    s = (DartSchema *)(buf + hoff);
    s->wire = dart_bytes(buf, wire_len);
    s->name = dart_string(root_name, root_namelen);
    s->nfields = nfields;
    s->hash = i_dart_fnv1a64(buf, wire_len);

    running = 0;
    for (i = 0; i < nfields; i++){
        i_Field *f = &s->fields[i];
        uint8_t fl = i_dart_rd_u8(&r); const char *fn = (const char *)(buf + r.pos);
        size_t kpos; uint32_t sz;
        i_dart_rd_skip(&r, fl);
        kpos = r.pos;
        f->name = dart_string(fn, fl);
        f->kind = (kpos < r.n) ? buf[kpos] : 0;
        f->cap = 0; f->elem = 0;
        sz = i_dart_rd_type_size(&r);
        if (r.fail) return NULL;
        if (f->kind == DART_CAPARR){                            /* capture cap/elem for readers */
            i_Rd q; q.w = buf; q.n = wire_len; q.pos = kpos + 1; q.fail = 0;
            f->cap = i_dart_rd_u16(&q);
            f->elem = i_dart_rd_u8(&q);
        }
        f->size = sz;
        f->offset = running;
        running += sz;
    }
    s->size = running;
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

void dart_schema_field_fix(DartSchemaBuilder *b, const char *name, uint16_t size){
    if (!b || b->err) return;
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name); i_dart_schema_builder_put(b, (uint8_t)DART_FIX); i_dart_schema_builder_put_u16(b, size);
}

void dart_schema_field_caparr(DartSchemaBuilder *b, const char *name,
                              DartSchemaTypeKind elem_scalar, uint16_t cap){
    if (!b || b->err) return;
    if (dart_schema_scalar_size(elem_scalar) == 0){ b->err = -6; return; }  /* fixed scalar elem */
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_CAPARR); i_dart_schema_builder_put_u16(b, cap); i_dart_schema_builder_put(b, (uint8_t)elem_scalar);
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
        size_t need; uint8_t *nb;
        b->buf[b->count_pos[0]] = (uint8_t)b->field_count[0]; /* backpatch the root field count */
        need = b->len + 7u + sizeof(DartSchema)
             + (size_t)(b->field_count[0] ? b->field_count[0] - 1u : 0u) * sizeof(i_Field);
        nb = (uint8_t *)b->alloc(b->user, b->buf, need);      /* resize the block to hold the handle */
        if (nb){ b->buf = nb; b->cap = need; s = i_dart_schema_compile(b->buf, b->len, b->cap); }
    }
    if (!s && b && b->buf) b->alloc(b->user, b->buf, 0);      /* free on any failure */
    if (b) b->buf = NULL;                                     /* owned by s now, or freed */
    return s;
}

/* ---- parse a received schema ------------------------------------------------------- */
/* bytes a compiled schema needs for `wire`: copy + alignment pad + handle + field table.
   0 if the header is malformed. */
static size_t i_dart_schema_compiled_size(const void *wire, size_t wire_len){
    i_Rd r; uint8_t ver, rl, rk, nf;
    r.w = (const uint8_t *)wire; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_dart_rd_u8(&r); if (r.fail || ver != DART_SCHEMA_WIRE_VERSION) return 0;
    rl = i_dart_rd_u8(&r); i_dart_rd_skip(&r, rl);
    rk = i_dart_rd_u8(&r); if (r.fail || rk != DART_STRUCT) return 0;
    nf = i_dart_rd_u8(&r); if (r.fail) return 0;
    return wire_len + 7u + sizeof(DartSchema) + (size_t)(nf ? nf - 1u : 0u) * sizeof(i_Field);
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
        out->cap = f->cap; out->offset = f->offset; out->size = f->size;
    }
    return 1;
}

int dart_schema_field_index(const DartSchema *s, const char *name){
    uint16_t i; size_t n = 0;
    if (!s || !name) return -1;
    while (name[n]) n++;
    for (i = 0; i < s->nfields; i++){
        const i_Field *f = &s->fields[i];
        if (f->name.len == n){
            size_t j; int eq = 1;
            for (j = 0; j < n; j++) if ((uint8_t)f->name.data[j] != (uint8_t)name[j]){ eq = 0; break; }
            if (eq) return (int)i;
        }
    }
    return -1;
}

/* ---- read a message ---------------------------------------------------------------- */
int dart_schema_validate(const DartSchema *s, DartBytes msg){
    return s ? (msg.len == s->size) : 0;
}

/* Resolve a field and bounds-check it against the message; NULL if out of range or the
 * message is too short. */
static const i_Field *i_dart_schema_field_lookup(const DartSchema *s, DartBytes msg, uint16_t field){
    const i_Field *f;
    if (!s || field >= s->nfields) return NULL;
    f = &s->fields[field];
    if ((size_t)f->offset + f->size > msg.len) return NULL;
    return f;
}

uint64_t dart_get_uint(DartBytes msg, const DartSchema *s, uint16_t field){
    const i_Field *f = i_dart_schema_field_lookup(s, msg, field); const uint8_t *p;
    if (!f) return 0;
    p = msg.data + f->offset;
    switch (f->kind){
        case DART_U8: case DART_BOOL: return p[0];
        case DART_U16: return i_dart_le_r16(p);
        case DART_U32: return i_dart_le_r32(p);
        case DART_U64: return i_dart_le_r64(p);
        default: return 0;
    }
}

int64_t dart_get_int(DartBytes msg, const DartSchema *s, uint16_t field){
    const i_Field *f = i_dart_schema_field_lookup(s, msg, field); const uint8_t *p;
    if (!f) return 0;
    p = msg.data + f->offset;
    switch (f->kind){
        case DART_I8:  return (int8_t)p[0];
        case DART_I16: return (int16_t)i_dart_le_r16(p);
        case DART_I32: return (int32_t)i_dart_le_r32(p);
        case DART_I64: return (int64_t)i_dart_le_r64(p);
        default: return 0;
    }
}

double dart_get_f64(DartBytes msg, const DartSchema *s, uint16_t field){
    const i_Field *f = i_dart_schema_field_lookup(s, msg, field);
    if (!f) return 0.0;
    if (f->kind == DART_F64){ uint64_t b = i_dart_le_r64(msg.data + f->offset); double d; memcpy(&d, &b, 8); return d; }
    if (f->kind == DART_F32){ uint32_t b = i_dart_le_r32(msg.data + f->offset); float  x; memcpy(&x, &b, 4); return (double)x; }
    return 0.0;
}

float dart_get_f32(DartBytes msg, const DartSchema *s, uint16_t field){
    const i_Field *f = i_dart_schema_field_lookup(s, msg, field);
    if (!f || f->kind != DART_F32) return 0.0f;
    { uint32_t b = i_dart_le_r32(msg.data + f->offset); float x; memcpy(&x, &b, 4); return x; }
}

DartBytes dart_get_fix(DartBytes msg, const DartSchema *s, uint16_t field){
    DartBytes out; const i_Field *f = i_dart_schema_field_lookup(s, msg, field);
    out.data = NULL; out.len = 0;
    if (!f || f->kind != DART_FIX) return out;
    out.data = msg.data + f->offset; out.len = f->size;
    return out;
}

DartBytes dart_get_caparr(DartBytes msg, const DartSchema *s, uint16_t field, uint16_t *count){
    DartBytes out; const i_Field *f = i_dart_schema_field_lookup(s, msg, field); uint16_t n, esz;
    out.data = NULL; out.len = 0; if (count) *count = 0;
    if (!f || f->kind != DART_CAPARR) return out;
    n = i_dart_le_r16(msg.data + f->offset);          /* used count, stored inline */
    if (n > f->cap) n = f->cap;                       /* clamp to capacity */
    esz = (uint16_t)dart_schema_scalar_size((DartSchemaTypeKind)f->elem);
    if (count) *count = n;
    out.data = msg.data + f->offset + 2;
    out.len  = (size_t)n * esz;
    return out;
}
