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

static void on_message(DartMsg *msg)
{
    // Access the fields of the DartMessage structure
    msg.channel_id // get channel id
    msg.sender_id // get sender id
    msg.channel_name // get channel name as string
    msg.channel_name_len // get channel name length
    msg.data // get message data
    msg.len // get message length
    //etc

    printf("%.*s > %.*s\n", (int)msg.channel_name_len, msg.channel_name, (int)msg.len, (const char *)msg.data);
}

int main(int argc, char *argv[]){
    const char *ch_name = argc > 1 ? argv[1] : "msg";
    
    DartNode *node = dart_node_open(1 << 20, on_message, { /*options here*/ });    
    if (!node){ fprintf(stderr, "dart_node_open failed\n"); return 1; }

    DartChannel *channel = dart_node_create_channel(node, ch_name, DART_PUBSUB, { /*options here*/ });
    
    printf("type a message and press enter (ctrl-d / ctrl-z to quit):\n");
    while (1)
    {
        dart_node_poll(node, 10);                 /* service discovery and the socket */
        if (input_ready())
        {
            char line[256];
            if (!fgets(line, sizeof line, stdin)) break;     /* EOF: quit */
            size_t len = strcspn(line, "\n");                /* drop the trailing newline */
            if (len) dart_channel_send(channel, line, len);
        }
    }
    dart_node_close(node, 1);
    return 0;
}
