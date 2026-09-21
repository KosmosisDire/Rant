/* The text side: the type writer, the printer and the DSL parser. */
#include "internal.h"

/* The type writer: the growing buffer of wire type bytes that the parser fills. It holds a
 * type on its own, with no schema header, and a failure latches in err. */
typedef struct {
    RantAllocFn alloc; void *user;
    uint8_t *buf;
    size_t   cap;
    size_t   len;                                /* wire bytes written so far */
    int      err;                                /* 0 ok, nonzero latches failure */
    uint16_t depth;                              /* open structs */
    size_t   count_pos[RANT_SCHEMA_MAX_DEPTH]; /* wire offset of each open struct's nfields byte */
    uint16_t field_count[RANT_SCHEMA_MAX_DEPTH];
} i_RantSchemaBuilder;

/* room for extra more bytes, growing the wire buffer through the hook */
static int i_rant_schema_builder_reserve(i_RantSchemaBuilder *b, size_t extra){
    size_t newcap; uint8_t *nb;
    if (b->err) return 0;
    if (b->len + extra <= b->cap) return 1;
    newcap = b->cap ? b->cap : 64u;
    while (newcap < b->len + extra){
        if (newcap > ((size_t)-1) / 2u){ newcap = b->len + extra; break; }
        newcap *= 2u;
    }
    nb = (uint8_t *)b->alloc(b->user, b->buf, newcap);
    if (!nb){ b->err = -1; return 0; }              /* a failed realloc leaves b->buf intact */
    b->buf = nb; b->cap = newcap;
    return 1;
}
static void i_rant_schema_builder_put(i_RantSchemaBuilder *b, uint8_t v){
    if (!i_rant_schema_builder_reserve(b, 1)) return;
    b->buf[b->len++] = v;
}
static void i_rant_schema_builder_put_u16(i_RantSchemaBuilder *b, uint16_t v){
    if (!i_rant_schema_builder_reserve(b, 2)) return;
    i_rant_le_w16(b->buf + b->len, v); b->len += 2;
}
/* raw bytes, a compiled type encoding from elsewhere */
static void i_rant_schema_builder_put_raw(i_RantSchemaBuilder *b, const void *src, size_t len){
    if (!len) return;
    if (!src){ b->err = -6; return; }
    if (!i_rant_schema_builder_reserve(b, len)) return;
    memcpy(b->buf + b->len, src, len);
    b->len += len;
}
/* bytes little endian bytes of v, an enum option's backing sized value */
static void i_rant_schema_builder_put_le(i_RantSchemaBuilder *b, uint64_t v, uint32_t bytes){
    uint32_t i;
    if (!i_rant_schema_builder_reserve(b, bytes)) return;
    for (i = 0; i < bytes; i++) b->buf[b->len++] = (uint8_t)(v >> (8 * i));
}
static void i_rant_schema_builder_put_name(i_RantSchemaBuilder *b, const char *name){
    size_t n = 0, i; if (name) while (name[n]) n++;
    if (b->depth == 0){                                  /* a type on its own carries no field name */
        if (n) b->err = -4;
        return;
    }
    if (n > 255){ b->err = -4; return; }
    if (!i_rant_schema_builder_reserve(b, 1 + n)) return;
    b->buf[b->len++] = (uint8_t)n;
    for (i = 0; i < n; i++) b->buf[b->len++] = (uint8_t)name[i];
}
/* count a field on the innermost open struct. A type on its own counts nothing */
static void i_rant_schema_builder_count(i_RantSchemaBuilder *b){
    uint16_t *c;
    if (b->err || b->depth == 0) return;
    c = &b->field_count[b->depth - 1];
    if (*c >= 255){ b->err = -5; return; }       /* nfields is a u8 */
    (*c)++;
}
/* open a struct type: [STRUCT][nfields placeholder], push a nesting level */
static void i_rant_schema_builder_open_struct(i_RantSchemaBuilder *b){
    if (b->err) return;
    if (b->depth >= RANT_SCHEMA_MAX_DEPTH){ b->err = -2; return; }
    i_rant_schema_builder_put(b, (uint8_t)RANT_STRUCT);
    b->count_pos[b->depth] = b->len;
    i_rant_schema_builder_put(b, 0);
    b->field_count[b->depth] = 0;
    b->depth++;
}

static void i_rant_schema_builder_close_struct(i_RantSchemaBuilder *b){
    if (b->err) return;
    if (b->depth == 0){ b->err = -3; return; }      /* no open struct */
    b->depth--;
    b->buf[b->count_pos[b->depth]] = (uint8_t)b->field_count[b->depth];
}

