/* Minimal node: type a line, it publishes on topic "msg". Lines from peers print.
 * Run two copies (one host, or two on a LAN) and type in each. All defaults:
 * best-effort, domain 0, unicast data, multicast discovery.
 *   POSIX  : cc  -std=c99 -Idist examples/example.c -o node -lrt
 *   Windows: gcc -std=c99 -Idist examples/example.c -o example.exe -lws2_32 -lbcrypt -lwinmm */
#define DART_TRANSPORT_IMPLEMENTATION
#include "dart_transport.h"

#include <stdio.h>
#include <string.h>

/* a non-blocking "is a line waiting?" check, so the poll loop never stalls on input */
#ifdef _WIN32
#include <conio.h>
static int input_ready(void){ return _kbhit(); }
#else
#include <unistd.h>
#include <sys/select.h>
static int input_ready(void){
    fd_set r; struct timeval t; FD_ZERO(&r); FD_SET(0, &r); t.tv_sec = 0; t.tv_usec = 0;
    return select(1, &r, NULL, NULL, &t) > 0;
}
#endif

static void on_message(void *u, uint16_t ch, uint32_t from, const void *data, size_t len){
    (void)u; (void)ch; (void)from;
    printf("> %.*s\n", (int)len, (const char *)data);
}

int main(int argc, char *argv[]){
    const char *ch_name = argc > 1 ? argv[1] : "msg";
    dart_channel_def ch  = { .name = ch_name, .qos = { .reliability = DART_RELIABLE } };
    dart_node_config cfg = { .channels = &ch, .n_channels = 1, .on_message = on_message };
    static uint8_t mem[1 << 20];

    dart_node *n = dart_node_open(mem, sizeof mem, &cfg);
    if (!n){ fprintf(stderr, "dart_node_open failed\n"); return 1; }

    printf("type a message and press enter (ctrl-d / ctrl-z to quit):\n");
    for (;;){
        dart_node_poll(n, 10);                 /* service discovery and the socket */
        if (input_ready()){
            char line[256];
            if (!fgets(line, sizeof line, stdin)) break;     /* EOF: quit */
            size_t len = strcspn(line, "\n");                /* drop the trailing newline */
            if (len) dart_node_send(n, 0, line, len);
        }
    }
    dart_node_close(n, 1);
    return 0;
}
