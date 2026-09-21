/* The map. Readers walk hostile bytes: every read is bounds checked, the kind vocabulary
 * is closed and nesting is depth capped. The body grammar is in spec/schema.md. */
#include "internal.h"

/* Reads (out set) or skips (out NULL) one value at r->pos. 1, or 0 with r->fail. */
static int i_rant_map_value(i_Rd *r, uint16_t depth, RantValue *out){
    uint8_t k = i_rant_rd_u8(r);
    uint32_t sz;
    if (r->fail) return 0;
    if (out){ memset(out, 0, sizeof *out); out->kind = k; }
    sz = rant_schema_scalar_size((RantSchemaTypeKind)k);
    if (sz){
        const uint8_t *p = r->w + r->pos;
        i_rant_rd_skip(r, sz);
        if (r->fail) return 0;
        if (out) switch (k){
            case RANT_U8: case RANT_U16: case RANT_U32: case RANT_U64: case RANT_BOOL:
                out->v.u = i_rant_schema_read_uint(k, p); break;
            case RANT_I8: case RANT_I16: case RANT_I32: case RANT_I64:
                out->v.i = i_rant_schema_read_int(k, p); break;
            default:
                out->v.f = i_rant_schema_read_f64(k, p); break;
        }
        return 1;
    }
    if (k == RANT_VSTR){
        uint16_t len = i_rant_rd_u16(r);
        const uint8_t *p = r->w + r->pos;
        i_rant_rd_skip(r, len);
        if (r->fail) return 0;
        if (out) out->bytes = rant_bytes(p, len);
        return 1;
    }
    if (k == RANT_MAP || k == RANT_VARR){
        size_t start = r->pos; uint16_t n, i;
        if (depth + 1u >= RANT_SCHEMA_MAX_DEPTH){ r->fail = 1; return 0; }
        n = i_rant_rd_u16(r);
        for (i = 0; i < n && !r->fail; i++){
            if (k == RANT_MAP){
                uint8_t kl = i_rant_rd_u8(r);
                i_rant_rd_skip(r, kl);
            }
            if (!i_rant_map_value(r, (uint16_t)(depth + 1), NULL)) return 0;
        }
        if (r->fail) return 0;
        if (out){ out->bytes = rant_bytes(r->w + start, r->pos - start); out->count = n; }
        return 1;
    }
    r->fail = 1;                                        /* unknown kind: reject */
    return 0;
}

uint16_t rant_map_count(RantBytes map){
    return (map.data && map.len >= 2) ? i_rant_le_r16(map.data) : 0;
}
uint16_t rant_map_array_count(RantBytes arr){ return rant_map_count(arr); }

int rant_map_at(RantBytes map, uint16_t index, RantString *key, RantValue *out){
    i_Rd r; uint16_t n, i;
    if (!map.data || map.len < 2) return 0;
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_rant_rd_u16(&r);
    if (index >= n) return 0;
    for (i = 0; i <= index; i++){
        uint8_t kl = i_rant_rd_u8(&r);
        const char *kp = (const char *)(r.w + r.pos);
        i_rant_rd_skip(&r, kl);
        if (r.fail) return 0;
        if (i == index){
            if (!i_rant_map_value(&r, 0, out)) return 0;
            if (key) *key = rant_string(kp, kl);
            return 1;
        }
        if (!i_rant_map_value(&r, 0, NULL)) return 0;
    }
    return 0;
}

int rant_map_get(RantBytes map, const char *key, RantValue *out){
    i_Rd r; uint16_t n, i; size_t want;
    if (!map.data || map.len < 2 || !key) return 0;
    want = strlen(key);
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_rant_rd_u16(&r);
    for (i = 0; i < n; i++){
        uint8_t kl = i_rant_rd_u8(&r);
        const char *kp = (const char *)(r.w + r.pos);
        i_rant_rd_skip(&r, kl);
        if (r.fail) return 0;
        if (kl == want && (want == 0 || memcmp(kp, key, want) == 0))
            return i_rant_map_value(&r, 0, out);
        if (!i_rant_map_value(&r, 0, NULL)) return 0;
    }
    return 0;
}

int rant_map_array_at(RantBytes arr, uint16_t index, RantValue *out){
    i_Rd r; uint16_t n, i;
    if (!arr.data || arr.len < 2) return 0;
    r.w = arr.data; r.n = arr.len; r.pos = 0; r.fail = 0;
    n = i_rant_rd_u16(&r);
    if (index >= n) return 0;
    for (i = 0; i < index; i++)
        if (!i_rant_map_value(&r, 0, NULL)) return 0;
    return i_rant_map_value(&r, 0, out);
}

int rant_map_valid(RantBytes map){
    i_Rd r; uint16_t n, i;
    if (map.len == 0) return 1;                         /* an empty body is an empty map */
    if (!map.data || map.len < 2) return 0;
    r.w = map.data; r.n = map.len; r.pos = 0; r.fail = 0;
    n = i_rant_rd_u16(&r);
    for (i = 0; i < n; i++){
        uint8_t kl = i_rant_rd_u8(&r);
        i_rant_rd_skip(&r, kl);
        if (r.fail || !i_rant_map_value(&r, 0, NULL)) return 0;
    }
    return r.pos == r.n;                                /* no trailing garbage */
}

/* the map writer */
RantMapWriter rant_map_begin(void *buf, size_t cap){
    RantMapWriter w;
    memset(&w, 0, sizeof w);
    w.buf = (uint8_t *)buf; w.cap = cap;
    if (!buf || cap < 2){ w.err = -1; return w; }
    w.buf[0] = 0; w.buf[1] = 0;                         /* the root map's count */
    w.count_pos[0] = 0; w.len = 2;
    w.depth = 1;
    return w;
}

