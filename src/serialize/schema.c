/* The wire format and the layout rules are in spec/schema.md. */
#include "internal.h"

uint32_t rant_schema_scalar_size(RantSchemaTypeKind kind){
    switch (kind){
        case RANT_U8: case RANT_I8: case RANT_BOOL: return 1;
        case RANT_U16: case RANT_I16:                   return 2;
        case RANT_U32: case RANT_I32: case RANT_F32: return 4;
        case RANT_U64: case RANT_I64: case RANT_F64: return 8;
        default: return 0;
    }
}

/* enum backing values: an integer kind, read and written like the scalar */
int i_rant_enum_backing_ok(uint8_t backing){ return backing <= (uint8_t)RANT_I64; }
/* widened to int64, sign extended for a signed kind */
int64_t i_rant_enum_read_val(uint8_t backing, const uint8_t *p){
    switch (backing){
        case RANT_U8:    return (int64_t)(uint64_t)p[0];
        case RANT_U16: return (int64_t)(uint64_t)i_rant_le_r16(p);
        case RANT_U32: return (int64_t)(uint64_t)i_rant_le_r32(p);
        case RANT_U64: return (int64_t)i_rant_le_r64(p);
        case RANT_I8:    return (int64_t)(int8_t)p[0];
        case RANT_I16: return (int64_t)(int16_t)i_rant_le_r16(p);
        case RANT_I32: return (int64_t)(int32_t)i_rant_le_r32(p);
        case RANT_I64: return (int64_t)i_rant_le_r64(p);
        default:       return 0;
    }
}
/* truncated to the backing */
void i_rant_enum_write_val(uint8_t backing, uint8_t *p, int64_t v){
    uint64_t u = (uint64_t)v; uint32_t bs = rant_schema_scalar_size((RantSchemaTypeKind)backing), i;
    for (i = 0; i < bs; i++) p[i] = (uint8_t)(u >> (8 * i));
}
/* a u64 above INT64_MAX is not expressible */
int i_rant_enum_val_fits(uint8_t backing, int64_t v){
    switch (backing){
        case RANT_U8:    return v >= 0 && v <= 0xFF;
        case RANT_U16: return v >= 0 && v <= 0xFFFF;
        case RANT_U32: return v >= 0 && v <= (int64_t)0xFFFFFFFF;
        case RANT_U64: return v >= 0;
        case RANT_I8:    return v >= -128 && v <= 127;
        case RANT_I16: return v >= -32768 && v <= 32767;
        case RANT_I32: return v >= (int64_t)(-2147483647 - 1) && v <= 2147483647;
        case RANT_I64: return 1;
        default:       return 0;
    }
}

/* The kind at r->pos with any NAMED wrappers peeled off, 0xFF if the wire runs out. */
static uint8_t i_rant_peek_kind(const i_Rd *r){
    size_t p = r->pos;
    for (;;){
        if (p >= r->n) return 0xFFu;
        if (r->w[p] != RANT_NAMED) return r->w[p];
        if (p + 2u > r->n) return 0xFFu;
        p += 2u + r->w[p + 1];
    }
}

/* Where a type sits relative to an array. DIRECT is an element type, so not itself an
 * array. INSIDE is anywhere within one, so no variable kinds and no further struct arrays. */
#define I_T_ELEM_DIRECT 1u
#define I_T_ELEM_INSIDE 2u

/* The fixed byte size of the type at r->pos, advancing past it. Counts every field walked
 * into *fields and every variable field into *nvar. Fails on any rule break. */
