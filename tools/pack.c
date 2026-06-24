/* amalgamate the split src/ layers into single-header distributables.
 *
 * Generates into dist/: dart_discovery.h, dart_transport.h, and dart.h (all
 * three). Concatenates source layers in dependency order, drops local include
 * lines, and wraps layers in feature guards.
 *
 * Flag scheme (<P> is DART_DISCOVERY or DART_TRANSPORT, DART for the combined
 * header maps onto both):
 *   <P>_IMPLEMENTATION   emit the implementation (define in exactly one TU)
 *   <P>_SANS_IO          strip the socket/runtime layer, leaving the core only
 *
 * Run: ./pack [srcdir outdir]  (defaults: src, dist)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Foldable section markers: VSCode (and others) collapse #pragma region blocks and
 * list them in the minimap. The guard silences -Wunknown-pragmas on the toolchains
 * (older gcc/clang) that don't recognize the markers; MSVC folds them natively. */
static void emit_region_guard(FILE *out){
    fputs("#if defined(__GNUC__)   /* let the section markers below fold quietly */\n"
          "#pragma GCC diagnostic ignored \"-Wunknown-pragmas\"\n"
          "#endif\n", out);
}

/* Copy srcdir/name into out, wrapped in a foldable #pragma region. If strip_local,
 * drop local #include "..." lines. */
static void emit(FILE *out, const char *srcdir, const char *name, int strip_local){
    char path[512]; char line[8192]; FILE *in;
    snprintf(path, sizeof path, "%s/%s", srcdir, name);
    in = fopen(path, "rb");
    if (!in){ fprintf(stderr, "pack: cannot open %s\n", path); exit(1); }
    fprintf(out, "#pragma region %s\n", name);
    while (fgets(line, sizeof line, in)){
        if (strip_local){
            const char *s = line;
            while (*s==' '||*s=='\t') s++;
            if (strncmp(s, "#include", 8)==0){
                const char *q = s+8;
                while (*q==' '||*q=='\t') q++;
                if (*q=='"') continue;
            }
        }
        fputs(line, out);
    }
    if (line[strlen(line)?strlen(line)-1:0] != '\n') fputc('\n', out);
    fputs("#pragma endregion\n", out);
    fclose(in);
}

static const char *BANNER =
"/* GENERATED single-header build. DO NOT EDIT.\n"
" * DART = Discovery And Realtime Transport. Amalgamated from src/ by\n"
" * tools/pack.c. Edit the split sources in src/ and re-run pack to regenerate.\n"
" * See the flag scheme at the top of tools/pack.c.\n"
" */\n";

/* POSIX feature-test preamble. Must precede the first system header so glibc
 * exposes the socket API. */
static void posix_preamble(FILE *out, const char *impl, const char *sansio){
    fprintf(out,
        "#if defined(%s) && !defined(%s) && !defined(_WIN32)\n"
        "  #ifndef _POSIX_C_SOURCE\n  #define _POSIX_C_SOURCE 200809L\n  #endif\n"
        "  #ifndef _DEFAULT_SOURCE\n  #define _DEFAULT_SOURCE 1\n  #endif\n"
        "#endif\n\n", impl, sansio);
}

/* dart_discovery.h: discovery core plus runtime, one header. */
static void build_discovery(const char *srcdir, const char *outdir){
    char path[512]; FILE *out;
    snprintf(path, sizeof path, "%s/dart_discovery.h", outdir);
    out = fopen(path, "wb");
    if (!out){ fprintf(stderr, "pack: cannot write %s\n", path); exit(1); }
    fputs(BANNER, out);
    emit_region_guard(out);
    posix_preamble(out, "DART_DISCOVERY_IMPLEMENTATION", "DART_DISCOVERY_SANS_IO");

    emit(out, srcdir, "discovery/core.h", 1);
    fputs("\n#ifndef DART_DISCOVERY_SANS_IO\n", out);
    emit(out, srcdir, "platform/core.h", 1);
    emit(out, srcdir, "discovery/runtime.h", 1);
    fputs("#endif /* !DART_DISCOVERY_SANS_IO */\n", out);

    fputs("\n#ifdef DART_DISCOVERY_IMPLEMENTATION\n", out);
    emit(out, srcdir, "common/bytes.h", 1);   /* shared LE helpers, before first use */
    emit(out, srcdir, "discovery/core.c", 1);
    fputs("\n#ifndef DART_DISCOVERY_SANS_IO\n", out);
    emit(out, srcdir, "platform/core.c", 1);
    emit(out, srcdir, "discovery/runtime.c", 1);
    fputs("#endif /* !DART_DISCOVERY_SANS_IO */\n", out);
    fputs("#endif /* DART_DISCOVERY_IMPLEMENTATION */\n", out);
    fclose(out);
    printf("wrote %s\n", path);
}

