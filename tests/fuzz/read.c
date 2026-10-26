/* The reader behind read.h, on the public API only. Each walk takes a step budget, since a
 * hostile count would otherwise turn the quadratic map walk into a timeout. */
#include "read.h"
#include "fuzz.h"

#define FUZZ_READ_STEPS 4096

static volatile uint8_t g_sink;

void fuzz_touch(const void *data, size_t len){
    const uint8_t *p = (const uint8_t *)data;
    uint8_t x = 0;
    size_t i;
    for (i = 0; i < len; i++) x ^= p[i];
    g_sink ^= x;
}

static void read_value(const RantValue *v, int depth, long *steps);

/* a valid map reads every entry, and its nested bodies are valid maps too */
static void read_map(RantBytes map, int depth, long *steps){
    int valid = rant_map_valid(map);
    uint16_t i, n = rant_map_count(map);
    for (i = 0; i < n && *steps > 0; i++, (*steps)--){
        RantString key; RantValue v;
        int ok = rant_map_at(map, i, &key, &v);
        if (valid) FUZZ_REQUIRE(ok);
        if (!ok) return;
        fuzz_touch(key.data, key.len);
        read_value(&v, depth, steps);
    }
}

static void read_array(RantBytes arr, int depth, long *steps){
    uint16_t i, n = rant_map_array_count(arr);
    for (i = 0; i < n && *steps > 0; i++, (*steps)--){
        RantValue v;
        if (!rant_map_array_at(arr, i, &v)) return;
        read_value(&v, depth, steps);
    }
}

static void read_value(const RantValue *v, int depth, long *steps){
    fuzz_touch(v->bytes.data, v->bytes.len);
    if (depth >= RANT_SCHEMA_MAX_DEPTH) return;
    if (v->kind == RANT_MAP) read_map(v->bytes, depth + 1, steps);
    else if (v->kind == RANT_VARR) read_array(v->bytes, depth + 1, steps);
}

void fuzz_read_map(RantBytes map){
    long steps = FUZZ_READ_STEPS;
    read_map(map, 0, &steps);
}

/* every element of field f, through the struct arrays around it */
static void read_field(const RantSchema *s, RantBytes msg, uint16_t f, const uint16_t *chain,
                       uint16_t depth, uint32_t *elems, uint16_t filled, long *steps){
    RantValue v;
    uint32_t i, count;
    if (filled == depth){
        if (rant_get_value_at(msg, s, f, elems, depth, &v)){
            fuzz_touch(v.bytes.data, v.bytes.len);
            if (v.kind == RANT_MAP) read_map(v.bytes, 0, steps);
            if (v.kind == RANT_ENUM){
                RantString name = rant_enum_name_of(s, f, v.v.i);
                fuzz_touch(name.data, name.len);
            }
        }
        (*steps)--;
        return;
    }
    count = rant_array_count_at(msg, s, chain[depth - 1 - filled]);
    for (i = 0; i < count && i < 3 && *steps > 0; i++){
        elems[filled] = i;
        read_field(s, msg, f, chain, depth, elems, (uint16_t)(filled + 1), steps);
    }
}

void fuzz_read_message(const RantSchema *s, RantBytes msg){
    long steps = FUZZ_READ_STEPS;
    uint16_t f, nf = rant_schema_field_count(s);
    int valid = rant_schema_validate(s, msg);
    if (valid) FUZZ_REQUIRE(rant_schema_msg_len(s, msg.data, msg.len) == msg.len);
    for (f = 0; f < nf && steps > 0; f++){
        RantSchemaFieldInfo fi;
        uint16_t chain[RANT_SCHEMA_MAX_DEPTH], depth = 0, p;
        uint32_t elems[RANT_SCHEMA_MAX_DEPTH];
        if (!rant_schema_field_at(s, f, &fi)) continue;
        if (valid && fi.arr_depth == 0){
            RantValue v;
            FUZZ_REQUIRE(rant_get_value(msg, s, f, &v));
        }
        for (p = fi.arr_parent; p != 0xFFFF && depth < RANT_SCHEMA_MAX_DEPTH; depth++){
            RantSchemaFieldInfo pi;
            chain[depth] = p;
            if (!rant_schema_field_at(s, p, &pi)) break;
            p = pi.arr_parent;
        }
        if (depth != fi.arr_depth) continue;
        read_field(s, msg, f, chain, depth, elems, 0, &steps);
    }
}