static i_RantSchemaBuilder i_rant_schema_builder_begin(RantAllocFn alloc, void *user){
    i_RantSchemaBuilder b;
    memset(&b, 0, sizeof b);
    b.alloc = alloc; b.user = user;
    if (!alloc){ b.err = -1; return b; }
    b.cap = 64u;
    b.buf = (uint8_t *)alloc(user, NULL, b.cap);
    if (!b.buf){ b.err = -1; b.cap = 0; }
    return b;
}

/* the root type bytes of a compiled schema, past [version][namelen][name] */
static int i_rant_schema_root_type(const RantSchema *t, const uint8_t **bytes, size_t *len){
    size_t off;
    if (!t || !t->wire.data) return 0;
    off = 2u + t->name.len;
    if (off >= t->wire.len) return 0;
    *bytes = t->wire.data + off;
    *len   = t->wire.len - off;
    return 1;
}

/* Streaming enum construction, so the parser never buffers the option list: open writes
 * the head and returns the count placeholder, add appends, finish backpatches. */
static size_t i_rant_schema_builder_enum_open(i_RantSchemaBuilder *b, RantSchemaTypeKind backing){
    size_t count_pos;
    i_rant_schema_builder_put(b, (uint8_t)RANT_ENUM);
    i_rant_schema_builder_put(b, (uint8_t)backing);
    count_pos = b->len;
    i_rant_schema_builder_put_u16(b, 0);                 /* n, backpatched by finish */
    return count_pos;
}
static void i_rant_schema_builder_enum_add(i_RantSchemaBuilder *b, RantSchemaTypeKind backing,
                                           int64_t value, const char *name, size_t name_len){
    size_t i;
    if (b->err) return;
    if (name_len > 255){ b->err = -4; return; }
    i_rant_schema_builder_put_le(b, (uint64_t)value, rant_schema_scalar_size(backing));
    if (!i_rant_schema_builder_reserve(b, 1 + name_len)) return;
    b->buf[b->len++] = (uint8_t)name_len;
    for (i = 0; i < name_len; i++) b->buf[b->len++] = (uint8_t)name[i];
}
static void i_rant_schema_builder_enum_finish(i_RantSchemaBuilder *b, size_t count_pos, uint16_t count){
    if (b->err) return;
    i_rant_le_w16(b->buf + count_pos, count);
}

/* spelling types back as DSL text */
const char *i_rant_why_kind(uint8_t k){
    switch (k){
        case RANT_U8:    return "u8";  case RANT_U16: return "u16";
        case RANT_U32: return "u32"; case RANT_U64: return "u64";
        case RANT_I8:    return "i8";  case RANT_I16: return "i16";
        case RANT_I32: return "i32"; case RANT_I64: return "i64";
        case RANT_F32: return "f32"; case RANT_F64: return "f64";
        case RANT_BOOL: return "bool"; case RANT_STRUCT: return "struct";
        default: return "?";
    }
}

/* A counting text sink: appends into [p, end) but always tallies the full length in n,
   so a NULL or short buffer still measures. */
typedef struct { char *p, *end; uint32_t n; } i_RantTextOut;
static void i_rant_out_raw(i_RantTextOut *o, const char *s, size_t len){
    size_t i;
    o->n += (uint32_t)len;
    for (i = 0; i < len && o->p < o->end; i++) *o->p++ = s[i];
}
static void i_rant_out_str(i_RantTextOut *o, const char *s){ i_rant_out_raw(o, s, strlen(s)); }
static void i_rant_out_view(i_RantTextOut *o, RantString v){ if (v.data) i_rant_out_raw(o, v.data, v.len); }
static void i_rant_out_indent(i_RantTextOut *o, int levels){ while (levels-- > 0) i_rant_out_raw(o, "  ", 2); }
static void i_rant_out_i64(i_RantTextOut *o, int64_t v){
    char tmp[20]; int k = 0; uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    if (v < 0) i_rant_out_raw(o, "-", 1);
    do { tmp[k++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    while (k) { char c = tmp[--k]; i_rant_out_raw(o, &c, 1); }
}

/* Steps over the type at pos in already validated wire. Returns the position past it. */
static size_t i_rant_skip_type(const uint8_t *w, size_t n, size_t pos){
    uint8_t k;
    if (pos >= n) return n;
    k = w[pos++];
    switch (k){
        case RANT_NAMED:
            if (pos >= n) return n;
            pos += 1u + w[pos];
            return i_rant_skip_type(w, n, pos);
        case RANT_STR: return pos + 2u <= n ? pos + 2u : n;
        case RANT_ARR: return i_rant_skip_type(w, n, pos + 2u <= n ? pos + 2u : n);
        case RANT_VARR: return i_rant_skip_type(w, n, pos);
        case RANT_VSTR: case RANT_MAP: return pos;
        case RANT_ENUM: {
            uint8_t bs; uint16_t cnt, i;
            if (pos + 3u > n) return n;
            bs = (uint8_t)rant_schema_scalar_size((RantSchemaTypeKind)w[pos]);
            cnt = i_rant_le_r16(w + pos + 1);
            pos += 3u;
            for (i = 0; i < cnt; i++){
                if (pos + bs + 1u > n) return n;
                pos += bs + 1u + w[pos + bs];
            }
            return pos <= n ? pos : n;
        }
        case RANT_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= n) return n;
            nf = w[pos++];
            for (i = 0; i < nf; i++){
                if (pos >= n) return n;
                pos += 1u + w[pos];
                if (pos > n) return n;
                pos = i_rant_skip_type(w, n, pos);
            }
            return pos;
        }
        default: return pos;
    }
}

