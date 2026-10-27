/* Schema text and schema wire, the two ways a schema reaches a node. The first byte picks:
 * even compiles the rest as DSL text, odd parses it as the wire a peer advertises. */
#include "support/test.h"
#include "fuzz.h"
#include "schemas.h"
#include "read.h"

#define FUZZ_MSG_MAX (1u << 16)

/* What holds for any schema, and what only a compiled one owes, since a peer's wire may hold
   what text refuses (spec/testing.md) */
static void schema_properties(i_RantRegistry *reg, RantAllocator *a, const RantSchema *s,
                              int compiled){
    RantBytes wire = rant_schema_wire(s);
    RantSchema *again = i_rant_schema_parse(wire.data, wire.len, rant_allocator_alloc, a);
    uint32_t min = rant_schema_msg_min(s), plen;
    uint16_t f, nf = rant_schema_field_count(s);
    char *text;
    FUZZ_REQUIRE(again && rant_schema_hash(again) == rant_schema_hash(s));
    for (f = 0; f < nf; f++){
        RantSchemaFieldInfo fi;
        uint16_t e, ne = rant_schema_enum_count(s, f);
        FUZZ_REQUIRE(rant_schema_field_at(s, f, &fi));
        for (e = 0; e < ne; e++){
            RantString name; int64_t v;
            if (!rant_schema_enum_variant(s, f, e, &v, &name)) continue;
            fuzz_touch(name.data, name.len);
            if (compiled) FUZZ_REQUIRE(rant_enum_name_of(s, f, v).len > 0);
        }
    }
    if (min <= FUZZ_MSG_MAX){
        uint8_t *msg = (uint8_t *)malloc(min ? min : 1);
        FUZZ_REQUIRE(msg && rant_schema_message_default(s, msg, min));
        FUZZ_REQUIRE(rant_schema_validate(s, rant_bytes(msg, min)));
        free(msg);
    }
    plen = rant_schema_print(s, NULL, 0);
    text = (char *)malloc((size_t)plen + 1);
    FUZZ_REQUIRE(text && rant_schema_print(s, text, (size_t)plen + 1) == plen);
    if (compiled){
        i_RantSchemaErr err;
        RantSchema *back;
        char why[256];
        if (!rant_schema_subset_why(s, s, why, sizeof why))
            fprintf(stderr, "cannot read itself: %s\n", why);
        FUZZ_REQUIRE(rant_schema_subset(s, s));
        back = i_rant_registry_compile(reg, text, &err);
        if (!back || rant_schema_hash(back) != rant_schema_hash(s))
            fprintf(stderr, "printed: %s\n%s\n", text, back ? "compiles to another schema" : err.why);
        FUZZ_REQUIRE(back && rant_schema_hash(back) == rant_schema_hash(s));
    }
    free(text);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size){
    RantAllocator a = rant_allocator_heap(0);
    i_RantRegistry reg;
    RantSchema *s;
    if (size < 1) return 0;
    i_rant_registry_init(&reg, rant_allocator_alloc, &a);
    if (data[0] % 2 == 0){
        char *text = (char *)malloc(size);
        i_RantSchemaErr err;
        if (!text) abort();
        memcpy(text, data + 1, size - 1);
        text[size - 1] = 0;
        s = i_rant_registry_compile(&reg, text, &err);
        if (!s) FUZZ_REQUIRE(err.why != NULL);
        free(text);
    } else {
        (void)i_rant_schema_wire_why(data + 1, size - 1);
        s = i_rant_registry_parse(&reg, data + 1, size - 1);
    }
    if (s) schema_properties(&reg, &a, s, data[0] % 2 == 0);
    rant_allocator_reset(&a);
    return 0;
}

static void emit_text(FuzzEmit emit, const char *text){
    uint8_t buf[1024];
    size_t n = strlen(text);
    if (n + 1 > sizeof buf) return;
    buf[0] = 0;
    memcpy(buf + 1, text, n);
    emit(buf, n + 1);
}

static void emit_wire(FuzzEmit emit, const char *text){
    RantAllocator a = rant_allocator_heap(0);
    RantSchema *s = test_compile(&a, text, NULL);
    if (s){
        RantBytes w = rant_schema_wire(s);
        uint8_t *buf = (uint8_t *)malloc(w.len + 1);
        if (buf){
            buf[0] = 1;
            memcpy(buf + 1, w.data, w.len);
            emit(buf, w.len + 1);
            free(buf);
        }
    }
    rant_allocator_reset(&a);
}

void fuzz_seeds(FuzzEmit emit){
    size_t i;
    int t;
    for (i = 0; i < FUZZ_SCHEMA_TEXTS; i++){
        emit_text(emit, fuzz_schema_texts[i]);
        emit_wire(emit, fuzz_schema_texts[i]);
    }
    for (t = 1; t < (int)RANT_STD_COUNT; t++){
        const char *name = rant_std_name((RantStdType)t);
        if (!name) continue;
        emit_text(emit, name);
        emit_wire(emit, name);
    }
}
