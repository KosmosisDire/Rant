/* Runs one interop pair: starts the publisher, then the subscriber, and returns the
 * subscriber's exit code, or 1 when it has not exited within a minute. The publisher is
 * stopped either way. Usage: rant_interop_pair <publisher argv...> -- <subscriber argv...> */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <signal.h>
  #include <sys/wait.h>
  #include <time.h>
  #include <unistd.h>
#endif

#define PAIR_TIMEOUT_MS 60000

#ifdef _WIN32

typedef HANDLE Child;

/* argv as one command line, every argument quoted */
static Child start(char **argv){
    char line[4096] = "";
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    size_t k;
    for (; *argv; argv++){
        if (strlen(line) + strlen(*argv) + 4 >= sizeof line) return NULL;
        strcat(line, line[0] ? " \"" : "\"");
        strcat(line, *argv);
        strcat(line, "\"");
    }
    for (k = 1; line[k] && line[k] != '"'; k++)   /* the program path wants backslashes */
        if (line[k] == '/') line[k] = '\\';
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static int wait_exit(Child c, int timeout_ms){
    DWORD code = 1;
    if (WaitForSingleObject(c, (DWORD)timeout_ms) != WAIT_OBJECT_0) return -1;
    GetExitCodeProcess(c, &code);
    return (int)code;
}

static void stop(Child c){
    TerminateProcess(c, 1);
    WaitForSingleObject(c, INFINITE);
    CloseHandle(c);
}

#else

typedef pid_t Child;

static Child start(char **argv){
    pid_t pid = fork();
    if (pid == 0){ execvp(argv[0], argv); _exit(127); }
    return pid > 0 ? pid : 0;
}

static int wait_exit(Child c, int timeout_ms){
    int st, waited = 0;
    struct timespec ts = { 0, 10000000L };
    while (waitpid(c, &st, WNOHANG) == 0){
        if (waited >= timeout_ms) return -1;
        nanosleep(&ts, NULL);
        waited += 10;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static void stop(Child c){
    kill(c, SIGKILL);
    waitpid(c, NULL, 0);
}

#endif

int main(int argc, char **argv){
    int split, code;
    Child pub, sub;
    for (split = 1; split < argc && strcmp(argv[split], "--"); split++) {}
    if (split < 2 || split >= argc - 1){
        fprintf(stderr, "usage: rant_interop_pair <publisher argv...> -- <subscriber argv...>\n");
        return 2;
    }
    argv[split] = NULL;
    setvbuf(stdout, NULL, _IONBF, 0);
    pub = start(argv + 1);
    if (!pub){ printf("FAIL: the publisher did not start\n"); return 1; }
    sub = start(argv + split + 1);
    if (!sub){ printf("FAIL: the subscriber did not start\n"); stop(pub); return 1; }
    code = wait_exit(sub, PAIR_TIMEOUT_MS);
    if (code < 0){ printf("FAIL: the subscriber did not exit\n"); stop(sub); code = 1; }
#ifdef _WIN32
    else CloseHandle(sub);
#endif
    stop(pub);
    return code;
}