/* Spells the type at pos as DSL text. A NAMED type spells as its bare name and the
 * caller hoists its definition. Returns the position past the type. */
static size_t i_rant_spell_type(i_RantTextOut *o, const uint8_t *w, size_t n, size_t pos,
                                int indent){
    uint8_t k;
    if (pos >= n) return n;
    k = w[pos++];
    switch (k){
        case RANT_NAMED: {
            uint8_t nl;
            if (pos >= n) return n;
            nl = w[pos++];
            i_rant_out_raw(o, (const char *)(w + pos), nl);
            return i_rant_skip_type(w, n, pos + nl);
        }
        case RANT_STR:
            i_rant_out_str(o, "string<");
            i_rant_out_i64(o, (int64_t)i_rant_le_r16(w + pos));
            i_rant_out_str(o, ">");
            return pos + 2u;
        case RANT_VSTR: i_rant_out_str(o, "string"); return pos;
        case RANT_MAP:    i_rant_out_str(o, "map");    return pos;
        case RANT_ARR: {
            uint16_t cnt = i_rant_le_r16(w + pos);
            size_t after = i_rant_spell_type(o, w, n, pos + 2u, indent);
            i_rant_out_str(o, "[");
            i_rant_out_i64(o, (int64_t)cnt);
            i_rant_out_str(o, "]");
            return after;
        }
        case RANT_VARR: {
            size_t after = i_rant_spell_type(o, w, n, pos, indent);
            i_rant_out_str(o, "[]");
            return after;
        }
        case RANT_ENUM: {
            uint8_t backing; uint16_t cnt, i; uint32_t bs;
            if (pos + 3u > n) return n;
            backing = w[pos]; cnt = i_rant_le_r16(w + pos + 1); pos += 3u;
            bs = rant_schema_scalar_size((RantSchemaTypeKind)backing);
            i_rant_out_str(o, "enum<");
            i_rant_out_str(o, i_rant_why_kind(backing));
            i_rant_out_str(o, "> { ");
            for (i = 0; i < cnt; i++){
                uint8_t nl;
                if (pos + bs + 1u > n) return n;
                if (i) i_rant_out_str(o, ", ");
                nl = w[pos + bs];
                i_rant_out_raw(o, (const char *)(w + pos + bs + 1u), nl);
                i_rant_out_str(o, " = ");
                i_rant_out_i64(o, i_rant_enum_read_val(backing, w + pos));
                pos += bs + 1u + nl;
            }
            i_rant_out_str(o, " }");
            return pos;
        }
        case RANT_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= n) return n;
            nf = w[pos++];
            i_rant_out_str(o, "{\n");
            for (i = 0; i < nf; i++){
                uint8_t nl;
                if (pos >= n) return n;
                nl = w[pos++];
                i_rant_out_indent(o, indent + 1);
                i_rant_out_raw(o, (const char *)(w + pos), nl);
                pos += nl;
                i_rant_out_str(o, ": ");
                pos = i_rant_spell_type(o, w, n, pos, indent + 1);
                i_rant_out_str(o, ",\n");
            }
            i_rant_out_indent(o, indent);
            i_rant_out_str(o, "}");
            return pos;
        }
        default:
            i_rant_out_str(o, i_rant_why_kind(k));
            return pos;
    }
}

/* The named types a schema uses, in dependency order and deduped by name, so each
 * prints one leading definition. */
#define I_RANT_MAX_DEFS 48u
typedef struct {
    const uint8_t *w; size_t n;
    struct { uint32_t noff, toff; uint8_t nlen; } d[I_RANT_MAX_DEFS];
    uint16_t count;
} i_RantDefScan;

static void i_rant_defscan_add(i_RantDefScan *L, uint32_t noff, uint8_t nlen, uint32_t toff){
    uint16_t i;
    for (i = 0; i < L->count; i++)
        if (L->d[i].nlen == nlen && memcmp(L->w + L->d[i].noff, L->w + noff, nlen) == 0) return;
    if (L->count >= I_RANT_MAX_DEFS) return;     /* past the cap the tail spells by reference */
    L->d[L->count].noff = noff; L->d[L->count].nlen = nlen; L->d[L->count].toff = toff;
    L->count++;
}

