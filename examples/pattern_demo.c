/* Patterns showcase for the DART Explorer: one process runs a "server" node that owns a
 * FUNCTION, a VARIABLE, and a SIGNAL (plus a plain pub/sub topic), and a "client" node that
 * calls / observes / listens to them, so every pattern channel is live and MATCHED. Leave it
 * running and open the explorer on the same domain to see each channel with its own icon:
 *
 *     ./pattern_demo                 # server + client on domain 0 (the explorer default)
 *     ./dart_explorer                # in another terminal
 *
 * The explorer's Topics tab then shows, each with a type icon:
 *     compute/add@req, compute/add@rsp         -> function  (the square-function glyph)
 *     state/temperature, state/temperature@set -> variable  (the variable glyph)
 *     events/alarm                             -> signal    (the lightning glyph)
 *     sensors/lidar                            -> a plain topic (no type icon)
 *
 * Typed payloads go through the schema layer both ways: built with
 * dart_schema_message_default + dart_set_* and read with dart_get_* by field name, exactly
 * as on a plain typed topic. Only the lidar topic is raw bytes (no schema attached).
 *
 *   POSIX  : cc  -std=c99 -Idist examples/pattern_demo.c -o pattern_demo -lrt -lpthread
 *   Windows: gcc -std=c99 -Idist examples/pattern_demo.c -o pattern_demo.exe -lws2_32 -lbcrypt -lwinmm */
#define DART_IMPLEMENTATION
#include "dart.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#ifdef _WIN32
static void sleep_ms(int ms){ Sleep((DWORD)ms); }              /* windows.h via dart.h */
#else
#include <time.h>
static void sleep_ms(int ms){ struct timespec t; t.tv_sec = ms/1000; t.tv_nsec = (long)(ms%1000)*1000000L; nanosleep(&t, NULL); }
#endif

static volatile sig_atomic_t g_run = 1;
static void on_sigint(int s){ (void)s; g_run = 0; }

/* the shared types, at file scope so the handlers can encode/decode with them */
static DartSchema *add_req_s, *add_rsp_s, *temp_s, *alarm_s;

/* raw little-endian helpers for the schema-less lidar topic only */
static uint32_t rd32(DartBytes b){ return b.len >= 4 ? i_dart_le_r32(b.data) : 0; }
static void     wr32(uint8_t *o, uint32_t v){ i_dart_le_w32(o, v); }

/* DartAllocFn over the platform realloc (the hook takes a leading user pointer) */
static void *demo_alloc(void *user, void *ptr, size_t size){
    (void)user; return i_dart_plat_realloc(ptr, size);
}

/* build a one-u32-field message in buf and return the bytes to send */
static DartBytes enc_u32(const DartSchema *s, const char *field, uint32_t v,
                         uint8_t *buf, size_t cap){
    dart_schema_message_default(s, buf, cap);
    dart_set_uint(buf, cap, s, field, v);
    return dart_bytes(buf, dart_schema_size(s));
}

/* server: the compute/add provider, AddRequest { x, y } in, AddResult { sum } back.
 * req->schema is the caller's request schema, delivered like DartMsg.schema on a topic. */
static void add_handler(DartRequest *req, void *user)
{
    int64_t x = dart_get_int(req->data, req->schema, "x");
    int64_t y = dart_get_int(req->data, req->schema, "y");
    uint8_t out[16]; (void)user;
    if (!req->schema){ dart_request_fail(req, "untyped request refused", dart_bytes(NULL, 0)); return; }
    dart_schema_message_default(add_rsp_s, out, sizeof out);
    dart_set_int(out, sizeof out, add_rsp_s, "sum", x + y);
    dart_request_reply(req, dart_bytes(out, dart_schema_size(add_rsp_s)));
}
/* client: log every call result and every signal / plain message */
static void on_reply(const DartResponse *r){
    if (r->status == DART_CALL_OK && r->schema)
        printf("  call  compute/add -> sum=%lld\n",
               (long long)dart_get_int(r->data, r->schema, "sum"));
    else
        printf("  call  compute/add failed (status %d)\n", (int)r->status);
}
static void on_alarm(const DartMsg *m, void *user){
    (void)user;   /* m->schema is the sender's schema, as on any typed topic */
    printf("  signal events/alarm  code=%llu  from %.*s\n",
           (unsigned long long)dart_get_uint(m->data, m->schema ? m->schema : alarm_s, "code"),
           (int)m->publisher_name.len, m->publisher_name.data);
}
static void on_lidar(const DartMsg *m){
    printf("  topic sensors/lidar  #%u\n", rd32(m->data));
}
/* both nodes: every lifecycle event and error on one line via dart_event_str. ev->user is
 * the label passed as opts.user_data; ev->kind == DART_ERROR is the one "did something
 * break?" test (dart_last_error(node) would return the same event). */
static void on_node_event(const DartEvent *ev){
    char text[192];
    printf("  event [%s%s] %s\n", (const char*)ev->user,
           ev->kind == DART_ERROR ? " ERROR" : "", dart_event_str(ev, text, sizeof text));
}

