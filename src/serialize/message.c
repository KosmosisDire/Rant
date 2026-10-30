/* Message validate, get and set over a compiled schema. */
#include "internal.h"

uint32_t rant_schema_msg_min(const RantSchema *s){ return s ? s->msg_min : 0; }

uint32_t rant_schema_msg_len(const RantSchema *s, const void *buf, size_t cap){
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t total; uint16_t i;
    if (!s || !p || cap < s->size) return 0;
    total = s->size;
    for (i = 0; i < s->n_var; i++){                     /* hop the frames, bounds checked */
        if (total + 4u > cap) return 0;
        total += 4u + (uint64_t)i_rant_le_r32(p + total);
        if (total > cap) return 0;
    }
    return total <= 0xFFFFFFFFu ? (uint32_t)total : 0;
}

/* Locating a value. A record is the message, rec -1, or one variable struct element, rec
 * its element row. A span is a fixed row's bytes, a variable row's payload or an element. */
typedef struct { size_t off, len; } i_Span;

/* The lengths that carry a value, outermost first: a frame's length word when count is 0,
 * else the ends from..count-1 of the table at at. A resize adds its change to each. */
typedef struct { size_t at; uint32_t from, count; } i_Carry;
typedef struct { i_Carry c[2u * RANT_SCHEMA_MAX_DEPTH + 2u]; uint16_t n; } i_Carriers;

static int i_rant_carry(i_Carriers *cr, size_t at, uint32_t from, uint32_t count){
    if (!cr) return 1;
    if (cr->n >= sizeof cr->c / sizeof cr->c[0]) return 0;
    cr->c[cr->n].at = at; cr->c[cr->n].from = from; cr->c[cr->n].count = count;
    cr->n++;
    return 1;
}

/* frame ord of record rec laid out over rg */
static int i_rant_record_frame(const RantSchema *s, RantBytes msg, i_Span rg, int rec,
                               uint16_t ord, i_Span *out){
    uint64_t pos = (uint64_t)rg.off + (rec < 0 ? s->size : s->fields[rec].size);
    uint64_t end = (uint64_t)rg.off + rg.len;
    uint16_t nv = rec < 0 ? s->n_var : s->fields[rec].n_var, i;
    if (ord >= nv) return 0;
    for (i = 0; ; i++){
        uint32_t flen;
        if (pos + 4u > end) return 0;
        flen = i_rant_le_r32(msg.data + pos);
        if (pos + 4u + flen > end) return 0;
        if (i == ord){ out->off = (size_t)pos + 4u; out->len = flen; return 1; }
        pos += 4u + (uint64_t)flen;
    }
}

/* Row f inside the region rg of record rec: the region itself when f is that element, its
 * frame when f is framed, else its fixed bytes. */
static int i_rant_locate(const RantSchema *s, RantBytes msg, i_Span rg, int rec,
                         const i_Field *f, i_Span *out, i_Carriers *cr){
    if (rec >= 0 && f == &s->fields[rec]){
        if (rg.len < f->size) return 0;            /* a record's body holds its fixed part */
        *out = rg;
        return 1;
    }
    if (i_rant_row_framed(s, f))
        return i_rant_record_frame(s, msg, rg, rec, f->var_ord, out)
            && i_rant_carry(cr, out->off - 4u, 0, 0);
    if ((uint64_t)f->offset + f->size > rg.len) return 0;
    out->off = rg.off + f->offset; out->len = f->size;
    return 1;
}

/* The live count of array row a over its span. A variable element array opens with
 * [u32 count][u32 end]*count, each end counted from where the bodies start. */
static int i_rant_array_live(RantBytes msg, const i_Field *a, i_Span sp, uint32_t *count){
    const i_Field *e = a + 1;
    if (!e->var){
        *count = a->kind == RANT_ARR ? a->count : (uint32_t)(sp.len / e->size);
        return 1;
    }
    if (sp.len < 4u) return 0;
    *count = i_rant_le_r32(msg.data + sp.off);
    if (a->kind == RANT_ARR && *count != a->count) return 0;
    return 4u + 4u * (uint64_t)*count <= sp.len;
}