/* dart_transport.h: transport core plus node runtime, one header. The node
 * impl needs discovery, so this header includes the sibling dart_discovery.h. */
static void build_transport(const char *srcdir, const char *outdir){
    char path[512]; FILE *out;
    snprintf(path, sizeof path, "%s/dart_transport.h", outdir);
    out = fopen(path, "wb");
    if (!out){ fprintf(stderr, "pack: cannot write %s\n", path); exit(1); }
    fputs(BANNER, out);
    emit_region_guard(out);
    posix_preamble(out, "DART_TRANSPORT_IMPLEMENTATION", "DART_TRANSPORT_SANS_IO");

    /* bridge DART_TRANSPORT_* flags to the bundled discovery and pull it in. */
    fputs(
        "#ifndef DART_TRANSPORT_SANS_IO\n"
        "  #if defined(DART_TRANSPORT_IMPLEMENTATION) && !defined(DART_DISCOVERY_IMPLEMENTATION)\n"
        "  #define DART_DISCOVERY_IMPLEMENTATION\n"
        "  #endif\n"
        "  #include \"dart_discovery.h\"   /* discovery: needed by the node runtime */\n"
        "#endif\n\n", out);

    emit(out, srcdir, "transport/core.h", 1);
    fputs("\n#ifndef DART_TRANSPORT_SANS_IO\n", out);
    emit(out, srcdir, "node/core.h", 1);
    emit(out, srcdir, "shm/core.h", 1);    /* SHM module (inert without DART_SHM) */
    fputs("#endif /* !DART_TRANSPORT_SANS_IO */\n", out);

    fputs("\n#ifdef DART_TRANSPORT_IMPLEMENTATION\n", out);
    emit(out, srcdir, "common/bytes.h", 1);   /* shared LE helpers, before first use */
    emit(out, srcdir, "common/arena.h", 1);   /* shared bump allocator, before first use */
    emit(out, srcdir, "transport/internal.h", 1);  /* split transport: shared decls first */
    emit(out, srcdir, "transport/wire.c", 1);
    emit(out, srcdir, "transport/sched.c", 1);
    emit(out, srcdir, "transport/writer.c", 1);
    emit(out, srcdir, "transport/reader.c", 1);
    emit(out, srcdir, "transport/core.c", 1);
    fputs("\n#ifndef DART_TRANSPORT_SANS_IO\n", out);
    emit(out, srcdir, "shm/core.c", 1);    /* SHM module impl, before node.c uses it */
    emit(out, srcdir, "node/core.c", 1);
    fputs("#endif /* !DART_TRANSPORT_SANS_IO */\n", out);
    fputs("#endif /* DART_TRANSPORT_IMPLEMENTATION */\n", out);
    fclose(out);
    printf("wrote %s\n", path);
}