static size_t i_rant_defscan_type(i_RantDefScan *L, size_t pos){
    uint8_t k;
    if (pos >= L->n) return L->n;
    k = L->w[pos];
    if (k == RANT_NAMED){
        uint8_t nl; size_t ipos, after;
        if (pos + 2u > L->n) return L->n;
        nl = L->w[pos + 1];
        ipos = pos + 2u + nl;
        if (ipos > L->n) return L->n;
        after = i_rant_defscan_type(L, ipos);          /* dependencies first */
        i_rant_defscan_add(L, (uint32_t)(pos + 2u), nl, (uint32_t)ipos);
        return after;
    }
    pos++;
    switch (k){
        case RANT_ARR:    return i_rant_defscan_type(L, pos + 2u <= L->n ? pos + 2u : L->n);
        case RANT_VARR: return i_rant_defscan_type(L, pos);
        case RANT_STRUCT: {
            uint8_t nf; uint16_t i;
            if (pos >= L->n) return L->n;
            nf = L->w[pos++];
            for (i = 0; i < nf; i++){
                if (pos >= L->n) return L->n;
                pos += 1u + L->w[pos];
                if (pos > L->n) return L->n;
                pos = i_rant_defscan_type(L, pos);
            }
            return pos;
        }
        default: return i_rant_skip_type(L->w, L->n, pos - 1u);
    }
}

uint32_t rant_schema_print(const RantSchema *s, char *buf, size_t cap){
    i_RantTextOut o;
    i_RantDefScan scan;
    size_t root_type;
    uint16_t i;
    o.p   = (buf && cap) ? buf : NULL;
    o.end = o.p ? buf + cap - 1 : NULL;   /* one byte reserved for the NUL */
    o.n   = 0;
    if (!s){ if (buf && cap) buf[0] = '\0'; return 0; }
    root_type = 2u + s->name.len;
    scan.w = s->wire.data; scan.n = s->wire.len; scan.count = 0;
    i_rant_defscan_type(&scan, root_type);
    for (i = 0; i < scan.count; i++){                    /* each named type, once, in order */
        i_rant_out_raw(&o, (const char *)(scan.w + scan.d[i].noff), scan.d[i].nlen);
        i_rant_out_str(&o, " = ");
        i_rant_spell_type(&o, scan.w, scan.n, scan.d[i].toff, 0);
        i_rant_out_str(&o, "\n");
    }
    if (s->value_root){                    /* a bare type, or Name = type for an alias */
        if (s->name.len){
            i_rant_out_view(&o, s->name);
            i_rant_out_str(&o, " = ");
        }
        i_rant_spell_type(&o, scan.w, scan.n, root_type, 0);
        i_rant_out_str(&o, "\n");
    } else {
        i_rant_out_view(&o, s->name);
        i_rant_out_str(&o, " ");
        i_rant_spell_type(&o, scan.w, scan.n, root_type, 0);
        i_rant_out_str(&o, "\n");
    }
    if (o.p) *o.p = '\0';                   /* o.p is at most end, the reserved byte */
    else if (buf && cap) buf[0] = '\0';
    return o.n;
}

/* The schema DSL. The grammar is in spec/schema.md. A type word that is not a built in
 * resolves against the definitions, the environment and the standard library. */
#ifndef RANT_NO_STDTYPES
const char *i_rant_std_lookup(const char *name);     /* serialize/stdtypes.c */
#else
#define i_rant_std_lookup(name) ((const char *)0)
#endif

typedef struct { const char *p; const char *err; } i_RantDsl;

/* The definition registry: one arena holding each named type's name and compiled type,
 * plus an index. A definition compiles in its own scratch builder before it is appended. */
#define I_RANT_DSL_MAX_DEFS 64u
typedef struct {
    i_RantSchemaBuilder arena;
    struct { uint32_t noff, toff, tlen; uint8_t nlen; } e[I_RANT_DSL_MAX_DEFS];
    uint16_t n;
    uint16_t rec;                                  /* standard type expansion depth */
    const RantSchema *const *env; size_t n_env;
} i_RantDefs;