/* element i of array row a over its span */
static int i_rant_array_elem(RantBytes msg, const i_Field *a, i_Span sp, uint32_t i,
                             i_Span *out, i_Carriers *cr){
    const i_Field *e = a + 1; uint32_t count, lo, hi; size_t tab;
    if (!i_rant_array_live(msg, a, sp, &count) || i >= count) return 0;
    if (!e->var){
        out->off = sp.off + (size_t)i * e->size; out->len = e->size;
        return 1;
    }
    tab = sp.off + 4u;
    lo = i ? i_rant_le_r32(msg.data + tab + 4u * (size_t)(i - 1u)) : 0u;
    hi = i_rant_le_r32(msg.data + tab + 4u * (size_t)i);
    if (lo > hi || (uint64_t)hi > sp.len - 4u - 4u * (uint64_t)count) return 0;
    out->off = tab + 4u * (size_t)count + lo; out->len = hi - lo;
    return i_rant_carry(cr, sp.off, i, count);
}

/* Where row f sits, given one index per enclosing array, outermost first. cr, when not
 * NULL, gets every length that carries it. */
static int i_rant_resolve(const RantSchema *s, RantBytes msg, const i_Field *f,
                          const uint32_t *elems, uint16_t n_elems, i_Span *out, i_Carriers *cr){
    uint16_t chain[RANT_SCHEMA_MAX_DEPTH + 1u], k = 0, a;
    i_Span rg; int rec = -1;
    if (cr) cr->n = 0;
    if (n_elems != f->arr_depth || (n_elems && !elems) || !msg.data) return 0;
    for (a = f->arr_parent; a != I_RANT_NO_PARENT; a = s->fields[a].arr_parent){
        if (k > RANT_SCHEMA_MAX_DEPTH) return 0;
        chain[k++] = a;
    }
    rg.off = 0; rg.len = msg.len;
    while (k--){
        const i_Field *arr = &s->fields[chain[k]]; i_Span sp;
        if (!i_rant_locate(s, msg, rg, rec, arr, &sp, cr)
            || !i_rant_array_elem(msg, arr, sp, elems[n_elems - 1u - k], &rg, cr)) return 0;
        rec = (int)chain[k] + 1;
    }
    return i_rant_locate(s, msg, rg, rec, f, out, cr);
}

/* Resolves a possibly indexed path against a message. NULL if unknown, out of range or
 * the message is too short. */
static const i_Field *i_rant_schema_read_lookup(const RantSchema *s, RantBytes msg,
                                                const char *field, i_Span *sp, i_Carriers *cr){
    uint32_t elems[RANT_SCHEMA_MAX_DEPTH]; uint16_t n = 0;
    const i_Field *f = i_rant_schema_field_by_path(s, field, elems, &n);
    if (!f || !i_rant_resolve(s, msg, f, elems, n, sp, cr)) return NULL;
    return f;
}

/* validating a message */
static int i_rant_valid_record(const RantSchema *s, RantBytes msg, int rec, i_Span rg);

/* a variable row's payload: an element table with each body valid, or whole elements */
static int i_rant_valid_payload(const RantSchema *s, RantBytes msg, const i_Field *f, i_Span sp){
    const i_Field *e = f + 1; uint32_t count, k, prev = 0; size_t tab, bodies;
    if (f->kind != RANT_ARR && f->kind != RANT_VARR) return 1;   /* string and map bytes */
    if (!e->var) return sp.len % e->size == 0;
    if (!i_rant_array_live(msg, f, sp, &count)) return 0;
    tab = sp.off + 4u;
    bodies = tab + 4u * (size_t)count;
    for (k = 0; k < count; k++){
        uint32_t end = i_rant_le_r32(msg.data + tab + 4u * (size_t)k);
        i_Span body;
        if (end < prev) return 0;
        body.off = bodies + prev; body.len = end - prev;
        if ((uint64_t)body.off + body.len > (uint64_t)sp.off + sp.len) return 0;
        if (e->kind == RANT_STRUCT ? !i_rant_valid_record(s, msg, (int)(e - s->fields), body)
                                   : !i_rant_valid_payload(s, msg, e, body)) return 0;
        prev = end;
    }
    return bodies + prev == sp.off + sp.len;               /* the bodies fill the payload */
}

/* The row of record rec, whose rows run first to last, that holds frame ord, NULL when the
 * schema does not declare it. *at is a cursor, since a writer's rows hold frames in order. */
