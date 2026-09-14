/* A patterns showcase for the explorer: a server node owns a function, a task and a
 * variable next to a plain topic, and a client node calls and observes them. */
#define RAMBLE_IMPLEMENTATION
#include "ramble.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#ifdef _WIN32
static void sleep_ms(int ms){ Sleep((DWORD)ms); }              /* windows.h via ramble.h */
#else
#include <time.h>
static void sleep_ms(int ms){ struct timespec t; t.tv_sec = ms/1000; t.tv_nsec = (long)(ms%1000)*1000000L; nanosleep(&t, NULL); }
#endif

static volatile sig_atomic_t g_run = 1;
static void on_sigint(int s){ (void)s; g_run = 0; }

/* the shared types, at file scope so the handlers can encode and decode with them */
static RambleSchema *add_req_s, *add_rsp_s, *temp_s;

/* raw little endian helpers for the schema less lidar topic only */
static uint32_t rd32(RambleBytes b){ return b.len >= 4 ? i_ramble_le_r32(b.data) : 0; }
static void     wr32(uint8_t *o, uint32_t v){ i_ramble_le_w32(o, v); }

/* build a one u32 field message in buf and return the bytes to send */
static RambleBytes enc_u32(const RambleSchema *s, const char *field, uint32_t v,
                         uint8_t *buf, size_t cap){
    ramble_schema_message_default(s, buf, cap);
    ramble_set_uint(buf, cap, s, field, v);
    return ramble_bytes(buf, ramble_schema_size(s));
}

/* server: the compute/add provider, AddRequest in and AddResult back. req->schema is the
 * caller's request schema, delivered like RambleMsg.schema on a topic. */
static void add_handler(RambleRequest *req, void *user)
{
    int64_t x = ramble_get_int(req->data, req->schema, "x");
    int64_t y = ramble_get_int(req->data, req->schema, "y");
    uint8_t out[16]; (void)user;
    if (!req->schema){ ramble_request_fail(req, "untyped request refused", ramble_bytes(NULL, 0)); return; }
    ramble_schema_message_default(add_rsp_s, out, sizeof out);
    ramble_set_int(out, sizeof out, add_rsp_s, "sum", x + y);
    ramble_request_reply(req, ramble_bytes(out, ramble_schema_size(add_rsp_s)));
}
/* server: the files/transfer task in the superloop shape. The handler only defers and
 * returns fast, and main's own loop does one chunk per pass through the token. */
static RambleFunction *task_def;
static struct { volatile uint64_t token; volatile int active; uint32_t sent, total; } g_xfer;
static void transfer_handler(RambleRequest *req, void *user){
    (void)user;
    g_xfer.total = rd32(req->data); if (!g_xfer.total) g_xfer.total = 8;
    g_xfer.sent  = 0;
    g_xfer.token = ramble_request_defer(req);   /* implies RUNNING to the caller */
    g_xfer.active = g_xfer.token != 0;
}
static void run_transfer_step(void){          /* one pass of the app's own loop */
    uint8_t chunk[4];
    if (!g_xfer.active) return;
    if (ramble_function_cancelled(task_def, g_xfer.token) == 1){
        ramble_function_complete(task_def, g_xfer.token, RAMBLE_CALL_CANCELLED, "stopped",
                               ramble_bytes(NULL, 0));
        g_xfer.active = 0;
        return;
    }
    wr32(chunk, ++g_xfer.sent);
    ramble_function_progress(task_def, g_xfer.token, ramble_bytes(chunk, 4));
    if (g_xfer.sent >= g_xfer.total){
        wr32(chunk, g_xfer.sent);
        ramble_function_complete(task_def, g_xfer.token, RAMBLE_CALL_OK, "transfer complete",
                               ramble_bytes(chunk, 4));
        g_xfer.active = 0;
    }
}
/* client: watch progress and log the outcome. The first update is the empty RUNNING ack. */
static uint32_t g_xfer_call;
static void on_task_progress(const RambleProgress *p){
    if (p->data.len) printf("  task  files/transfer  chunk %u\n", rd32(p->data));
    else             printf("  task  files/transfer  running (call %u)\n", p->call_id);
}
static void on_task_reply(const RambleResponse *r){
    printf("  task  files/transfer -> %s (%.*s)\n",
           r->status == RAMBLE_CALL_OK ? "ok"
           : r->status == RAMBLE_CALL_CANCELLED ? "cancelled" : "failed",
           (int)r->message.len, r->message.data);
}
/* client: log every call result and every plain message */
static void on_reply(const RambleResponse *r){
    if (r->status == RAMBLE_CALL_OK && r->schema)
        printf("  call  compute/add -> sum=%lld\n",
               (long long)ramble_get_int(r->data, r->schema, "sum"));
    else
        printf("  call  compute/add failed (status %d)\n", (int)r->status);
}
static void on_lidar(const RambleMsg *m){
    printf("  topic sensors/lidar  #%u\n", rd32(m->data));
}
/* both nodes: every lifecycle event and error on one line. ev->user is the label passed
 * as user_data, and ev->kind == RAMBLE_ERROR is the one "did something break" test. */
static void on_node_event(const RambleEvent *ev){
    char text[192];
    printf("  event [%s%s] %s\n", (const char*)ev->user,
           ev->kind == RAMBLE_ERROR ? " ERROR" : "", ramble_event_str(ev, text, sizeof text));
}