static void i_rant_dsl_ws(i_RantDsl *d){
    for (;;){
        while (*d->p==' ' || *d->p=='\t' || *d->p=='\r' || *d->p=='\n') d->p++;
        if (d->p[0]=='-' && d->p[1]=='-'){ while (*d->p && *d->p!='\n') d->p++; continue; }
        return;
    }
}
static void i_rant_dsl_fail(i_RantDsl *d, const char *at){ if (!d->err) d->err = at; }
/* an identifier into out[256], NUL terminated. 0 and err when missing or overlong */
static int i_rant_dsl_ident(i_RantDsl *d, char out[256]){
    const char *q = d->p; size_t n;
    if (!((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || *q=='_')){ i_rant_dsl_fail(d, q); return 0; }
    while ((*q>='A'&&*q<='Z') || (*q>='a'&&*q<='z') || (*q>='0'&&*q<='9') || *q=='_') q++;
    n = (size_t)(q - d->p);
    if (n > 255){ i_rant_dsl_fail(d, d->p); return 0; }
    memcpy(out, d->p, n); out[n] = '\0';
    d->p = q;
    return 1;
}
static int i_rant_dsl_expect(i_RantDsl *d, char c){
    if (*d->p == c){ d->p++; return 1; }
    i_rant_dsl_fail(d, d->p);
    return 0;
}
/* a decimal array count, 1 to 65535 */
static int i_rant_dsl_count(i_RantDsl *d, uint16_t *out){
    const char *at = d->p; uint32_t v = 0;
    while (*d->p>='0' && *d->p<='9'){
        v = v*10u + (uint32_t)(*d->p - '0');
        if (v > 0xFFFFu){ i_rant_dsl_fail(d, at); return 0; }
        d->p++;
    }
    if (d->p == at || v == 0){ i_rant_dsl_fail(d, at); return 0; }
    *out = (uint16_t)v;
    return 1;
}
static int i_rant_dsl_kind(const char *s, RantSchemaTypeKind *k){
    static const struct { const char *word; uint8_t kind; } table[] = {
        {"u8",RANT_U8},{"u16",RANT_U16},{"u32",RANT_U32},{"u64",RANT_U64},
        {"i8",RANT_I8},{"i16",RANT_I16},{"i32",RANT_I32},{"i64",RANT_I64},
        {"f32",RANT_F32},{"f64",RANT_F64},{"bool",RANT_BOOL} };
    size_t i;
    for (i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(s, table[i].word) == 0){ *k = (RantSchemaTypeKind)table[i].kind; return 1; }
    return 0;
}
/* a signed decimal enum value fitting int64 */
static int i_rant_dsl_enum_value(i_RantDsl *d, int64_t *out){
    const char *at = d->p; int neg = 0, any = 0; uint64_t v = 0, lim;
    if (*d->p == '-'){ neg = 1; d->p++; }
    lim = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    while (*d->p >= '0' && *d->p <= '9'){
        v = v * 10u + (uint64_t)(*d->p - '0');
        if (v > lim){ i_rant_dsl_fail(d, at); return 0; }
        d->p++; any = 1;
    }
    if (!any){ i_rant_dsl_fail(d, at); return 0; }
    *out = neg ? -(int64_t)v : (int64_t)v;
    return 1;
}
/* enum<uN> { Name [= value], ... } after the word enum was read. Streams the options. */
static void i_rant_dsl_enum(i_RantDsl *d, i_RantSchemaBuilder *b){
    char wname[256]; RantSchemaTypeKind backing; size_t count_pos; uint16_t count = 0; int64_t next = 0;
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_expect(d, '<')) return;
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_ident(d, wname)) return;
    if (!i_rant_dsl_kind(wname, &backing) || !i_rant_enum_backing_ok((uint8_t)backing)){
        i_rant_dsl_fail(d, d->p); return;                  /* the backing must be an integer kind */
    }
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_expect(d, '>')) return;
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_expect(d, '{')) return;
    count_pos = i_rant_schema_builder_enum_open(b, backing);
    for (;;){
        char vname[256]; int64_t v;
        i_rant_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) break;
        if (!i_rant_dsl_ident(d, vname)) return;
        i_rant_dsl_ws(d);
        if (*d->p == '='){   /* an explicit value, else auto increment */
            d->p++; i_rant_dsl_ws(d);
            if (!i_rant_dsl_enum_value(d, &v)) return;
        } else v = next;
        if (!i_rant_enum_val_fits((uint8_t)backing, v)){ i_rant_dsl_fail(d, d->p); return; }
        i_rant_schema_builder_enum_add(b, backing, v, vname, strlen(vname));
        count++; next = v + 1;
        i_rant_dsl_ws(d);
        if (*d->p == ',') d->p++;                        /* an optional separator */
    }
    if (!i_rant_dsl_expect(d, '}')) return;
    i_rant_schema_builder_enum_finish(b, count_pos, count);
}

static void i_rant_dsl_field_type(i_RantDsl *d, i_RantSchemaBuilder *b, i_RantDefs *defs,
                                  const char *name);
static void i_rant_dsl_fields(i_RantDsl *d, i_RantSchemaBuilder *b, i_RantDefs *defs);