static const i_Field *i_rant_record_row(const RantSchema *s, uint16_t first, uint16_t last,
                                        uint16_t *at, uint16_t ord){
    uint16_t k = *at; int pass;
    for (pass = 0; pass < 2; pass++, k = first){
        while (k < last){
            const i_Field *f = &s->fields[k];
            if (i_rant_row_framed(s, f)){
                k = f->next;
                if (f->var_ord == ord){ *at = k; return f; }
            } else k = f->kind == RANT_ARR ? f->next : (uint16_t)(k + 1u);
        }
    }
    return NULL;
}

/* a record's fixed part and frames consume rg exactly, and every frame it declares holds
 * a valid payload */
static int i_rant_valid_record(const RantSchema *s, RantBytes msg, int rec, i_Span rg){
    uint64_t pos = (uint64_t)rg.off + (rec < 0 ? s->size : s->fields[rec].size);
    uint64_t end = (uint64_t)rg.off + rg.len;
    uint16_t nv = rec < 0 ? s->n_var : s->fields[rec].n_var, ord;
    uint16_t first = rec < 0 ? 0u : (uint16_t)(rec + 1), at = first;
    uint16_t last = rec < 0 ? s->nfields : s->fields[rec].next;
    if (pos > end) return 0;
    for (ord = 0; ord < nv; ord++){
        const i_Field *f; i_Span sp; uint32_t flen;
        if (pos + 4u > end) return 0;
        flen = i_rant_le_r32(msg.data + pos);
        if (pos + 4u + flen > end) return 0;
        sp.off = (size_t)pos + 4u; sp.len = flen;
        f = i_rant_record_row(s, first, last, &at, ord);
        if (f && !i_rant_valid_payload(s, msg, f, sp)) return 0;
        pos += 4u + (uint64_t)flen;
    }
    return pos == end;
}

int rant_schema_validate(const RantSchema *s, RantBytes msg){
    i_Span all;
    if (!s) return 0;
    if (s->n_var == 0) return msg.len == s->size;
    if (!msg.data) return 0;
    all.off = 0; all.len = msg.len;
    return i_rant_valid_record(s, msg, -1, all);
}

uint64_t i_rant_schema_read_uint(uint8_t kind, const uint8_t *p){
    switch (kind){
        case RANT_U8: case RANT_BOOL: return p[0];
        case RANT_U16: return i_rant_le_r16(p);
        case RANT_U32: return i_rant_le_r32(p);
        case RANT_U64: return i_rant_le_r64(p);
        default: return 0;
    }
}
int64_t i_rant_schema_read_int(uint8_t kind, const uint8_t *p){
    switch (kind){
        case RANT_I8:    return (int8_t)p[0];
        case RANT_I16: return (int16_t)i_rant_le_r16(p);
        case RANT_I32: return (int32_t)i_rant_le_r32(p);
        case RANT_I64: return (int64_t)i_rant_le_r64(p);
        default: return 0;
    }
}
double i_rant_schema_read_f64(uint8_t kind, const uint8_t *p){
    if (kind == RANT_F64){ uint64_t b = i_rant_le_r64(p); double d; memcpy(&d, &b, 8); return d; }
    if (kind == RANT_F32){ uint32_t b = i_rant_le_r32(p); float      x; memcpy(&x, &b, 4); return (double)x; }
    return 0.0;
}


uint64_t rant_get_uint(RantBytes msg, const RantSchema *s, const char *field){
    i_Span sp; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &sp, NULL);
    if (!f) return 0;
    if (f->kind == RANT_ENUM) return (uint64_t)i_rant_enum_read_val(f->backing, msg.data + sp.off);
    return i_rant_schema_read_uint(f->kind, msg.data + sp.off);
}

int64_t rant_get_int(RantBytes msg, const RantSchema *s, const char *field){
    i_Span sp; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &sp, NULL);
    if (!f) return 0;
    if (f->kind == RANT_ENUM) return i_rant_enum_read_val(f->backing, msg.data + sp.off);
    return i_rant_schema_read_int(f->kind, msg.data + sp.off);
}

double rant_get_f64(RantBytes msg, const RantSchema *s, const char *field){
    i_Span sp; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &sp, NULL);
    return f ? i_rant_schema_read_f64(f->kind, msg.data + sp.off) : 0.0;
}

