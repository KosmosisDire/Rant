/* Typed messages and maps over hostile bytes. The first byte picks a schema from schemas.h,
 * the rest is the message. One past the table, the rest is a bare map body. */
#include "support/test.h"
#include "fuzz.h"
#include "read.h"
#include "schemas.h"

static RantAllocator g_alloc;
static RantSchema *g_schemas[FUZZ_SCHEMA_TEXTS];

static void load_schemas(void){
    size_t i;
    if (g_schemas[0]) return;
    g_alloc = rant_allocator_heap(0);
    for (i = 0; i < FUZZ_SCHEMA_TEXTS; i++){
        i_RantSchemaErr err;
        g_schemas[i] = test_compile(&g_alloc, fuzz_schema_texts[i], &err);
        if (!g_schemas[i]){
            fprintf(stderr, "schemas.h text %u does not compile: %s\n", (unsigned)i, err.why);
            abort();
        }
    }
}

static int is_integer(uint8_t kind){
    return kind == RANT_U8 || kind == RANT_U16 || kind == RANT_U32 || kind == RANT_U64
        || kind == RANT_I8 || kind == RANT_I16 || kind == RANT_I32 || kind == RANT_I64
        || kind == RANT_ENUM;
}

/* Copies a valid message field by field onto a default one. The setters never leave an
 * invalid message, and an integer reads back as it was written. */
static void copy_properties(const RantSchema *s, RantBytes msg){
    size_t cap = msg.len + rant_schema_msg_min(s);
    uint8_t *out = (uint8_t *)malloc(cap ? cap : 1);
    uint16_t f, nf = rant_schema_field_count(s);
    uint32_t len;
    FUZZ_REQUIRE(out && rant_schema_message_default(s, out, cap));
    for (f = 0; f < nf; f++){
        RantSchemaFieldInfo fi; RantValue v;
        if (!rant_schema_field_at(s, f, &fi) || fi.arr_depth) continue;
        if (rant_get_value(msg, s, f, &v)) (void)rant_set_value(out, cap, s, f, &v);
    }
    len = rant_schema_msg_len(s, out, cap);
    FUZZ_REQUIRE(len != 0 && rant_schema_validate(s, rant_bytes(out, len)));
    for (f = 0; f < nf; f++){
        RantSchemaFieldInfo fi; RantValue a, b;
        if (!rant_schema_field_at(s, f, &fi) || fi.arr_depth || !is_integer(fi.kind)) continue;
        FUZZ_REQUIRE(rant_get_value(msg, s, f, &a) && rant_get_value(rant_bytes(out, len), s, f, &b));
        FUZZ_REQUIRE(a.v.u == b.v.u);
    }
    free(out);
}

/* Writes every row once, at index 0 of each array around it, onto a copy of a valid
 * message. A write keeps it valid, and a refused one leaves it as it was. */
static void write_properties(const RantSchema *s, RantBytes msg){
    size_t cap = msg.len + 64u;
    uint8_t *buf = (uint8_t *)calloc(1, cap), *was = (uint8_t *)malloc(cap);
    uint16_t f, nf = rant_schema_field_count(s);
    uint32_t zeros[RANT_SCHEMA_MAX_DEPTH];
    FUZZ_REQUIRE(buf && was);
    memset(zeros, 0, sizeof zeros);
    if (msg.len) memcpy(buf, msg.data, msg.len);
    for (f = 0; f < nf; f++){
        RantSchemaFieldInfo fi; RantValue v; uint32_t len;
        if (!rant_schema_field_at(s, f, &fi)
            || !rant_get_value_at(rant_bytes(buf, cap), s, f, zeros, fi.arr_depth, &v)) continue;
        if (fi.kind == RANT_STRUCT || fi.kind == RANT_ARR || fi.kind == RANT_STR) continue;
        if (fi.kind == RANT_VSTR) v.bytes = rant_bytes("ab", 2);
        if (fi.kind == RANT_MAP) v.bytes = rant_bytes(NULL, 0);
        if (fi.kind == RANT_VARR){ v.count += 1u; v.bytes = rant_bytes(NULL, 0); }
        memcpy(was, buf, cap);
        if (rant_set_value_at(buf, cap, s, f, zeros, fi.arr_depth, &v)){
            len = rant_schema_msg_len(s, buf, cap);
            FUZZ_REQUIRE(len != 0 && rant_schema_validate(s, rant_bytes(buf, len)));
        } else FUZZ_REQUIRE(memcmp(was, buf, cap) == 0);
    }
    free(buf); free(was);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size){
    RantBytes body;
    size_t k;
    if (size < 1) return 0;
    load_schemas();
    k = data[0] % (FUZZ_SCHEMA_TEXTS + 1);
    body = rant_bytes(data + 1, size - 1);
    if (k == FUZZ_SCHEMA_TEXTS){
        fuzz_read_map(body);
        return 0;
    }
    fuzz_read_message(g_schemas[k], body);
    if (rant_schema_validate(g_schemas[k], body)){
        copy_properties(g_schemas[k], body);
        write_properties(g_schemas[k], body);
    }
    return 0;
}

