/* The main of fuzz_<name>_replay: runs the target's seeds, then every file named or found
 * in a named folder. `--seeds <folder>` writes the seeds there to start a corpus. */
#include "fuzz.h"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <dirent.h>
  #include <sys/stat.h>
#endif

static long g_inputs;
static const char *g_seed_dir;

static void run_input(const uint8_t *data, size_t size){
    uint8_t *copy = (uint8_t *)malloc(size ? size : 1);   /* exact size, so ASan sees overreads */
    if (!copy) abort();
    if (size) memcpy(copy, data, size);
    LLVMFuzzerTestOneInput(copy, size);
    free(copy);
    g_inputs++;
}

static void write_seed(const uint8_t *data, size_t size){
    char path[1024];
    FILE *f;
    snprintf(path, sizeof path, "%s/seed-%04ld", g_seed_dir, g_inputs++);
    f = fopen(path, "wb");
    if (!f || fwrite(data, 1, size, f) != size){ fprintf(stderr, "cannot write %s\n", path); exit(1); }
    fclose(f);
}

static int run_file(const char *path){
    FILE *f = fopen(path, "rb");
    uint8_t *buf; long n;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (!buf || (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)){ fclose(f); free(buf); return 0; }
    fclose(f);
    run_input(buf, n > 0 ? (size_t)n : 0);
    free(buf);
    return 1;
}

/* a folder that does not exist holds no inputs */
static int run_path(const char *path){
#ifdef _WIN32
    char pattern[1024], file[1024];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 1;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return run_file(path);
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(file, sizeof file, "%s\\%s", path, fd.cFileName);
        if (!run_file(file)){ FindClose(h); return 0; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 1;
#else
    struct stat st;
    DIR *d;
    struct dirent *e;
    char file[1024];
    if (stat(path, &st) != 0) return 1;
    if (!S_ISDIR(st.st_mode)) return run_file(path);
    d = opendir(path);
    if (!d) return 0;
    while ((e = readdir(d)) != NULL){
        snprintf(file, sizeof file, "%s/%s", path, e->d_name);
        if (stat(file, &st) != 0 || S_ISDIR(st.st_mode)) continue;
        if (!run_file(file)){ closedir(d); return 0; }
    }
    closedir(d);
    return 1;
#endif
}

int main(int argc, char **argv){
    int i;
    if (argc == 3 && !strcmp(argv[1], "--seeds")){
        g_seed_dir = argv[2];
        fuzz_seeds(write_seed);
        printf("%ld seeds written to %s\n", g_inputs, g_seed_dir);
        return 0;
    }
    fuzz_seeds(run_input);
    for (i = 1; i < argc; i++){
        if (!run_path(argv[i])){ fprintf(stderr, "cannot read %s\n", argv[i]); return 1; }
    }
    printf("%ld inputs ran clean\n", g_inputs);
    return 0;
}