float rant_get_f32(RantBytes msg, const RantSchema *s, const char *field){
    i_Span sp; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &sp, NULL);
    if (!f || f->kind != RANT_F32) return 0.0f;
    { uint32_t b = i_rant_le_r32(msg.data + sp.off); float x; memcpy(&x, &b, 4); return x; }
}

/* an array's packed live elements when its element is fixed, else {NULL,0} */
static RantBytes i_rant_array_packed(RantBytes msg, const i_Field *f, i_Span sp){
    const i_Field *e = f + 1;
    if ((f->kind != RANT_ARR && f->kind != RANT_VARR) || e->var) return rant_bytes(NULL, 0);
    return rant_bytes(msg.data + sp.off, sp.len - sp.len % e->size);   /* whole elements only */
}

RantBytes rant_get_array(RantBytes msg, const RantSchema *s, const char *field){
    i_Span sp; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &sp, NULL);
    return f ? i_rant_array_packed(msg, f, sp) : rant_bytes(NULL, 0);
}

uint32_t rant_get_array_count(RantBytes msg, const RantSchema *s, const char *field){
    i_Span sp; uint32_t n;
    const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &sp, NULL);
    if (!f || (f->kind != RANT_ARR && f->kind != RANT_VARR)) return 0;
    return i_rant_array_live(msg, f, sp, &n) ? n : 0;
}

/* one slot's live string, the length clamped to the cap against a hostile message */
static RantString i_rant_schema_str_view(const uint8_t *slot, uint16_t cap){
    uint16_t len = i_rant_le_r16(slot);
    if (len > cap) len = cap;
    return rant_string((const char *)(slot + 2), len);
}

RantString rant_get_string(RantBytes msg, const RantSchema *s, const char *field){
    i_Span sp; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &sp, NULL);
    if (!f) return rant_string(NULL, 0);
    if (f->kind == RANT_VSTR) return rant_string((const char *)(msg.data + sp.off), sp.len);
    if (f->kind != RANT_STR) return rant_string(NULL, 0);
    return i_rant_schema_str_view(msg.data + sp.off, f->str_cap);
}

RantBytes rant_get_map(RantBytes msg, const RantSchema *s, const char *field){
    i_Span sp; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &sp, NULL);
    if (!f || f->kind != RANT_MAP) return rant_bytes(NULL, 0);
    return rant_bytes(msg.data + sp.off, sp.len);
}

RantString rant_get_enum(RantBytes msg, const RantSchema *s, const char *field){
    i_Span sp; const i_Field *f = i_rant_schema_read_lookup(s, msg, field, &sp, NULL);
    if (!f || f->kind != RANT_ENUM) return rant_string(NULL, 0);
    return rant_enum_name_of(s, (uint16_t)(f - s->fields),
                             i_rant_enum_read_val(f->backing, msg.data + sp.off));
}

int rant_get_value_at(RantBytes msg, const RantSchema *s, uint16_t field,
                      const uint32_t *elems, uint16_t n_elems, RantValue *out){
    const i_Field *f; const uint8_t *p; i_Span sp;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (!i_rant_resolve(s, msg, f, elems, n_elems, &sp, NULL)) return 0;
    p = msg.data + sp.off;
    out->kind = f->kind;
    switch (f->kind){
        case RANT_U8: case RANT_U16: case RANT_U32: case RANT_U64: case RANT_BOOL:
            out->v.u = i_rant_schema_read_uint(f->kind, p); break;
        case RANT_I8: case RANT_I16: case RANT_I32: case RANT_I64:
            out->v.i = i_rant_schema_read_int(f->kind, p); break;
        case RANT_F32: case RANT_F64:
            out->v.f = i_rant_schema_read_f64(f->kind, p); break;
        case RANT_STRUCT:
            out->bytes = rant_bytes(p, f->size); break;
        case RANT_STR: {
            RantString sv = i_rant_schema_str_view(p, f->str_cap);
            out->bytes = rant_bytes(sv.data, sv.len); break;
        }
        case RANT_VSTR:
            out->bytes = rant_bytes(p, sp.len); break;
        case RANT_ARR: case RANT_VARR:
            if (!i_rant_array_live(msg, f, sp, &out->count)) return 0;
            out->bytes = i_rant_array_packed(msg, f, sp);
            break;
        case RANT_MAP:
            out->bytes = rant_bytes(p, sp.len);
            out->count = rant_map_count(out->bytes); break;
        case RANT_ENUM:
            out->v.i = i_rant_enum_read_val(f->backing, p); break;
        default: return 0;
    }
    return 1;
}

