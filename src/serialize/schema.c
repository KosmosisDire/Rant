#include "schema.h"
#include "../common/bytes.h"   /* dart_le_* */
#include "../common/hash.h"    /* i_dart_fnv1a64 (the schema id) */
#include <string.h>            /* memcpy (float bit reinterpret) */

/* The schema wire format (what the builder emits and dart_schema_parse reads):
 *   schema := [u8 version][u8 root_namelen][root_name...][type]   ; root type is a STRUCT
 *   type   := [u8 kind] payload
 *     scalar (U8..BOOL)      : (none; size implied by kind)
 *     ARR                    : [u8 elem][u16 count]               ; elem must be a scalar
 *     STRUCT                 : [u8 nfields] ( [u8 namelen][name][type] )*nfields
 * Every type is fixed: a field's byte offset is the sum of the preceding field sizes. */

/* The per-field record the compiled schema stores for every top-level field. */
typedef struct {
    DartString name;      /* field name, a view into the wire bytes */
    uint32_t   offset;    /* byte offset in a message */
    uint32_t   size;      /* byte size */
    uint32_t   type_off;  /* wire offset of the field's type encoding (subset compare) */
    uint32_t   type_len;  /* wire length of the type encoding */
    uint16_t   count;     /* ARR element count, else 0 */
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

/* Byte size of the type at r->pos; advances r past it. Fails on an unknown/variable kind. */
static uint32_t i_dart_rd_type_size(i_Rd *r){
    uint8_t k = i_dart_rd_u8(r);
    if (r->fail) return 0;
    switch (k){
        case DART_U8: case DART_I8: case DART_BOOL: return 1;
        case DART_U16: case DART_I16:               return 2;
        case DART_U32: case DART_I32: case DART_F32: return 4;
        case DART_U64: case DART_I64: case DART_F64: return 8;
        case DART_ARR: {
            uint32_t es = dart_schema_scalar_size((DartSchemaTypeKind)i_dart_rd_u8(r));
            uint16_t count = i_dart_rd_u16(r);
            if (r->fail || es == 0){ r->fail = 1; return 0; }    /* elem must be a scalar */
            return (uint32_t)count * es;
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
        f->count = 0; f->elem = 0;
        sz = i_dart_rd_type_size(&r);
        if (r.fail) return NULL;
        f->type_off = (uint32_t)kpos;                           /* type extents: subset compare */
        f->type_len = (uint32_t)(r.pos - kpos);
        if (f->kind == DART_ARR){                               /* capture elem/count for readers */
            i_Rd q; q.w = buf; q.n = wire_len; q.pos = kpos + 1; q.fail = 0;
            f->elem = i_dart_rd_u8(&q);
            f->count = i_dart_rd_u16(&q);
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

void dart_schema_field_array(DartSchemaBuilder *b, const char *name,
                             DartSchemaTypeKind elem_scalar, uint16_t count){
    if (!b || b->err) return;
    if (dart_schema_scalar_size(elem_scalar) == 0){ b->err = -6; return; }  /* scalar elements only */
    i_dart_schema_builder_count(b); i_dart_schema_builder_put_name(b, name);
    i_dart_schema_builder_put(b, (uint8_t)DART_ARR); i_dart_schema_builder_put(b, (uint8_t)elem_scalar); i_dart_schema_builder_put_u16(b, count);
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
        out->count = f->count; out->offset = f->offset; out->size = f->size;
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

/* ---- reader/writer compatibility ----------------------------------------------------- */
static const i_Field *i_dart_schema_find(const DartSchema *s, DartString name){
    uint16_t i;
    for (i = 0; i < s->nfields; i++){
        const i_Field *f = &s->fields[i];
        if (f->name.len == name.len &&
            (name.len == 0 || memcmp(f->name.data, name.data, name.len) == 0)) return f;
    }
    return NULL;
}

int dart_schema_subset(const DartSchema *sub, const DartSchema *pub){
    uint16_t i;
    if (!sub || !pub) return 0;
    if (sub->name.len != pub->name.len ||
        (sub->name.len && memcmp(sub->name.data, pub->name.data, sub->name.len) != 0)) return 0;
    for (i = 0; i < sub->nfields; i++){
        const i_Field *a = &sub->fields[i], *b = i_dart_schema_find(pub, a->name);
        if (!b || a->kind != b->kind) return 0;
        if (a->kind == DART_ARR && (a->elem != b->elem || a->count != b->count)) return 0;
        if (a->kind == DART_STRUCT &&                         /* nested: exact type encoding */
            (a->type_len != b->type_len ||
             memcmp(sub->wire.data + a->type_off, pub->wire.data + b->type_off, a->type_len) != 0))
            return 0;
    }
    return 1;
}

DartSchema *dart_schema_rebase(const DartSchema *sub, const DartSchema *pub,
                               DartAllocFn alloc, void *user){
    DartSchema *r; uint16_t i;
    if (!alloc || !dart_schema_subset(sub, pub)) return NULL;
    r = dart_schema_parse(sub->wire.data, sub->wire.len, alloc, user);
    if (!r) return NULL;
    for (i = 0; i < r->nfields; i++)                          /* the writer's layout... */
        r->fields[i].offset = i_dart_schema_find(pub, r->fields[i].name)->offset;
    r->size = pub->size;                                      /* ...and the writer's message size */
    return r;
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

DartBytes dart_get_array(DartBytes msg, const DartSchema *s, uint16_t field){
    DartBytes out; const i_Field *f = i_dart_schema_field_lookup(s, msg, field);
    out.data = NULL; out.len = 0;
    if (!f || f->kind != DART_ARR) return out;
    out.data = msg.data + f->offset; out.len = f->size;
    return out;
}

/* ---- write a message ----------------------------------------------------------------- */
static const i_Field *i_dart_schema_set_lookup(const DartSchema *s, const void *buf, size_t cap,
                                               uint16_t field){
    if (!buf) return NULL;
    return i_dart_schema_field_lookup(s, dart_bytes(buf, cap), field);
}

int dart_set_uint(void *buf, size_t cap, const DartSchema *s, uint16_t field, uint64_t v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field); uint8_t *p;
    if (!f) return 0;
    p = (uint8_t *)buf + f->offset;
    switch (f->kind){
        case DART_U8:   p[0] = (uint8_t)v;  return 1;
        case DART_BOOL: p[0] = v ? 1u : 0u; return 1;
        case DART_U16: i_dart_le_w16(p, (uint16_t)v); return 1;
        case DART_U32: i_dart_le_w32(p, (uint32_t)v); return 1;
        case DART_U64: i_dart_le_w64(p, v); return 1;
        default: return 0;
    }
}

int dart_set_int(void *buf, size_t cap, const DartSchema *s, uint16_t field, int64_t v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field); uint8_t *p;
    if (!f) return 0;
    p = (uint8_t *)buf + f->offset;
    switch (f->kind){
        case DART_I8:  p[0] = (uint8_t)v; return 1;
        case DART_I16: i_dart_le_w16(p, (uint16_t)v); return 1;
        case DART_I32: i_dart_le_w32(p, (uint32_t)v); return 1;
        case DART_I64: i_dart_le_w64(p, (uint64_t)v); return 1;
        default: return 0;
    }
}

int dart_set_f64(void *buf, size_t cap, const DartSchema *s, uint16_t field, double v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field);
    if (!f) return 0;
    if (f->kind == DART_F64){ uint64_t b; memcpy(&b, &v, 8); i_dart_le_w64((uint8_t *)buf + f->offset, b); return 1; }
    if (f->kind == DART_F32){ float x = (float)v; uint32_t b; memcpy(&b, &x, 4); i_dart_le_w32((uint8_t *)buf + f->offset, b); return 1; }
    return 0;
}

int dart_set_f32(void *buf, size_t cap, const DartSchema *s, uint16_t field, float v){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field); uint32_t b;
    if (!f || f->kind != DART_F32) return 0;
    memcpy(&b, &v, 4); i_dart_le_w32((uint8_t *)buf + f->offset, b);
    return 1;
}

int dart_set_array(void *buf, size_t cap, const DartSchema *s, uint16_t field, DartBytes elems){
    const i_Field *f = i_dart_schema_set_lookup(s, buf, cap, field); uint32_t esz;
    if (!f || f->kind != DART_ARR) return 0;
    esz = dart_schema_scalar_size((DartSchemaTypeKind)f->elem);
    if (elems.len > f->size || (esz && elems.len % esz != 0)) return 0;  /* never silently truncate */
    if (elems.len && !elems.data) return 0;
    if (elems.len) memcpy((uint8_t *)buf + f->offset, elems.data, elems.len);
    memset((uint8_t *)buf + f->offset + elems.len, 0, f->size - elems.len);  /* zero the tail */
    return 1;
}

/* ---- the schema DSL ------------------------------------------------------------------ */
/* dart_schema_compile input (see schema.h for the full doc):
 *   schema := name '{' fields '}'
 *   field  := name ':' type  (',')?          fields self-delimit; commas optional
 *   type   := scalar | scalar '[' count ']' | '{' fields '}'
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
            if (!i_dart_dsl_ident(d, tname)) return;
            if (!i_dart_dsl_kind(tname, &k)){ i_dart_dsl_fail(d, at); return; }   /* unknown type */
            i_dart_dsl_ws(d);
            if (*d->p == '['){
                uint16_t count;
                d->p++;
                i_dart_dsl_ws(d);
                if (!i_dart_dsl_count(d, &count)) return;
                i_dart_dsl_ws(d);
                if (!i_dart_dsl_expect(d, ']')) return;
                dart_schema_field_array(b, name, k, count);
            } else {
                dart_schema_field(b, name, k);
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