int main(int argc, char **argv){
    uint16_t domain = argc > 1 ? (uint16_t)atoi(argv[1]) : 0;
    DartAllocator sa = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartAllocator ca = dart_allocator_dynamic(i_dart_plat_realloc, 0);
    DartNode *server, *client;
    DartFunction *fn_def, *fn_remote;
    DartVariable *var_def, *var_remote;
    DartSignal   *emitter, *listener;
    DartTopic    *lidar_pub;
    uint32_t tick = 0;
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep output visible under redirect / on Ctrl-C */

    server = dart_node_open(&sa, "robot-server", NULL, on_node_event,
                            &(DartNodeOpts){ .domain = domain, .user_data = (void*)"server" });
    client = dart_node_open(&ca, "robot-client", on_lidar, on_node_event,
                            &(DartNodeOpts){ .domain = domain, .user_data = (void*)"client" });
    if (!server || !client){ fprintf(stderr, "node open failed\n"); return 1; }

    /* typed entities: the explorer shows these shapes and its forms fill them */
    add_req_s = dart_schema_compile(demo_alloc, NULL, "AddRequest { x: i32, y: i32 }", NULL);
    add_rsp_s = dart_schema_compile(demo_alloc, NULL, "AddResult { sum: i32 }", NULL);
    temp_s    = dart_schema_compile(demo_alloc, NULL, "Temperature { celsius: u32 }", NULL);
    alarm_s   = dart_schema_compile(demo_alloc, NULL, "Alarm { code: u32 }", NULL);
    if (!add_req_s || !add_rsp_s || !temp_s || !alarm_s){
        fprintf(stderr, "schema compile failed\n"); return 1;
    }

    /* server side: the definitions (the function body and the variable storage live here)
       plus the emitting end of the signal (no handler passed: it emits, never listens) */
    fn_def    = dart_node_create_function_definition(server, "compute/add", add_req_s, add_rsp_s,
                                          add_handler, NULL, NULL);
    var_def   = dart_node_create_variable_definition(server, "state/temperature", temp_s,
                              &(DartVariableOpts){ .allow_force = 1 });
    emitter   = dart_node_create_signal(server, "events/alarm", alarm_s, NULL, NULL, NULL);
    lidar_pub = dart_node_create_topic(server, "sensors/lidar", DART_PUB_ONLY, NULL, NULL);

    /* client side: remotes (references to the server's definitions) plus the listening end
       of the signal (passing on_alarm IS the subscription) */
    fn_remote = dart_node_create_remote_function(client, "compute/add", add_req_s, add_rsp_s, NULL);
    var_remote= dart_node_create_remote_variable(client, "state/temperature", temp_s, NULL);
    listener  = dart_node_create_signal(client, "events/alarm", alarm_s, on_alarm, NULL, NULL);
    dart_node_create_topic(client, "sensors/lidar", DART_SUB_ONLY, NULL, NULL);
    if (!fn_def || !var_def || !emitter || !lidar_pub || !fn_remote || !var_remote || !listener){
        fprintf(stderr, "pattern setup failed\n"); return 1;
    }

    signal(SIGINT, on_sigint);
    dart_node_start(server);
    dart_node_start(client);
    printf("pattern_demo running on domain %u (server + client).\n"
           "Open the explorer:  dart_explorer%s\n"
           "Ctrl-C to quit.\n\n", domain, domain ? " --domain N" : "");

    while (g_run){
        uint8_t buf[16], raw[4];
        ++tick;

        /* function req/resp: a typed AddRequest, the reply decoded in on_reply */
        dart_schema_message_default(add_req_s, buf, sizeof buf);
        dart_set_int(buf, sizeof buf, add_req_s, "x", (int64_t)tick);
        dart_set_int(buf, sizeof buf, add_req_s, "y", 1000);
        dart_function_call_async(fn_remote, dart_bytes(buf, dart_schema_size(add_req_s)), on_reply, NULL, NULL);

        dart_variable_set(var_def, enc_u32(temp_s, "celsius", tick, buf, sizeof buf));
        dart_signal_emit(emitter, enc_u32(alarm_s, "code", tick, buf, sizeof buf));

        wr32(raw, tick);                                   /* plain topic: raw bytes */
        dart_topic_send(lidar_pub, dart_bytes(raw, 4));

        if (tick % 4 == 0){
            DartBytes cur;
            if (dart_variable_get(var_remote, &cur))
                printf("  var   state/temperature = %llu%s\n",
                       (unsigned long long)dart_get_uint(cur, temp_s, "celsius"),
                       dart_variable_forced(var_remote) ? "  (forced)" : "");
        }
        if (tick == 8){
            dart_variable_force(var_def, enc_u32(temp_s, "celsius", 999, buf, sizeof buf));
            printf("  -- forced temperature to 999 --\n");
        }
        if (tick == 16){ dart_variable_unforce(var_def); printf("  -- unforced temperature --\n"); }
        sleep_ms(500);
    }

    printf("\nshutting down...\n");
    dart_node_close(server, 1);
    dart_node_close(client, 1);
    return 0;
}