int rant_get_value(RantBytes msg, const RantSchema *s, uint16_t field, RantValue *out){
    return rant_get_value_at(msg, s, field, NULL, 0, out);
}

/* writing a message */
static uint32_t i_rant_default_record(const RantSchema *s, int rec, uint8_t *p);

/* the default payload of variable row f at p: empty, or an array's element table and
 * default bodies. Returns its length, f->dmin. */
static uint32_t i_rant_default_payload(const RantSchema *s, const i_Field *f, uint8_t *p){
    const i_Field *e = f + 1; uint32_t n, k, at;
    if ((f->kind != RANT_ARR && f->kind != RANT_VARR) || !e->var) return 0;
    n = f->kind == RANT_ARR ? f->count : 0u;
    i_rant_le_w32(p, n);
    for (k = 0; k < n; k++) i_rant_le_w32(p + 4u + 4u * (size_t)k, (k + 1u) * e->dmin);
    at = 4u + 4u * n;
    for (k = 0; k < n; k++)
        at += e->kind == RANT_STRUCT ? i_rant_default_record(s, (int)(e - s->fields), p + at)
                                     : i_rant_default_payload(s, e, p + at);
    return at;
}

/* the default record at p: zeros, then one default frame per framed row it holds */
static uint32_t i_rant_default_record(const RantSchema *s, int rec, uint8_t *p){
    uint32_t at = rec < 0 ? s->size : s->fields[rec].size;
    uint16_t j = rec < 0 ? 0u : (uint16_t)(rec + 1);
    uint16_t last = rec < 0 ? s->nfields : s->fields[rec].next;
    if (at) memset(p, 0, at);
    while (j < last){
        const i_Field *f = &s->fields[j];
        if (i_rant_row_framed(s, f)){
            i_rant_le_w32(p + at, f->dmin);
            at += 4u + i_rant_default_payload(s, f, p + at + 4u);
            j = f->next;
        } else if (f->kind == RANT_ARR){
            j = f->next;
        } else j++;
    }
    return at;
}

int rant_schema_message_default(const RantSchema *s, void *buf, size_t cap){
    if (!s) return 0;
    if (cap < s->msg_min || (!buf && s->msg_min)) return 0;   /* an empty message needs no buffer */
    if (s->msg_min) i_rant_default_record(s, -1, (uint8_t *)buf);
    return 1;
}

static const i_Field *i_rant_schema_set_lookup(const RantSchema *s, const void *buf, size_t cap,
                                               const char *field, i_Span *sp, i_Carriers *cr){
    if (!buf) return NULL;
    return i_rant_schema_read_lookup(s, rant_bytes(buf, cap), field, sp, cr);
}

static int i_rant_schema_write_uint(const i_Field *f, uint8_t *p, uint64_t v){
    switch (f->kind){
        case RANT_U8:     p[0] = (uint8_t)v;  return 1;
        case RANT_BOOL: p[0] = v ? 1u : 0u; return 1;
        case RANT_U16: i_rant_le_w16(p, (uint16_t)v); return 1;
        case RANT_U32: i_rant_le_w32(p, (uint32_t)v); return 1;
        case RANT_U64: i_rant_le_w64(p, v); return 1;
        default: return 0;
    }
}
static int i_rant_schema_write_int(const i_Field *f, uint8_t *p, int64_t v){
    switch (f->kind){
        case RANT_I8:    p[0] = (uint8_t)v; return 1;
        case RANT_I16: i_rant_le_w16(p, (uint16_t)v); return 1;
        case RANT_I32: i_rant_le_w32(p, (uint32_t)v); return 1;
        case RANT_I64: i_rant_le_w64(p, (uint64_t)v); return 1;
        default: return 0;
    }
}
static int i_rant_schema_write_f64(const i_Field *f, uint8_t *p, double v){
    if (f->kind == RANT_F64){ uint64_t b; memcpy(&b, &v, 8); i_rant_le_w64(p, b); return 1; }
    if (f->kind == RANT_F32){ float x = (float)v; uint32_t b; memcpy(&b, &x, 4); i_rant_le_w32(p, b); return 1; }
    return 0;
}
/* one string slot: [u16 len][bytes][zeroed tail]. Refuses v.len over the cap */
static int i_rant_schema_write_str_slot(uint8_t *p, uint16_t cap, RantString v){
    if (v.len > cap || (v.len && !v.data)) return 0;             /* never silently truncate */
    i_rant_le_w16(p, (uint16_t)v.len);
    if (v.len) memcpy(p + 2, v.data, v.len);
    memset(p + 2 + v.len, 0, (size_t)cap - v.len);
    return 1;
}
/* element payload sanity shared by fixed and variable arrays: whole fixed elements, and
 * every string slot's length prefix within its cap */