/* records a named type: its name bytes then its compiled type, both in the arena */
static int i_rant_defs_add_bytes(i_RantDefs *defs, const char *name, size_t nlen,
                                 const uint8_t *type, size_t tlen){
    uint32_t noff, toff;
    if (defs->n >= I_RANT_DSL_MAX_DEFS || nlen == 0 || nlen > 255 || tlen == 0) return 0;
    noff = (uint32_t)defs->arena.len;
    i_rant_schema_builder_put_raw(&defs->arena, name, nlen);
    toff = (uint32_t)defs->arena.len;
    i_rant_schema_builder_put_raw(&defs->arena, type, tlen);
    if (defs->arena.err) return 0;
    defs->e[defs->n].noff = noff; defs->e[defs->n].nlen = (uint8_t)nlen;
    defs->e[defs->n].toff = toff; defs->e[defs->n].tlen = (uint32_t)tlen;
    defs->n++;
    return 1;
}

static int i_rant_defs_find(i_RantDefs *defs, const char *name, uint32_t *toff, uint32_t *tlen){
    size_t nlen = strlen(name); uint16_t i;
    for (i = 0; i < defs->n; i++)
        if (defs->e[i].nlen == nlen &&
            memcmp(defs->arena.buf + defs->e[i].noff, name, nlen) == 0){
            *toff = defs->e[i].toff; *tlen = defs->e[i].tlen;
            return 1;
        }
    return 0;
}

/* compiles one type spelling, a standard library entry or any type text, as a definition */
static int i_rant_defs_add_text(i_RantDefs *defs, const char *name, const char *text){
    i_RantSchemaBuilder sb; i_RantDsl sd; int ok = 0;
    if (defs->rec >= 8u) return 0;                       /* the roster is acyclic, but be sure */
    defs->rec++;
    sb = i_rant_schema_builder_begin(defs->arena.alloc, defs->arena.user);
    sd.p = text; sd.err = NULL;
    i_rant_dsl_field_type(&sd, &sb, defs, "");
    i_rant_dsl_ws(&sd);
    if (*sd.p) i_rant_dsl_fail(&sd, sd.p);
    if (!sd.err && !sb.err && sb.len)
        ok = i_rant_defs_add_bytes(defs, name, strlen(name), sb.buf, sb.len);
    if (sb.buf) sb.alloc(sb.user, sb.buf, 0);
    defs->rec--;
    return ok;
}

/* Resolves a type name to its encoding in the arena: the text's own definitions first,
 * then the environment schemas, then the standard library. */
static int i_rant_dsl_ref(i_RantDefs *defs, const char *name, uint32_t *toff, uint32_t *tlen){
    size_t nlen = strlen(name), i;
    const char *std;
    if (i_rant_defs_find(defs, name, toff, tlen)) return 1;
    for (i = 0; i < defs->n_env; i++){
        const RantSchema *t = defs->env ? defs->env[i] : NULL;
        const uint8_t *tb; size_t tl;
        if (!t || t->name.len != nlen || memcmp(t->name.data, name, nlen) != 0) continue;
        if (!i_rant_schema_root_type(t, &tb, &tl)) continue;
        if (!i_rant_defs_add_bytes(defs, name, nlen, tb, tl)) return 0;
        return i_rant_defs_find(defs, name, toff, tlen);
    }
    std = i_rant_std_lookup(name);
    if (std && i_rant_defs_add_text(defs, name, std))
        return i_rant_defs_find(defs, name, toff, tlen);
    return 0;
}

/* turns the type just written at b->buf[head..len) into an array of itself by splicing
 * the array head in front of it */
static void i_rant_dsl_splice_array(i_RantSchemaBuilder *b, size_t head, uint16_t count,
                                    int variable){
    size_t extra = variable ? 1u : 3u, tail;
    if (b->err) return;
    if (!i_rant_schema_builder_reserve(b, extra)) return;
    tail = b->len - head;
    memmove(b->buf + head + extra, b->buf + head, tail);
    if (variable){
        b->buf[head] = (uint8_t)RANT_VARR;
    } else {
        b->buf[head] = (uint8_t)RANT_ARR;
        i_rant_le_w16(b->buf + head + 1u, count);
    }
    b->len += extra;
}

/* an optional [N] or [] suffix. 1 if one was read, variable for the [] form */
static int i_rant_dsl_suffix(i_RantDsl *d, uint16_t *count, int *variable){
    *count = 0; *variable = 0;
    i_rant_dsl_ws(d);
    if (*d->p != '[') return 0;
    d->p++;
    i_rant_dsl_ws(d);
    if (*d->p == ']'){ d->p++; *variable = 1; return 1; }
    if (!i_rant_dsl_count(d, count)) return 0;
    i_rant_dsl_ws(d);
    if (!i_rant_dsl_expect(d, ']')) return 0;
    return 1;
}