/* dart.h: discovery, transport, and node all inlined into one file. */
static void build_combined(const char *srcdir, const char *outdir){
    char path[512]; FILE *out;
    snprintf(path, sizeof path, "%s/dart.h", outdir);
    out = fopen(path, "wb");
    if (!out){ fprintf(stderr, "pack: cannot write %s\n", path); exit(1); }
    fputs(BANNER, out);
    emit_region_guard(out);

    /* DART_* is the one knob users touch. Map it onto the per-module flags. */
    fputs(
        "#ifdef DART_IMPLEMENTATION\n"
        "  #ifndef DART_DISCOVERY_IMPLEMENTATION\n  #define DART_DISCOVERY_IMPLEMENTATION\n  #endif\n"
        "  #ifndef DART_TRANSPORT_IMPLEMENTATION\n  #define DART_TRANSPORT_IMPLEMENTATION\n  #endif\n"
        "#endif\n"
        "#ifdef DART_SANS_IO\n"
        "  #ifndef DART_DISCOVERY_SANS_IO\n  #define DART_DISCOVERY_SANS_IO\n  #endif\n"
        "  #ifndef DART_TRANSPORT_SANS_IO\n  #define DART_TRANSPORT_SANS_IO\n  #endif\n"
        "#endif\n\n", out);

    posix_preamble(out, "DART_DISCOVERY_IMPLEMENTATION", "DART_DISCOVERY_SANS_IO");

    /* APIs in dependency order */
    emit(out, srcdir, "discovery/core.h", 1);
    fputs("\n#ifndef DART_DISCOVERY_SANS_IO\n", out);
    emit(out, srcdir, "platform/core.h", 1);
    emit(out, srcdir, "discovery/runtime.h", 1);
    fputs("#endif /* !DART_DISCOVERY_SANS_IO */\n", out);
    emit(out, srcdir, "transport/core.h", 1);
    fputs("\n#ifndef DART_TRANSPORT_SANS_IO\n", out);
    emit(out, srcdir, "node/core.h", 1);
    emit(out, srcdir, "shm/core.h", 1);    /* SHM module (inert without DART_SHM) */
    fputs("#endif /* !DART_TRANSPORT_SANS_IO */\n", out);

    /* implementations in dependency order */
    fputs("\n#ifdef DART_DISCOVERY_IMPLEMENTATION\n", out);
    emit(out, srcdir, "common/bytes.h", 1);   /* shared LE helpers, before first use */
    emit(out, srcdir, "discovery/core.c", 1);
    fputs("\n#ifndef DART_DISCOVERY_SANS_IO\n", out);
    emit(out, srcdir, "platform/core.c", 1);
    emit(out, srcdir, "discovery/runtime.c", 1);
    fputs("#endif /* !DART_DISCOVERY_SANS_IO */\n", out);
    fputs("#endif /* DART_DISCOVERY_IMPLEMENTATION */\n", out);

    fputs("\n#ifdef DART_TRANSPORT_IMPLEMENTATION\n", out);
    emit(out, srcdir, "common/bytes.h", 1);   /* shared LE helpers, before first use */
    emit(out, srcdir, "common/arena.h", 1);   /* shared bump allocator, before first use */
    emit(out, srcdir, "transport/internal.h", 1);  /* split transport: shared decls first */
    emit(out, srcdir, "transport/wire.c", 1);
    emit(out, srcdir, "transport/sched.c", 1);
    emit(out, srcdir, "transport/writer.c", 1);
    emit(out, srcdir, "transport/reader.c", 1);
    emit(out, srcdir, "transport/core.c", 1);
    fputs("\n#ifndef DART_TRANSPORT_SANS_IO\n", out);
    emit(out, srcdir, "shm/core.c", 1);    /* SHM module impl, before node.c uses it */
    emit(out, srcdir, "node/core.c", 1);
    fputs("#endif /* !DART_TRANSPORT_SANS_IO */\n", out);
    fputs("#endif /* DART_TRANSPORT_IMPLEMENTATION */\n", out);
    fclose(out);
    printf("wrote %s\n", path);
}

int main(int argc, char **argv){
    const char *srcdir = argc>1 ? argv[1] : "src";
    const char *outdir = argc>2 ? argv[2] : "dist";
    build_discovery(srcdir, outdir);
    build_transport(srcdir, outdir);
    build_combined(srcdir, outdir);
    printf("pack: done (%s -> %s)\n", srcdir, outdir);
    return 0;
}