static int i_rant_schema_check_elems(const i_Field *f, RantBytes elems){
    const i_Field *e = f + 1;
    if (e->var || !e->size || elems.len % e->size != 0) return 0;   /* never silently truncate */
    if (elems.len && !elems.data) return 0;
    if (e->kind == RANT_STR){
        size_t off;
        for (off = 0; off + e->size <= elems.len; off += e->size)
            if (i_rant_le_r16(elems.data + off) > e->str_cap) return 0;
    }
    return 1;
}
/* copy elems over the front of a fixed array and zero the rest */
static int i_rant_schema_write_array(const i_Field *f, uint8_t *p, RantBytes elems){
    if (f->kind != RANT_ARR || f->var) return 0;
    if (elems.len > f->size || !i_rant_schema_check_elems(f, elems)) return 0;
    if (elems.len) memcpy(p, elems.data, elems.len);
    memset(p + elems.len, 0, f->size - elems.len);
    return 1;
}
/* Whether the value at sp may become new_len bytes: the message is whole, holds sp, and
 * stays within cap. *total is its live length. */
static int i_rant_resize_fits(const RantSchema *s, const uint8_t *buf, size_t cap, i_Span sp,
                              size_t new_len, uint64_t *total){
    uint64_t after;
    *total = rant_schema_msg_len(s, buf, cap);
    if (*total == 0 || (uint64_t)sp.off + sp.len > *total) return 0;   /* malformed or uninitialized */
    after = *total - sp.len + new_len;
    return after <= cap && after <= 0xFFFFFFFFu;                       /* never silently truncate */
}

/* Resizes the value at sp to new_len, moving the rest of the message, and adds the change
 * to every length that carries it. src NULL keeps the prefix and zero fills any growth. */
static int i_rant_resize(const RantSchema *s, uint8_t *buf, size_t cap, const i_Carriers *cr,
                         i_Span sp, const void *src, size_t new_len){
    uint64_t total; uint16_t i; uint32_t k;
    if (!i_rant_resize_fits(s, buf, cap, sp, new_len, &total)) return 0;
    memmove(buf + sp.off + new_len, buf + sp.off + sp.len, (size_t)(total - sp.off - sp.len));
    for (i = 0; i < cr->n; i++){              /* every carrier sits before sp, so none moved */
        const i_Carry *c = &cr->c[i];
        for (k = c->from; k < (c->count ? c->count : 1u); k++){
            uint8_t *p = buf + c->at + (c->count ? 4u + 4u * (size_t)k : 0u);
            i_rant_le_w32(p, (uint32_t)(i_rant_le_r32(p) - sp.len + new_len));
        }
    }
    if (src){ if (new_len) memcpy(buf + sp.off, src, new_len); }
    else if (new_len > sp.len) memset(buf + sp.off + sp.len, 0, new_len - sp.len);
    return 1;
}

/* Sets variable array a's live count over its payload at sp: whole fixed elements, or the
 * end table with the kept bodies and new default ones. */