/* The type whose leading word is in tname, with at pointing at it for errors */
static void i_rant_dsl_word_type(i_RantDsl *d, i_RantSchemaBuilder *b, i_RantDefs *defs,
                                 const char *tname, const char *at){
    RantSchemaTypeKind k; uint16_t cap; uint32_t toff, tlen; size_t nlen;
    if (strcmp(tname, "map") == 0){
        i_rant_schema_builder_put(b, (uint8_t)RANT_MAP);
        return;
    }
    if (strcmp(tname, "enum") == 0){
        i_rant_dsl_enum(d, b);
        return;
    }
    if (strcmp(tname, "string") == 0){                   /* string or string<cap> */
        i_rant_dsl_ws(d);
        if (*d->p != '<'){ i_rant_schema_builder_put(b, (uint8_t)RANT_VSTR); return; }
        d->p++;
        i_rant_dsl_ws(d);
        if (!i_rant_dsl_count(d, &cap)) return;
        i_rant_dsl_ws(d);
        if (!i_rant_dsl_expect(d, '>')) return;
        i_rant_schema_builder_put(b, (uint8_t)RANT_STR);
        i_rant_schema_builder_put_u16(b, cap);
        return;
    }
    if (i_rant_dsl_kind(tname, &k)){
        i_rant_schema_builder_put(b, (uint8_t)k);
        return;
    }
    if (!i_rant_dsl_ref(defs, tname, &toff, &tlen)){ i_rant_dsl_fail(d, at); return; }   /* a named type */
    nlen = strlen(tname);
    i_rant_schema_builder_put(b, (uint8_t)RANT_NAMED);
    i_rant_schema_builder_put(b, (uint8_t)nlen);
    i_rant_schema_builder_put_raw(b, tname, nlen);
    i_rant_schema_builder_put_raw(b, defs->arena.buf + toff, tlen);
}

/* One type plus the field it defines. The count follows the body in the text, so an array
 * suffix wraps what was just written. A type on its own passes name "". */
static void i_rant_dsl_field_type(i_RantDsl *d, i_RantSchemaBuilder *b, i_RantDefs *defs,
                                  const char *name){
    char tname[256]; const char *at; size_t head; uint16_t cnt; int variable; uint8_t kind;
    i_rant_dsl_ws(d);
    at = d->p;
    i_rant_schema_builder_count(b);
    i_rant_schema_builder_put_name(b, name);
    head = b->len;
    if (*d->p == '{'){
        d->p++;
        i_rant_schema_builder_open_struct(b);
        i_rant_dsl_fields(d, b, defs);
        if (!i_rant_dsl_expect(d, '}')) return;
        i_rant_schema_builder_close_struct(b);
    } else {
        if (!i_rant_dsl_ident(d, tname)) return;
        i_rant_dsl_word_type(d, b, defs, tname, at);
    }
    if (d->err || b->err || head >= b->len) return;
    kind = b->buf[head];
    if (kind == RANT_MAP || kind == RANT_ENUM) return;   /* never an array element */
    if (!i_rant_dsl_suffix(d, &cnt, &variable) || d->err) return;
    if (kind == RANT_VSTR){ i_rant_dsl_fail(d, at); return; }    /* string[] is ragged */
    i_rant_dsl_splice_array(b, head, cnt, variable);
}

/* the fields of one struct body, up to and not consuming the closing brace */
static void i_rant_dsl_fields(i_RantDsl *d, i_RantSchemaBuilder *b, i_RantDefs *defs){
    char name[256];
    for (;;){
        i_rant_dsl_ws(d);
        if (*d->p == '}' || *d->p == '\0' || d->err || b->err) return;
        if (!i_rant_dsl_ident(d, name)) return;
        i_rant_dsl_ws(d);
        if (!i_rant_dsl_expect(d, ':')) return;
        i_rant_dsl_field_type(d, b, defs, name);
        if (d->err) return;
        i_rant_dsl_ws(d);
        if (*d->p == ',') d->p++;                        /* an optional separator */
    }
}

/* Name = type: compiles the body on its own, then keeps it, or verifies it matches an
 * existing or reserved definition of the same name exactly. */
static void i_rant_dsl_def(i_RantDsl *d, i_RantDefs *defs, const char *name){
    i_RantSchemaBuilder sb; uint32_t toff = 0, tlen = 0; int ok = 0, exists;
    exists = i_rant_dsl_ref(defs, name, &toff, &tlen);
    sb = i_rant_schema_builder_begin(defs->arena.alloc, defs->arena.user);
    i_rant_dsl_field_type(d, &sb, defs, "");
    if (!d->err && !sb.err && sb.len){
        if (exists)                                      /* redefining is fine if identical */
            ok = (tlen == sb.len && memcmp(defs->arena.buf + toff, sb.buf, sb.len) == 0);
        else
            ok = i_rant_defs_add_bytes(defs, name, strlen(name), sb.buf, sb.len);
    }
    if (sb.buf) sb.alloc(sb.user, sb.buf, 0);
    if (!ok) i_rant_dsl_fail(d, d->p);
}