int main(int argc, char **argv){
    uint16_t domain = argc > 1 ? (uint16_t)atoi(argv[1]) : 0;
    RambleAllocator sa = ramble_allocator_heap(0);
    RambleAllocator ca = ramble_allocator_heap(0);
    RambleNode *server, *client;
    RambleFunction *fn_def, *fn_remote, *task_remote;
    RambleVariable *var_def, *var_remote;
    RambleTopic    *lidar_pub;
    uint32_t tick = 0;
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep output visible under a redirect or on Ctrl-C */

    server = ramble_node_open(&sa, "robot-server", NULL, on_node_event,
                            &(RambleNodeOpts){ .domain = domain, .user_data = (void*)"server" });
    client = ramble_node_open(&ca, "robot-client", on_lidar, on_node_event,
                            &(RambleNodeOpts){ .domain = domain, .user_data = (void*)"client" });
    if (!server || !client){ fprintf(stderr, "node open failed\n"); return 1; }

    /* typed entities: the explorer shows these shapes and its forms fill them */
    add_req_s = ramble_schema_compile(ramble_heap_realloc, NULL, "AddRequest { x: i32, y: i32 }", NULL);
    add_rsp_s = ramble_schema_compile(ramble_heap_realloc, NULL, "AddResult { sum: i32 }", NULL);
    temp_s    = ramble_schema_compile(ramble_heap_realloc, NULL, "Temperature { celsius: u32 }", NULL);
    if (!add_req_s || !add_rsp_s || !temp_s){
        fprintf(stderr, "schema compile failed\n"); return 1;
    }

    /* server side: the definitions, where the function body and the variable storage live */
    fn_def    = ramble_node_create_function_definition(server, "compute/add", add_req_s, add_rsp_s,
                                          add_handler, NULL, NULL);
    task_def  = ramble_node_create_task_definition(server, "files/transfer", NULL, NULL, NULL,
                                          transfer_handler, NULL, NULL);
    var_def   = ramble_node_create_variable_definition(server, "state/temperature", temp_s,
                              &(RambleVariableOpts){ .allow_force = 1 });
    lidar_pub = ramble_node_create_topic(server, "sensors/lidar", RAMBLE_PUB_ONLY, NULL, NULL);

    /* client side: remotes, references to the server's definitions */
    fn_remote = ramble_node_create_remote_function(client, "compute/add", add_req_s, add_rsp_s, NULL);
    task_remote= ramble_node_create_remote_task(client, "files/transfer", NULL, NULL, NULL, NULL);
    var_remote= ramble_node_create_remote_variable(client, "state/temperature", temp_s, NULL);
    ramble_node_create_topic(client, "sensors/lidar", RAMBLE_SUB_ONLY, NULL, NULL);
    if (!fn_def || !task_def || !var_def || !lidar_pub || !fn_remote || !task_remote || !var_remote){
        fprintf(stderr, "pattern setup failed\n"); return 1;
    }

    signal(SIGINT, on_sigint);
    ramble_node_start(server);
    ramble_node_start(client);
    printf("pattern_demo running on domain %u (server + client).\n"
           "Open the explorer:  ramble_explorer%s\n"
           "Ctrl-C to quit.\n\n", domain, domain ? " --domain N" : "");

    while (g_run){
        uint8_t buf[16], raw[4];
        ++tick;

        /* function request and response: a typed AddRequest, the reply decoded in on_reply */
        ramble_schema_message_default(add_req_s, buf, sizeof buf);
        ramble_set_int(buf, sizeof buf, add_req_s, "x", (int64_t)tick);
        ramble_set_int(buf, sizeof buf, add_req_s, "y", 1000);
        ramble_function_call_async(fn_remote, ramble_bytes(buf, ramble_schema_size(add_req_s)), on_reply, NULL, NULL);

        ramble_variable_set(var_def, enc_u32(temp_s, "celsius", tick, buf, sizeof buf));

        wr32(raw, tick);                                   /* plain topic: raw bytes */
        ramble_topic_send(lidar_pub, ramble_bytes(raw, 4), NULL);

        run_transfer_step();     /* task superloop, server side: one chunk per pass */

        /* task client side: start a long transfer, lose patience, then a short one */
        if (tick == 2){
            wr32(raw, 40);       /* 40 chunks: too long, cancelled below */
            ramble_function_call_async(task_remote, ramble_bytes(raw, 4), on_task_reply, NULL,
                &(RambleCallOpts){ .on_progress = on_task_progress, .id_out = &g_xfer_call });
        }
        if (tick == 6){
            printf("  -- cancelling the transfer --\n");
            ramble_function_cancel(task_remote, g_xfer_call);
        }
        if (tick == 9){
            wr32(raw, 5);        /* 5 chunks: runs to completion */
            ramble_function_call_async(task_remote, ramble_bytes(raw, 4), on_task_reply, NULL,
                &(RambleCallOpts){ .on_progress = on_task_progress });
        }

        if (tick % 4 == 0){
            RambleBytes cur;
            if (ramble_variable_get(var_remote, &cur))
                printf("  var   state/temperature = %llu%s\n",
                       (unsigned long long)ramble_get_uint(cur, temp_s, "celsius"),
                       ramble_variable_forced(var_remote) ? "  (forced)" : "");
        }
        if (tick == 8){
            ramble_variable_force(var_def, enc_u32(temp_s, "celsius", 999, buf, sizeof buf));
            printf("  -- forced temperature to 999 --\n");
        }
        if (tick == 16){ ramble_variable_unforce(var_def); printf("  -- unforced temperature --\n"); }
        sleep_ms(500);
    }

    printf("\nshutting down...\n");
    ramble_node_close(server, 1);
    ramble_node_close(client, 1);
    return 0;
}