static int i_rant_set_count(const RantSchema *s, uint8_t *buf, size_t cap, const i_Carriers *cr,
                            const i_Field *a, i_Span sp, uint32_t count){
    const i_Field *e = a + 1;
    uint8_t *p = buf + sp.off;
    uint32_t n, k, kept;
    uint64_t len, total;
    if (a->kind != RANT_VARR) return 0;
    if (!e->var){
        if ((uint64_t)count * e->size > 0xFFFFFFFFu) return 0;
        return i_rant_resize(s, buf, cap, cr, sp, NULL, (size_t)count * e->size);
    }
    if (!i_rant_array_live(rant_bytes(buf, cap), a, sp, &n)) return 0;
    kept = count < n ? count : n;
    kept = kept ? i_rant_le_r32(p + 4u * (size_t)kept) : 0u;     /* the bodies that stay */
    if ((uint64_t)kept > sp.len - 4u - 4u * (uint64_t)n) return 0;
    len = 4u + 4u * (uint64_t)count + kept + (count > n ? (uint64_t)(count - n) * e->dmin : 0u);
    if (len > 0xFFFFFFFFu || !i_rant_resize_fits(s, buf, cap, sp, (size_t)len, &total)) return 0;
    if (count < n){                           /* the kept bodies slide over the dropped ends */
        memmove(p + 4u + 4u * (size_t)count, p + 4u + 4u * (size_t)n, kept);
        i_rant_le_w32(p, count);
        return i_rant_resize(s, buf, cap, cr, sp, NULL, (size_t)len);
    }
    if (!i_rant_resize(s, buf, cap, cr, sp, NULL, (size_t)len)) return 0;
    memmove(p + 4u + 4u * (size_t)count, p + 4u + 4u * (size_t)n, kept);
    i_rant_le_w32(p, count);
    for (k = n; k < count; k++){
        uint8_t *body = p + 4u + 4u * (size_t)count + kept + (size_t)(k - n) * e->dmin;
        i_rant_le_w32(p + 4u + 4u * (size_t)k, kept + (k - n + 1u) * e->dmin);
        if (e->kind == RANT_STRUCT) i_rant_default_record(s, (int)(e - s->fields), body);
        else i_rant_default_payload(s, e, body);
    }
    return 1;
}

int rant_set_uint(void *buf, size_t cap, const RantSchema *s, const char *field, uint64_t v){
    i_Span sp; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &sp, NULL);
    if (!f) return 0;
    if (f->kind == RANT_ENUM){ i_rant_enum_write_val(f->backing, (uint8_t *)buf + sp.off, (int64_t)v); return 1; }
    return i_rant_schema_write_uint(f, (uint8_t *)buf + sp.off, v);
}

int rant_set_int(void *buf, size_t cap, const RantSchema *s, const char *field, int64_t v){
    i_Span sp; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &sp, NULL);
    if (!f) return 0;
    if (f->kind == RANT_ENUM){ i_rant_enum_write_val(f->backing, (uint8_t *)buf + sp.off, v); return 1; }
    return i_rant_schema_write_int(f, (uint8_t *)buf + sp.off, v);
}

int rant_set_f64(void *buf, size_t cap, const RantSchema *s, const char *field, double v){
    i_Span sp; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &sp, NULL);
    return f ? i_rant_schema_write_f64(f, (uint8_t *)buf + sp.off, v) : 0;
}

int rant_set_f32(void *buf, size_t cap, const RantSchema *s, const char *field, float v){
    i_Span sp; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &sp, NULL); uint32_t b;
    if (!f || f->kind != RANT_F32) return 0;
    memcpy(&b, &v, 4); i_rant_le_w32((uint8_t *)buf + sp.off, b);
    return 1;
}

int rant_set_array(void *buf, size_t cap, const RantSchema *s, const char *field, RantBytes elems){
    i_Span sp; i_Carriers cr;
    const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &sp, &cr);
    if (!f) return 0;
    if (f->kind == RANT_VARR)
        return i_rant_schema_check_elems(f, elems)
            && i_rant_resize(s, (uint8_t *)buf, cap, &cr, sp, elems.data, elems.len);
    return i_rant_schema_write_array(f, (uint8_t *)buf + sp.off, elems);
}

int rant_set_array_count(void *buf, size_t cap, const RantSchema *s, const char *field,
                         uint32_t count){
    i_Span sp; i_Carriers cr;
    const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &sp, &cr);
    if (!f || f->kind != RANT_VARR) return 0;
    return i_rant_set_count(s, (uint8_t *)buf, cap, &cr, f, sp, count);
}

int rant_set_string(void *buf, size_t cap, const RantSchema *s, const char *field, RantString v){
    i_Span sp; i_Carriers cr;
    const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &sp, &cr);
    if (!f) return 0;
    if (f->kind == RANT_VSTR){
        if (v.len && !v.data) return 0;
        return i_rant_resize(s, (uint8_t *)buf, cap, &cr, sp, v.data, v.len);
    }
    if (f->kind != RANT_STR) return 0;
    return i_rant_schema_write_str_slot((uint8_t *)buf + sp.off, f->str_cap, v);
}