/* a map with every kind the body grammar has, nested two deep */
static uint32_t build_map(uint8_t *buf, size_t cap){
    RantMapWriter w = rant_map_begin(buf, cap);
    rant_map_put_uint(&w, "count", 7);
    rant_map_put_int(&w, "delta", -3);
    rant_map_put_f64(&w, "gain", 0.5);
    rant_map_put_f32(&w, "trim", 1.25f);
    rant_map_put_bool(&w, "armed", 1);
    rant_map_put_string(&w, "label", rant_string("arm", 3));
    rant_map_open_map(&w, "inner");
    rant_map_put_uint(&w, "depth", 2);
    rant_map_open_array(&w, "list");
    rant_map_put_int(&w, NULL, 1);
    rant_map_put_string(&w, NULL, rant_string("two", 3));
    rant_map_close(&w);
    rant_map_close(&w);
    return rant_map_finish(&w);
}

/* the default message with every top level field given a value */
static uint32_t build_message(const RantSchema *s, uint8_t *buf, size_t cap){
    uint16_t f, nf = rant_schema_field_count(s);
    if (!rant_schema_message_default(s, buf, cap)) return 0;
    for (f = 0; f < nf; f++){
        RantSchemaFieldInfo fi; RantValue v; uint8_t elems[64]; uint8_t map[256];
        if (!rant_schema_field_at(s, f, &fi) || fi.arr_depth) continue;
        memset(&v, 0, sizeof v);
        memset(elems, 0x11, sizeof elems);
        v.v.u = f + 1u;
        if (fi.kind == RANT_F32 || fi.kind == RANT_F64) v.v.f = 1.5 * f;
        if (fi.kind == RANT_STR || fi.kind == RANT_VSTR) v.bytes = rant_bytes("fuzz", 4);
        if (fi.kind == RANT_VARR){                 /* packed scalars, else two default elements */
            RantSchemaFieldInfo ei;
            size_t ne;
            if (!rant_schema_field_at(s, (uint16_t)(f + 1u), &ei)) continue;
            v.count = 2;
            ne = ei.size && ei.kind != RANT_STRUCT ? sizeof elems / ei.size : 0;
            if (ne) v.bytes = rant_bytes(elems, ei.size * (ne < 3 ? ne : 3));
        }
        if (fi.kind == RANT_MAP) v.bytes = rant_bytes(map, build_map(map, sizeof map));
        if (fi.kind == RANT_ARR || fi.kind == RANT_STRUCT) continue;
        (void)rant_set_value(buf, cap, s, f, &v);
    }
    return rant_schema_msg_len(s, buf, cap);
}

void fuzz_seeds(FuzzEmit emit){
    uint8_t buf[4096];
    size_t k;
    uint32_t n;
    load_schemas();
    for (k = 0; k < FUZZ_SCHEMA_TEXTS; k++){
        uint32_t min = rant_schema_msg_min(g_schemas[k]);
        buf[0] = (uint8_t)k;
        if (min < sizeof buf && rant_schema_message_default(g_schemas[k], buf + 1, min))
            emit(buf, 1 + min);
        n = build_message(g_schemas[k], buf + 1, sizeof buf - 1);
        if (n) emit(buf, 1 + n);
    }
    buf[0] = (uint8_t)FUZZ_SCHEMA_TEXTS;
    n = build_map(buf + 1, sizeof buf - 1);
    if (n) emit(buf, 1 + n);
}