/* room for extra more bytes. The buffer is the caller's, so no growth */
static int i_rant_map_room(RantMapWriter *w, size_t extra){
    if (w->err) return 0;
    if (w->len + extra > w->cap){ w->err = -1; return 0; }
    return 1;
}
/* start an entry: a key in a map, none in an array. Bumps the open count */
static int i_rant_map_entry(RantMapWriter *w, const char *key){
    size_t kl = 0;
    if (w->err) return 0;
    if (w->depth == 0){ w->err = -3; return 0; }        /* a finished writer */
    if (w->is_arr[w->depth - 1]){
        if (key){ w->err = -3; return 0; }              /* array elements carry no key */
    } else {
        if (!key){ w->err = -3; return 0; }
        kl = strlen(key);
        if (kl > 255){ w->err = -4; return 0; }
    }
    if (w->count[w->depth - 1] == 0xFFFFu){ w->err = -5; return 0; }
    if (key){
        if (!i_rant_map_room(w, 1 + kl)) return 0;
        w->buf[w->len++] = (uint8_t)kl;
        if (kl) memcpy(w->buf + w->len, key, kl);
        w->len += kl;
    }
    w->count[w->depth - 1]++;
    return 1;
}
static int i_rant_map_put_scalar(RantMapWriter *w, const char *key, uint8_t kind,
                                 const uint8_t *le, uint32_t sz){
    if (!w || !i_rant_map_entry(w, key) || !i_rant_map_room(w, 1u + sz)) return 0;
    w->buf[w->len++] = kind;
    memcpy(w->buf + w->len, le, sz);
    w->len += sz;
    return 1;
}

int rant_map_put_uint(RantMapWriter *w, const char *key, uint64_t v){
    uint8_t le[8];                                       /* the smallest kind that fits */
    uint8_t k = v <= 0xFFu ? RANT_U8 : v <= 0xFFFFu ? RANT_U16
              : v <= 0xFFFFFFFFu ? RANT_U32 : RANT_U64;
    i_rant_le_w64(le, v);
    return i_rant_map_put_scalar(w, key, k, le, rant_schema_scalar_size((RantSchemaTypeKind)k));
}

int rant_map_put_int(RantMapWriter *w, const char *key, int64_t v){
    uint8_t le[8];
    uint8_t k = (v >= -128 && v <= 127) ? RANT_I8
              : (v >= -32768 && v <= 32767) ? RANT_I16
              : (v >= -2147483647 - 1 && v <= 2147483647) ? RANT_I32 : RANT_I64;
    i_rant_le_w64(le, (uint64_t)v);                        /* two's complement LE: the low bytes */
    return i_rant_map_put_scalar(w, key, k, le, rant_schema_scalar_size((RantSchemaTypeKind)k));
}

int rant_map_put_f64(RantMapWriter *w, const char *key, double v){
    uint8_t le[8]; uint64_t b;
    memcpy(&b, &v, 8); i_rant_le_w64(le, b);
    return i_rant_map_put_scalar(w, key, (uint8_t)RANT_F64, le, 8);
}

int rant_map_put_f32(RantMapWriter *w, const char *key, float v){
    uint8_t le[4]; uint32_t b;
    memcpy(&b, &v, 4); i_rant_le_w32(le, b);
    return i_rant_map_put_scalar(w, key, (uint8_t)RANT_F32, le, 4);
}

int rant_map_put_bool(RantMapWriter *w, const char *key, int v){
    uint8_t b = v ? 1u : 0u;
    return i_rant_map_put_scalar(w, key, (uint8_t)RANT_BOOL, &b, 1);
}

int rant_map_put_string(RantMapWriter *w, const char *key, RantString v){
    if (!w) return 0;
    if (v.len > 0xFFFFu || (v.len && !v.data)){ w->err = -4; return 0; }
    if (!i_rant_map_entry(w, key) || !i_rant_map_room(w, 3u + v.len)) return 0;
    w->buf[w->len++] = (uint8_t)RANT_VSTR;
    i_rant_le_w16(w->buf + w->len, (uint16_t)v.len); w->len += 2;
    if (v.len) memcpy(w->buf + w->len, v.data, v.len);
    w->len += v.len;
    return 1;
}

static int i_rant_map_open(RantMapWriter *w, const char *key, uint8_t kind, uint8_t is_arr){
    if (!w || !i_rant_map_entry(w, key)) return 0;
    if (w->depth >= RANT_SCHEMA_MAX_DEPTH){ w->err = -2; return 0; }
    if (!i_rant_map_room(w, 3)) return 0;
    w->buf[w->len++] = kind;
    w->count_pos[w->depth] = w->len;                    /* the nested body's count */
    w->buf[w->len++] = 0; w->buf[w->len++] = 0;
    w->count[w->depth] = 0; w->is_arr[w->depth] = is_arr;
    w->depth++;
    return 1;
}
int rant_map_open_map(RantMapWriter *w, const char *key){
    return i_rant_map_open(w, key, (uint8_t)RANT_MAP, 0);
}
int rant_map_open_array(RantMapWriter *w, const char *key){
    return i_rant_map_open(w, key, (uint8_t)RANT_VARR, 1);
}
int rant_map_close(RantMapWriter *w){
    if (!w || w->err) return 0;
    if (w->depth <= 1){ w->err = -3; return 0; }        /* the root closes in finish */
    w->depth--;
    i_rant_le_w16(w->buf + w->count_pos[w->depth], w->count[w->depth]);
    return 1;
}

uint32_t rant_map_finish(RantMapWriter *w){
    if (!w || w->err || w->depth != 1) return 0;        /* else an unbalanced open and close */
    i_rant_le_w16(w->buf + w->count_pos[0], w->count[0]);
    w->depth = 0;
    return (uint32_t)w->len;
}