RantSchema *rant_schema_compile_env(RantAllocFn alloc, void *user, const char *text,
                                    const RantSchema *const *env, size_t n_env,
                                    const char **err){
    i_RantDsl d; i_RantDefs defs; i_RantSchemaBuilder root;
    RantSchema *s = NULL;
    char rootbuf[256];
    const char *rname = NULL; size_t rnlen = 0;
    const uint8_t *rtype = NULL; size_t rtlen = 0;
    int have_root = 0, is_struct = 0;

    if (err) *err = NULL;
    if (!alloc || !text) return NULL;
    memset(&defs, 0, sizeof defs);
    defs.env = env; defs.n_env = n_env;
    defs.arena = i_rant_schema_builder_begin(alloc, user);
    root = i_rant_schema_builder_begin(alloc, user);
    d.p = text; d.err = NULL;
    if (defs.arena.err || root.err) i_rant_dsl_fail(&d, text);

    while (!d.err){
        const char *at;
        i_rant_dsl_ws(&d);
        if (!*d.p) break;
        at = d.p;
        if (!i_rant_dsl_ident(&d, rootbuf)) break;
        i_rant_dsl_ws(&d);
        if (*d.p == '='){                                /* a definition */
            d.p++;
            i_rant_dsl_def(&d, &defs, rootbuf);
            continue;
        }
        if (*d.p == '{'){                                /* Name { fields }: a struct root */
            d.p++;
            i_rant_schema_builder_open_struct(&root);
            i_rant_dsl_fields(&d, &root, &defs);
            if (!i_rant_dsl_expect(&d, '}')) break;
            i_rant_schema_builder_close_struct(&root);
            rnlen = strlen(rootbuf); rname = rootbuf;
            is_struct = 1;
        } else {                                         /* a bare type, maybe a reference */
            d.p = at;
            i_rant_dsl_field_type(&d, &root, &defs, "");
        }
        have_root = 1;
        i_rant_dsl_ws(&d);
        if (*d.p) i_rant_dsl_fail(&d, d.p);                /* the root ends the text */
        break;
    }
    if (!d.err && root.err) i_rant_dsl_fail(&d, d.p);
    if (!d.err && defs.arena.err) i_rant_dsl_fail(&d, d.p);

    if (!d.err){
        if (have_root){
            rtype = root.buf; rtlen = root.len;
            if (!is_struct && rtlen >= 2 && rtype[0] == RANT_NAMED){
                size_t nl = rtype[1];                    /* a bare reference is an alias root */
                if (2u + nl <= rtlen){
                    rname = (const char *)(rtype + 2); rnlen = nl;
                    rtype += 2u + nl; rtlen -= 2u + nl;
                }
            }
        } else if (defs.n){                              /* no root: the last definition is it */
            uint16_t last = (uint16_t)(defs.n - 1u);
            rname = (const char *)(defs.arena.buf + defs.e[last].noff);
            rnlen = defs.e[last].nlen;
            rtype = defs.arena.buf + defs.e[last].toff;
            rtlen = defs.e[last].tlen;
        }
        if (!rtype || !rtlen) i_rant_dsl_fail(&d, d.p);
    }
    /* the reservation covers definitions and references, never a plain Name { ... } root,
     * since nothing can reference a root. See spec/schema.md. */
    if (!d.err){
        size_t wlen = 2u + rnlen + rtlen;
        uint8_t *w = (uint8_t *)alloc(user, NULL, wlen);
        if (w){
            w[0] = (uint8_t)RANT_SCHEMA_WIRE_VERSION;
            w[1] = (uint8_t)rnlen;
            if (rnlen) memcpy(w + 2, rname, rnlen);
            memcpy(w + 2 + rnlen, rtype, rtlen);
            s = rant_schema_parse(w, wlen, alloc, user);
            alloc(user, w, 0);
        }
        if (!s) i_rant_dsl_fail(&d, d.p);
    }
    if (root.buf) alloc(user, root.buf, 0);
    if (defs.arena.buf) alloc(user, defs.arena.buf, 0);
    if (!s && err) *err = d.err ? d.err : d.p;
    return s;
}

RantSchema *rant_schema_compile(RantAllocFn alloc, void *user, const char *text, const char **err){
    return rant_schema_compile_env(alloc, user, text, NULL, 0, err);
}