static uint32_t i_rant_rd_type_size(i_Rd *r, uint32_t *fields, uint32_t *nvar,
                                    uint16_t depth, uint32_t fl){
    uint8_t k = i_rant_rd_u8(r);
    if (r->fail) return 0;
    switch (k){
        case RANT_U8: case RANT_I8: case RANT_BOOL: return 1;
        case RANT_U16: case RANT_I16:                   return 2;
        case RANT_U32: case RANT_I32: case RANT_F32: return 4;
        case RANT_U64: case RANT_I64: case RANT_F64: return 8;
        case RANT_STR:
            return 2u + (uint32_t)i_rant_rd_u16(r);                /* [u16 len][cap bytes] */
        case RANT_ARR: {
            uint16_t count; uint8_t ek; uint64_t es;
            if (fl & I_T_ELEM_DIRECT){ r->fail = 1; return 0; }  /* no array of arrays */
            count = i_rant_rd_u16(r);
            if (r->fail) return 0;
            ek = i_rant_peek_kind(r);
            if (ek == 0xFFu){ r->fail = 1; return 0; }
            if ((fl & I_T_ELEM_INSIDE) && ek == RANT_STRUCT){ r->fail = 1; return 0; }
            es = i_rant_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                     I_T_ELEM_DIRECT | I_T_ELEM_INSIDE);
            if (r->fail || es == 0){ r->fail = 1; return 0; }    /* elements must be fixed */
            if ((uint64_t)count * es > 0xFFFFFFFFu){ r->fail = 1; return 0; }
            return (uint32_t)((uint64_t)count * es);
        }
        case RANT_ENUM: {
            uint8_t backing; uint16_t n; uint32_t bs, i;
            if (fl & I_T_ELEM_DIRECT){ r->fail = 1; return 0; }  /* the element kind is ambiguous */
            backing = i_rant_rd_u8(r);
            n = i_rant_rd_u16(r);
            bs = rant_schema_scalar_size((RantSchemaTypeKind)backing);
            if (r->fail || bs == 0 || !i_rant_enum_backing_ok(backing)){ r->fail = 1; return 0; }
            for (i = 0; i < n && !r->fail; i++){                  /* skip the option table */
                i_rant_rd_skip(r, bs);
                i_rant_rd_skip(r, i_rant_rd_u8(r));
            }
            if (r->fail) return 0;
            return bs;   /* on the wire: just the backing scalar */
        }
        case RANT_VSTR: case RANT_MAP:
            if (fl){ r->fail = 1; return 0; }                    /* never inside an array element */
            if (nvar) (*nvar)++;
            return 0;
        case RANT_VARR: {
            uint64_t es;
            if (fl){ r->fail = 1; return 0; }
            if (i_rant_peek_kind(r) == 0xFFu){ r->fail = 1; return 0; }
            es = i_rant_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                     I_T_ELEM_DIRECT | I_T_ELEM_INSIDE);
            if (r->fail || es == 0){ r->fail = 1; return 0; }
            if (nvar) (*nvar)++;
            return 0;
        }
        case RANT_STRUCT: {
            uint8_t nf; uint64_t sum = 0; uint16_t i;
            if (depth >= RANT_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
            nf = i_rant_rd_u8(r);
            for (i = 0; i < nf && !r->fail; i++){
                uint8_t fnl = i_rant_rd_u8(r);
                i_rant_rd_skip(r, fnl);
                if (fields) (*fields)++;
                sum += i_rant_rd_type_size(r, fields, nvar, (uint16_t)(depth + 1),
                                           fl & I_T_ELEM_INSIDE);
            }
            if (sum > 0xFFFFFFFFu){ r->fail = 1; return 0; }
            return (uint32_t)sum;
        }
        case RANT_NAMED: {
            uint8_t nl = i_rant_rd_u8(r);
            if (r->fail || nl == 0){ r->fail = 1; return 0; }    /* a name is required */
            i_rant_rd_skip(r, nl);
            if (r->fail) return 0;
            if (r->pos < r->n && r->w[r->pos] == RANT_NAMED){ r->fail = 1; return 0; }
            return i_rant_rd_type_size(r, fields, nvar, depth, fl);
        }
        default: r->fail = 1; return 0;                          /* unknown kind: reject */
    }
}

/* Offset of the compiled handle in buf: past the wire bytes, 8 aligned. */
static size_t i_rant_schema_handle_off(const uint8_t *buf, size_t wire_len){
    uintptr_t addr = (uintptr_t)buf + wire_len;
    size_t pad = (size_t)((8u - (addr & 7u)) & 7u);
    return wire_len + pad;
}

/* Counts every field of the root into *n and its variable fields into *nvar. 0 on
 * malformed wire. A bare or alias root is 1 plus whatever its type flattens to. */
int i_rant_schema_wire_fields(const void *wire, size_t wire_len,
                              uint32_t *n, uint32_t *nvar){
    i_Rd r; uint8_t ver, rl;
    *n = 0; *nvar = 0;
    r.w = (const uint8_t *)wire; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_rant_rd_u8(&r); if (r.fail || ver != RANT_SCHEMA_WIRE_VERSION) return 0;
    rl = i_rant_rd_u8(&r); i_rant_rd_skip(&r, rl);
    if (r.fail || (size_t)r.pos >= r.n) return 0;
    if (r.w[r.pos] == RANT_NAMED) return 0;            /* a root's name rides the header */
    if (r.w[r.pos] == RANT_STRUCT){
        i_rant_rd_type_size(&r, n, nvar, 0, 0);
    } else {
        uint32_t sub = 0;
        i_rant_rd_type_size(&r, &sub, nvar, 0, 0);     /* depth 0: a variable root is allowed */
        *n = 1u + sub;                                 /* the root is the first field */
    }
    return r.fail ? 0 : 1;
}

