/* Interactive node: manage topics at runtime by typing commands, then chat.
 *   sub    <topic>   subscribe   (start seeing messages on that topic)
 *   pub    <topic>   publish     (your typed lines get pushed to that topic)
 *   pubsub <topic>   do both
 *   drop   <topic>   stop pub and sub on that topic
 * Any other line is published to every topic you currently publish on.
 * Run two copies (one host, or two on a LAN) and type in each; pass a node name
 * (e.g. ./node alice) to label who a message came from, --verbose to print
 * discovery/transport events, and --if <ip> to pin multicast to a given interface
 * (rarely needed: the interface is auto-detected, but pin it on a multihomed host
 * where the wrong NIC is chosen). All defaults:
 * best-effort, domain 0, unicast data, multicast discovery.
 *
 * DART runs on its own thread so the main thread can read stdin with a normal
 * blocking fgets. The node is not internally locked, so a mutex guards every node
 * call: the poll thread holds it for each non-blocking tick, the input thread holds
 * it while creating channels / setting roles / sending.
 *   POSIX  : cc  -std=c99 -Idist examples/example.c -o node -lrt -lpthread
 *   Windows: gcc -std=c99 -Idist examples/example.c -o example.exe -lws2_32 -lbcrypt -lwinmm */
#define DART_IMPLEMENTATION
#include "dart.h"

#include <stdio.h>
#include <string.h>

/* tiny cross-platform thread + mutex + sleep shim (Windows / POSIX) */
#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION Mutex;
static void mutex_init(Mutex *m){ InitializeCriticalSection(m); }
static void mutex_lock(Mutex *m){ EnterCriticalSection(m); }
static void mutex_unlock(Mutex *m){ LeaveCriticalSection(m); }
static void sleep_ms(int ms){ Sleep(ms); }
typedef HANDLE Thread;
#define THREAD_RET DWORD WINAPI
static Thread thread_start(LPTHREAD_START_ROUTINE fn, void *arg){ return CreateThread(NULL, 0, fn, arg, 0, NULL); }
static void   thread_join(Thread t){ WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t Mutex;
static void mutex_init(Mutex *m){ pthread_mutex_init(m, NULL); }
static void mutex_lock(Mutex *m){ pthread_mutex_lock(m); }
static void mutex_unlock(Mutex *m){ pthread_mutex_unlock(m); }
static void sleep_ms(int ms){ usleep(ms * 1000); }
typedef pthread_t Thread;
#define THREAD_RET void *
static Thread thread_start(void *(*fn)(void *), void *arg){ pthread_t t; pthread_create(&t, NULL, fn, arg); return t; }
static void   thread_join(Thread t){ pthread_join(t, NULL); }
#endif

#define MAX_TOPICS 32

static Mutex        g_lock;            /* guards every dart_* node call */
static volatile int g_running = 1;     /* cleared on EOF to stop the poll thread */
static int          g_verbose = 0;     /* --verbose: print discovery/transport events */

/* One topic the user has touched. We track the pub/sub bits locally because the
 * transport role enum has no getter, and we keep the handle to send/re-role it.
 * Only the input thread touches this table, so it needs no lock of its own. */
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

/* Find the topic, or create it on the node the first time it's named.
 * Caller must hold g_lock (it touches the node via dart_node_create_channel). */
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
    mutex_lock(&g_lock);
    Topic *t = get_topic(n, name);
    if (t){
        t->pub = pub; t->sub = sub;
        dart_channel_set_role(t->ch, role_of(pub, sub));
    }
    mutex_unlock(&g_lock);
    if (!t) return;
    const char *s = pub && sub ? "pubsub" : pub ? "pub" : sub ? "sub" : "drop";
    printf("  [%s] %s\n", s, name);
}

static void on_message(const DartMsg *msg){
    printf("[%.*s] %.*s > %.*s\n", (int)msg->sender_name.len, msg->sender_name.data,
           (int)msg->channel_name.len, msg->channel_name.data,
           (int)msg->data.len, (const char *)msg->data.data);
}

static void on_event(const DartEvent *ev){
    char line[160];
    if (g_verbose) printf("  <event> %s\n", dart_event_str(ev, line, sizeof line));
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

/* Background thread: drive discovery and the socket while the main thread blocks on
 * stdin. Each tick is a non-blocking poll under the lock, then a short sleep so the
 * input thread can take the lock to send. */
static THREAD_RET poll_thread(void *arg){
    DartNode *n = (DartNode *)arg;
    while (g_running){
        mutex_lock(&g_lock);
        dart_node_poll(n, 0);          /* non-blocking: drain RX, service timers/TX */
        mutex_unlock(&g_lock);
        sleep_ms(1);
    }
    return 0;
}

int main(int argc, char **argv){
    const char *name = NULL;   /* optional node name; NULL => auto "node-XXXXXXXX" */
    const char *ifc  = NULL;   /* --if <ip>: pin multicast to this interface (multihomed hosts) */
    for (int i = 1; i < argc; i++){
        if      (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) g_verbose = 1;
        else if ((strcmp(argv[i], "--if") == 0 || strcmp(argv[i], "-i") == 0) && i + 1 < argc) ifc = argv[++i];
        else if (!name) name = argv[i];
    }
    DartAllocator mem = dart_allocator_dynamic(1 << 20);
    DartNode *n = dart_node_open(&mem, name, on_message, on_event,
                                 &(DartNodeOpts){ .max_channels = MAX_TOPICS,
                                                  .net = { .multicast_interface = ifc } });
    if (!n){ fprintf(stderr, "dart_node_open failed\n"); return 1; }

    printf("commands: sub <topic> | pub <topic> | pubsub <topic> | drop <topic>\n"
           "any other line is published to every topic you pub on. ctrl-d / ctrl-z to quit.\n"
           "%s", g_verbose ? "" : "(run with --verbose to print discovery/transport events)\n");

    mutex_init(&g_lock);
    Thread poller = thread_start(poll_thread, n);

    char line[256];
    while (fgets(line, sizeof line, stdin)){          /* normal blocking input */
        size_t len = strcspn(line, "\n");             /* drop the trailing newline */
        line[len] = '\0';
        if (!len) continue;
        if (handle_command(n, line)) continue;

        /* plain chat: publish to every topic we currently publish on */
        int sent = 0;
        mutex_lock(&g_lock);
        for (int i = 0; i < g_n_topics; i++)
            if (g_topics[i].pub){ dart_channel_send(g_topics[i].ch, dart_bytes(line, len)); sent++; }
        mutex_unlock(&g_lock);
        if (!sent) printf("  (no pub topic yet: try 'pub <topic>' or 'pubsub <topic>')\n");
    }

    g_running = 0;                                     /* EOF: stop the poll thread and exit */
    thread_join(poller);
    dart_node_close(n, 1);
    return 0;
}
