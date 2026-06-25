/* Interactive node: manage topics at runtime by typing commands, then chat.
 *   sub    <topic>   subscribe   (start seeing messages on that topic)
 *   pub    <topic>   publish     (your typed lines get pushed to that topic)
 *   pubsub <topic>   do both
 *   drop   <topic>   stop pub and sub on that topic
 * Any other line is published to every topic you currently publish on.
 * Run two copies (one host, or two on a LAN) and type in each; pass a node name as
 * argv[1] (e.g. ./node alice) to label who a message came from. All defaults:
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

#define MAX_TOPICS 32

/* One topic the user has touched. We track the pub/sub bits locally because the
 * transport role enum has no getter, and we keep the handle to send/re-role it. */
typedef struct {
    char         name[DART_TOPIC_NAME_MAX + 1];
    DartChannel *ch;
    int          pub;   /* 1 = we publish on this topic */
    int          sub;   /* 1 = we subscribe to this topic */
} Topic;

static Topic g_topics[MAX_TOPICS];
static int   g_n_topics = 0;

static DartRole role_of(int pub, int sub){
    if (pub && sub) return DART_PUBSUB;
    if (pub)        return DART_PUB_ONLY;
    if (sub)        return DART_SUB_ONLY;
    return DART_INACTIVE;
}

static Topic *find_topic(const char *name){
    for (int i = 0; i < g_n_topics; i++)
        if (strcmp(g_topics[i].name, name) == 0) return &g_topics[i];
    return NULL;
}

/* Find the topic, or create it on the node the first time it's named. */
static Topic *get_topic(DartNode *n, const char *name){
    Topic *t = find_topic(name);
    if (t) return t;
    if (g_n_topics == MAX_TOPICS){ printf("  (topic table full, max %d)\n", MAX_TOPICS); return NULL; }
    if (strlen(name) > DART_TOPIC_NAME_MAX){ printf("  (topic name too long)\n"); return NULL; }

    DartChannel *ch = dart_node_create_channel(n, name, DART_INACTIVE, NULL);
    if (!ch){ printf("  (create channel failed for '%s')\n", name); return NULL; }

    t = &g_topics[g_n_topics++];
    strcpy(t->name, name);
    t->ch = ch; t->pub = 0; t->sub = 0;
    return t;
}

/* Apply a pub/sub change to a topic and push the new role to the transport. */
static void set_role(DartNode *n, const char *name, int pub, int sub){
    Topic *t = get_topic(n, name);
    if (!t) return;
    t->pub = pub; t->sub = sub;
    dart_channel_set_role(t->ch, role_of(pub, sub));
    const char *s = pub && sub ? "pubsub" : pub ? "pub" : sub ? "sub" : "drop";
    printf("  [%s] %s\n", s, name);
}

static void on_message(const DartMsg *msg){
    /* sender_name comes from discovery (never on the wire) and is never NULL */
    printf("[%s] %s > %.*s\n", msg->sender_name, msg->channel_name, (int)msg->len, (const char *)msg->data);
}

/* Handle a command line. Returns 1 if it was a command, 0 if it's plain chat. */
static int handle_command(DartNode *n, char *line){
    char verb[16], topic[DART_TOPIC_NAME_MAX + 1];
    int got = sscanf(line, "%15s %64s", verb, topic);
    if (got < 1) return 0;

    if      (strcmp(verb, "sub")    == 0 && got == 2){ Topic *t = find_topic(topic); set_role(n, topic, t ? t->pub : 0, 1); }
    else if (strcmp(verb, "pub")    == 0 && got == 2){ Topic *t = find_topic(topic); set_role(n, topic, 1, t ? t->sub : 0); }
    else if (strcmp(verb, "pubsub") == 0 && got == 2){ set_role(n, topic, 1, 1); }
    else if (strcmp(verb, "drop")   == 0 && got == 2){ set_role(n, topic, 0, 0); }
    else return 0;
    return 1;
}

int main(int argc, char **argv){
    const char *name = argc > 1 ? argv[1] : NULL;   /* optional node name; NULL => auto "node-XXXXXXXX" */
    DartNode *n = dart_node_open(1 << 20, on_message,
                                 &(DartNodeOpts){ .name = name, .max_channels = MAX_TOPICS });
    if (!n){ fprintf(stderr, "dart_node_open failed\n"); return 1; }

    printf("commands: sub <topic> | pub <topic> | pubsub <topic> | drop <topic>\n"
           "any other line is published to every topic you pub on. ctrl-d / ctrl-z to quit.\n");

    for (;;){
        dart_node_poll(n, 10);                 /* service discovery and the socket */
        if (input_ready()){
            char line[256];
            if (!fgets(line, sizeof line, stdin)) break;     /* EOF: quit */
            size_t len = strcspn(line, "\n");                /* drop the trailing newline */
            line[len] = '\0';
            if (!len) continue;
            if (handle_command(n, line)) continue;

            /* plain chat: publish to every topic we currently publish on */
            int sent = 0;
            for (int i = 0; i < g_n_topics; i++)
                if (g_topics[i].pub){ dart_channel_send(g_topics[i].ch, line, len); sent++; }
            if (!sent) printf("  (no pub topic yet: try 'pub <topic>' or 'pubsub <topic>')\n");
        }
    }
    dart_node_close(n, 1);
    return 0;
}