int rant_set_map(void *buf, size_t cap, const RantSchema *s, const char *field, RantBytes map){
    i_Span sp; i_Carriers cr;
    const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &sp, &cr);
    if (!f || f->kind != RANT_MAP) return 0;
    if (!rant_map_valid(map)) return 0;               /* malformed bytes never enter a message */
    return i_rant_resize(s, (uint8_t *)buf, cap, &cr, sp, map.data, map.len);
}

int rant_set_enum(void *buf, size_t cap, const RantSchema *s, const char *field, const char *name){
    i_Span sp; const i_Field *f = i_rant_schema_set_lookup(s, buf, cap, field, &sp, NULL);
    int64_t v;
    if (!f || f->kind != RANT_ENUM) return 0;
    if (!rant_enum_value_of(s, (uint16_t)(f - s->fields), name, &v)) return 0;
    i_rant_enum_write_val(f->backing, (uint8_t *)buf + sp.off, v);
    return 1;
}

int rant_set_value_at(void *buf, size_t cap, const RantSchema *s, uint16_t field,
                      const uint32_t *elems, uint16_t n_elems, const RantValue *val){
    const i_Field *f; uint8_t *p; i_Span sp; i_Carriers cr;
    if (!buf || !val || !s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (!i_rant_resolve(s, rant_bytes(buf, cap), f, elems, n_elems, &sp, &cr)) return 0;
    p = (uint8_t *)buf + sp.off;
    switch (f->kind){
        case RANT_U8: case RANT_U16: case RANT_U32: case RANT_U64: case RANT_BOOL:
            return i_rant_schema_write_uint(f, p, val->v.u);
        case RANT_I8: case RANT_I16: case RANT_I32: case RANT_I64:
            return i_rant_schema_write_int(f, p, val->v.i);
        case RANT_F32: case RANT_F64:
            return i_rant_schema_write_f64(f, p, val->v.f);
        case RANT_ARR:
            return i_rant_schema_write_array(f, p, val->bytes);
        case RANT_STR:
            return i_rant_schema_write_str_slot(p, f->str_cap,
                       rant_string((const char *)val->bytes.data, val->bytes.len));
        case RANT_STRUCT:     /* raw bytes, a short source zero fills the tail */
            if (val->bytes.len > f->size || (val->bytes.len && !val->bytes.data)) return 0;
            if (val->bytes.len) memcpy(p, val->bytes.data, val->bytes.len);
            memset(p + val->bytes.len, 0, f->size - val->bytes.len);
            return 1;
        case RANT_VSTR:
            if (val->bytes.len && !val->bytes.data) return 0;
            return i_rant_resize(s, (uint8_t *)buf, cap, &cr, sp, val->bytes.data, val->bytes.len);
        case RANT_VARR:       /* packed elements, refused when the element is variable */
            return i_rant_schema_check_elems(f, val->bytes)
                && i_rant_resize(s, (uint8_t *)buf, cap, &cr, sp, val->bytes.data, val->bytes.len);
        case RANT_MAP:
            if (!rant_map_valid(val->bytes)) return 0;
            return i_rant_resize(s, (uint8_t *)buf, cap, &cr, sp, val->bytes.data, val->bytes.len);
        case RANT_ENUM:
            i_rant_enum_write_val(f->backing, p, val->v.i);
            return 1;
        default: return 0;
    }
}

int rant_set_value(void *buf, size_t cap, const RantSchema *s, uint16_t field, const RantValue *val){
    return rant_set_value_at(buf, cap, s, field, NULL, 0, val);
}

int rant_set_array_count_at(void *buf, size_t cap, const RantSchema *s, uint16_t field,
                            const uint32_t *elems, uint16_t n_elems, uint32_t count){
    const i_Field *f; i_Span sp; i_Carriers cr;
    if (!buf || !s || field >= s->nfields) return 0;
    f = &s->fields[field];
    if (f->kind != RANT_VARR || !i_rant_resolve(s, rant_bytes(buf, cap), f, elems, n_elems, &sp, &cr))
        return 0;
    return i_rant_set_count(s, (uint8_t *)buf, cap, &cr, f, sp, count);
}