/* flattening a wire type into the field table */
static void i_rant_field_init(i_Field *f, RantString name, uint16_t depth, uint16_t parent,
                              uint32_t offset){
    memset(f, 0, sizeof *f);
    f->name = name;
    f->type_name = rant_string(NULL, 0);
    f->elem_name = rant_string(NULL, 0);
    f->depth = depth; f->parent = parent; f->arr_parent = I_RANT_NO_PARENT;
    f->offset = offset;
}

static uint32_t i_rant_emit_type(uint8_t *buf, size_t wl, i_Rd *r, RantSchema *s,
                                 uint16_t *emitted, uint32_t total,
                                 uint16_t idx, uint16_t depth, uint32_t base);

/* Emits the members of the struct body at r->pos into the table, depth first. Returns
 * the struct's fixed size and sets r->fail on malformed wire. */
static uint32_t i_rant_emit_struct(uint8_t *buf, size_t wl, i_Rd *r, RantSchema *s,
                                   uint16_t *emitted, uint32_t total,
                                   uint16_t depth, uint16_t parent, uint32_t base){
    uint8_t nf = i_rant_rd_u8(r); uint16_t i;
    uint32_t running = base;
    if (depth >= RANT_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
    for (i = 0; i < nf && !r->fail; i++){
        uint8_t fl = i_rant_rd_u8(r);
        const char *fn = (const char *)(buf + r->pos);
        uint16_t idx; uint32_t sz;
        i_rant_rd_skip(r, fl);
        if (r->fail) return 0;
        if (*emitted >= total){ r->fail = 1; return 0; }
        idx = (*emitted)++;
        i_rant_field_init(&s->fields[idx], rant_string(fn, fl), depth, parent, running);
        sz = i_rant_emit_type(buf, wl, r, s, emitted, total, idx, depth, running);
        if (r->fail) return 0;
        running += sz;
    }
    return running - base;
}

/* Fills field idx from the type at r->pos, NAMED wrappers unwrapped into type_name, and
 * emits any child fields it flattens to. base is where the field's storage starts. */
static uint32_t i_rant_emit_type(uint8_t *buf, size_t wl, i_Rd *r, RantSchema *s,
                                 uint16_t *emitted, uint32_t total,
                                 uint16_t idx, uint16_t depth, uint32_t base){
    size_t kpos = r->pos;
    uint8_t k;
    uint32_t sz = 0;
    i_Field *f = &s->fields[idx];
    f->type_off = (uint32_t)kpos;
    k = i_rant_rd_u8(r);
    if (k == RANT_NAMED){
        uint8_t nl = i_rant_rd_u8(r);
        const char *tn = (const char *)(buf + r->pos);
        i_rant_rd_skip(r, nl);
        if (r->fail || nl == 0){ r->fail = 1; return 0; }
        f->type_name = rant_string(tn, nl);
        k = i_rant_rd_u8(r);
        if (k == RANT_NAMED){ r->fail = 1; return 0; }
    }
    if (r->fail) return 0;
    f->kind = k;
    switch (k){
        case RANT_STRUCT:
            sz = i_rant_emit_struct(buf, wl, r, s, emitted, total,
                                    (uint16_t)(depth + 1), idx, base);
            break;
        case RANT_ARR: case RANT_VARR: {
            uint8_t ek; uint32_t esz = 0;
            if (k == RANT_ARR){
                f->count = i_rant_rd_u16(r);
                if (r->fail) return 0;
            }
            ek = i_rant_rd_u8(r);
            if (ek == RANT_NAMED){
                uint8_t nl = i_rant_rd_u8(r);
                const char *tn = (const char *)(buf + r->pos);
                i_rant_rd_skip(r, nl);
                if (r->fail || nl == 0){ r->fail = 1; return 0; }
                f->elem_name = rant_string(tn, nl);
                ek = i_rant_rd_u8(r);
                if (ek == RANT_NAMED){ r->fail = 1; return 0; }
            }
            if (r->fail) return 0;
            f->elem = ek;
            if (ek == RANT_STRUCT){                         /* one element 0 template */
                esz = i_rant_emit_struct(buf, wl, r, s, emitted, total,
                                         (uint16_t)(depth + 1), idx,
                                         k == RANT_ARR ? base : 0u);
            } else if (ek == RANT_STR){
                f->str_cap = i_rant_rd_u16(r);
                esz = 2u + (uint32_t)f->str_cap;
            } else {
                esz = rant_schema_scalar_size((RantSchemaTypeKind)ek);
            }
            if (r->fail || esz == 0){ r->fail = 1; return 0; }
            f->elem_size = esz;
            if (k == RANT_ARR){
                if ((uint64_t)f->count * esz > 0xFFFFFFFFu){ r->fail = 1; return 0; }
                sz = (uint32_t)((uint64_t)f->count * esz);
            }
            break;
        }
        case RANT_STR:
            f->str_cap = i_rant_rd_u16(r);
            sz = 2u + (uint32_t)f->str_cap;
            break;
        case RANT_ENUM: {
            uint16_t i;
            f->elem = i_rant_rd_u8(r);
            f->count = i_rant_rd_u16(r);
            sz = rant_schema_scalar_size((RantSchemaTypeKind)f->elem);
            if (r->fail || sz == 0 || !i_rant_enum_backing_ok(f->elem)){ r->fail = 1; return 0; }
            for (i = 0; i < f->count && !r->fail; i++){
                i_rant_rd_skip(r, sz);
                i_rant_rd_skip(r, i_rant_rd_u8(r));
            }
            break;
        }
        case RANT_VSTR: case RANT_MAP:
            sz = 0;
            break;
        default:
            sz = rant_schema_scalar_size((RantSchemaTypeKind)k);
            if (sz == 0){ r->fail = 1; return 0; }
            break;
    }
    if (r->fail) return 0;
    f = &s->fields[idx];                      /* unchanged, recursion never moves it */
    f->type_len = (uint32_t)(r->pos - kpos);
    f->size = sz;
    return sz;
}

/* Compiles the wire bytes at buf[0..wire_len] into a RantSchema placed after them in
 * buf. NULL on a malformed blob or when cap is too small. */
RantSchema *i_rant_schema_compile(uint8_t *buf, size_t wire_len, size_t cap){
    i_Rd r; uint8_t ver, root_kind, root_namelen;
    const char *root_name; size_t hoff, need; RantSchema *s;
    uint32_t total, nvar; uint16_t emitted = 0;

    if (!i_rant_schema_wire_fields(buf, wire_len, &total, &nvar) || total > 0xFFFFu) return NULL;

    r.w = buf; r.n = wire_len; r.pos = 0; r.fail = 0;
    ver = i_rant_rd_u8(&r);
    if (r.fail || ver != RANT_SCHEMA_WIRE_VERSION) return NULL;
    root_namelen = i_rant_rd_u8(&r);
    root_name = (const char *)(buf + r.pos);
    i_rant_rd_skip(&r, root_namelen);
    if (r.fail || r.pos >= r.n) return NULL;
    root_kind = buf[r.pos];

    hoff = i_rant_schema_handle_off(buf, wire_len);
    need = hoff + sizeof(RantSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
    if (need > cap) return NULL;

    s = (RantSchema *)(buf + hoff);
    s->wire = rant_bytes(buf, wire_len);
    s->name = rant_string(root_name, root_namelen);
    s->nfields = (uint16_t)total;
    s->n_var = (uint16_t)nvar;
    s->value_root = (uint8_t)(root_kind != RANT_STRUCT);
    s->hash = i_rant_fnv1a64(buf, wire_len);
    if (!s->value_root){
        r.pos++;                                 /* past the root's STRUCT kind byte */
        s->size = i_rant_emit_struct(buf, wire_len, &r, s, &emitted, total,
                                     0, I_RANT_NO_PARENT, 0);
    } else {                                     /* the bare or alias root is the first field */
        if (total == 0) return NULL;
        i_rant_field_init(&s->fields[0], rant_string((const char *)(buf + r.pos), 0),
                          0, I_RANT_NO_PARENT, 0);
        emitted = 1;
        s->size = i_rant_emit_type(buf, wire_len, &r, s, &emitted, total, 0, 0, 0);
    }
    if (r.fail || emitted != (uint16_t)total) return NULL;
    {   /* tail frame ordinals in depth first order, and the array ancestry */
        uint16_t i, ord = 0;
        for (i = 0; i < s->nfields; i++){
            i_Field *f = &s->fields[i];
            if (f->parent != I_RANT_NO_PARENT){
                const i_Field *p = &s->fields[f->parent];
                f->arr_parent = (p->kind == RANT_ARR || p->kind == RANT_VARR)
                              ? f->parent : p->arr_parent;
            }
            if (i_rant_kind_var(f->kind)){
                f->offset = 0;                   /* a frame has no static offset */
                f->var_ord = ord++;
            }
        }
    }
    return s;
}

/* bytes a compiled schema needs for wire: the copy, the alignment pad, the handle and
   the field table. 0 if the wire is malformed. */
static size_t i_rant_schema_compiled_size(const void *wire, size_t wire_len){
    uint32_t total, nvar;
    if (!i_rant_schema_wire_fields(wire, wire_len, &total, &nvar) || total > 0xFFFFu) return 0;
    return wire_len + 7u + sizeof(RantSchema) + (size_t)(total ? total - 1u : 0u) * sizeof(i_Field);
}

RantSchema *rant_schema_parse(const void *wire, size_t wire_len, RantAllocFn alloc, void *user){
    size_t need, i; uint8_t *buf; RantSchema *s;
    if (!wire || !alloc || wire_len == 0) return NULL;
    need = i_rant_schema_compiled_size(wire, wire_len);
    if (need == 0) return NULL;                              /* a malformed header */
    buf = (uint8_t *)alloc(user, NULL, need);
    if (!buf) return NULL;
    for (i = 0; i < wire_len; i++) buf[i] = ((const uint8_t *)wire)[i];   /* persist the bytes */
    s = i_rant_schema_compile(buf, wire_len, need);
    if (!s) alloc(user, buf, 0);                             /* a malformed body: no leak */
    return s;
}

RantSchema *rant_schema_copy(const RantSchema *s, RantAllocFn alloc, void *user){
    RantBytes w;
    if (!s || !alloc) return NULL;
    w = rant_schema_wire(s);
    return rant_schema_parse(w.data, w.len, alloc, user);
}

void rant_schema_free(RantSchema *s, RantAllocFn alloc, void *user){
    if (s && alloc) alloc(user, (void *)s->wire.data, 0);    /* wire.data is the block base */
}

/* queries */
RantBytes rant_schema_wire(const RantSchema *s){
    RantBytes b; if (s) return s->wire;
    b.data = NULL; b.len = 0; return b;
}
uint64_t     rant_schema_hash(const RantSchema *s){ return s ? s->hash : 0; }
RantString rant_schema_name(const RantSchema *s){
    RantString n; if (s) return s->name;
    n.data = NULL; n.len = 0; return n;
}
uint32_t rant_schema_size(const RantSchema *s){ return s ? s->size : 0; }
uint16_t rant_schema_field_count(const RantSchema *s){ return s ? s->nfields : 0; }

int rant_schema_field_at(const RantSchema *s, uint16_t i, RantSchemaFieldInfo *out){
    const i_Field *f;
    if (!s || i >= s->nfields) return 0;
    f = &s->fields[i];
    if (out){
        out->name = f->name;
        out->type_name = f->type_name; out->elem_name = f->elem_name;
        out->kind = f->kind; out->elem = f->elem;
        out->count = f->count; out->depth = f->depth;
        out->str_cap = f->str_cap; out->arr_parent = f->arr_parent;
        out->offset = f->offset; out->size = f->size;
        out->elem_size = f->elem_size;
    }
    return 1;
}

/* Matches a dotted path against a field: the last segment is its own name, the earlier
 * ones its ancestors. A segment may carry [N] on an array field, which *index gets. */
static int i_rant_schema_path_match(const RantSchema *s, const i_Field *f, const char *path,
                                    size_t path_len, uint32_t *index){
    const char *end = path + path_len;
    uint32_t found = 0;
    for (;;){
        const char *seg = end, *nend;
        while (seg > path && seg[-1] != '.') seg--;
        nend = end;
        if (nend - seg >= 3 && nend[-1] == ']'){          /* strip a trailing [N] */
            const char *br = nend - 1;
            while (br > seg && br[-1] != '[') br--;
            if (br > seg){
                const char *q = br; uint32_t idx = 0; int ok = 1;
                while (q < nend - 1){
                    if (*q < '0' || *q > '9'){ ok = 0; break; }
                    idx = idx * 10u + (uint32_t)(*q - '0');
                    q++;
                }
                if (ok && q > br){
                    if (f->kind != RANT_ARR && f->kind != RANT_VARR) return 0;
                    found = idx;
                    nend = br - 1;
                }
            }
        }
        if (f->name.len != (size_t)(nend - seg) ||
            (f->name.len && memcmp(f->name.data, seg, f->name.len) != 0)) return 0;
        if (f->parent == I_RANT_NO_PARENT){                  /* root: all segments consumed */
            if (seg != path) return 0;
            if (index) *index = found;
            return 1;
        }
        if (seg == path) return 0;                         /* segments ran out early */
        end = seg - 1;                                     /* past the dot */
        f = &s->fields[f->parent];
    }
}

const i_Field *i_rant_schema_field_by_path(const RantSchema *s, const char *path,
                                           uint32_t *index){
    uint16_t i; size_t n;
    if (index) *index = 0;
    if (!s || !path) return NULL;
    n = strlen(path);
    for (i = 0; i < s->nfields; i++)
        if (i_rant_schema_path_match(s, &s->fields[i], path, n, index)) return &s->fields[i];
    return NULL;
}

int rant_schema_field_index(const RantSchema *s, const char *path){
    const i_Field *f = i_rant_schema_field_by_path(s, path, NULL);
    return f ? (int)(f - s->fields) : -1;
}

RantBytes rant_schema_field_type_wire(const RantSchema *s, uint16_t field){
    const i_Field *f;
    if (!s || field >= s->nfields) return rant_bytes(NULL, 0);
    f = &s->fields[field];
    if ((size_t)f->type_off + f->type_len > s->wire.len) return rant_bytes(NULL, 0);
    return rant_bytes(s->wire.data + f->type_off, f->type_len);
}

uint16_t rant_schema_enum_count(const RantSchema *s, uint16_t field){
    if (!s || field >= s->nfields) return 0;
    return s->fields[field].kind == RANT_ENUM ? s->fields[field].count : 0;
}

int rant_schema_enum_variant(const RantSchema *s, uint16_t field, uint16_t i,
                             int64_t *value, RantString *name){
    const i_Field *f; const uint8_t *w; uint32_t pos, end; uint16_t k; uint8_t bs;
    if (!s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (f->kind != RANT_ENUM || i >= f->count) return 0;
    bs  = (uint8_t)rant_schema_scalar_size((RantSchemaTypeKind)f->elem);
    w   = s->wire.data;
    /* past an optional NAMED tag, then [ENUM][backing][u16 n], to the first option */
    pos = f->type_off;
    if (w[pos] == RANT_NAMED) pos += 2u + w[pos + 1];
    pos += 4u;
    end = f->type_off + f->type_len;
    for (k = 0; k < i; k++){                 /* hop over earlier options: [value][u8 nl][name] */
        if ((size_t)pos + bs + 1u > end) return 0;
        pos += bs + 1u + w[pos + bs];
    }
    if ((size_t)pos + bs + 1u > end) return 0;
    { uint8_t nl = w[pos + bs];
      if ((size_t)pos + bs + 1u + nl > end) return 0;
      if (value) *value = i_rant_enum_read_val(f->elem, w + pos);
      if (name)  *name  = rant_string((const char *)(w + pos + bs + 1u), nl); }
    return 1;
}

RantString rant_enum_name_of(const RantSchema *s, uint16_t field, int64_t value){
    uint16_t i, n = rant_schema_enum_count(s, field);
    for (i = 0; i < n; i++){
        int64_t v; RantString nm;
        if (rant_schema_enum_variant(s, field, i, &v, &nm) && v == value) return nm;
    }
    return rant_string(NULL, 0);
}

int rant_enum_value_of(const RantSchema *s, uint16_t field, const char *name, int64_t *out){
    uint16_t i, n = rant_schema_enum_count(s, field);
    size_t want = 0;
    if (name) while (name[want]) want++;
    for (i = 0; i < n; i++){
        int64_t v; RantString nm;
        if (rant_schema_enum_variant(s, field, i, &v, &nm) && nm.len == want &&
            (want == 0 || memcmp(nm.data, name, want) == 0)){ if (out) *out = v; return 1; }
    }
    return 0;
}
