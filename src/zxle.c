/* ZXL-E (M1 walking skeleton)
 *
 * pack:   zxle pack   <out.zxle> <files...>
 * unpack: zxle unpack <in.zxle>  <outdir>
 *
 * M1 format: opaque solid zstd-19 payload + manifest. No format-aware
 * unwrap yet. Round-trip is byte-identical for the input files; rebuilding
 * structured containers (ZIP, etc.) byte-identical from their entries is
 * an M2 problem.
 *
 * Container layout:
 *   "ZXLE" (4)  version (1)  flags (1)  manifest_size (4 LE)
 *   manifest: for each entry: path_len (2 LE) path (utf-8) size (8 LE) mode (4 LE)
 *   zstd-19 payload (decompresses to concatenation of file bytes in manifest order)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#define ZXLE_MKDIR(p) _mkdir(p)
#else
#define ZXLE_MKDIR(p) mkdir((p), 0755)
#endif

#define ZXLE_MAGIC "ZXLE"
#define ZXLE_VER 1

static void die(const char *msg) {
    fprintf(stderr, "zxle: %s", msg);
    if (errno) fprintf(stderr, " (%s)", strerror(errno));
    fputc('\n', stderr);
    exit(1);
}

static void wu16(FILE *f, uint16_t v) { fputc(v&0xFF,f); fputc((v>>8)&0xFF,f); }
static void wu32(FILE *f, uint32_t v) { for (int i=0;i<4;i++) fputc((v>>(i*8))&0xFF,f); }
static void wu64(FILE *f, uint64_t v) { for (int i=0;i<8;i++) fputc((v>>(i*8))&0xFF,f); }
static uint16_t ru16(FILE *f) { int a=fgetc(f),b=fgetc(f); if(b<0) die("eof"); return (uint16_t)(a|(b<<8)); }
static uint32_t ru32(FILE *f) { uint32_t v=0; for(int i=0;i<4;i++){int b=fgetc(f); if(b<0) die("eof"); v|=(uint32_t)b<<(i*8);} return v; }
static uint64_t ru64(FILE *f) { uint64_t v=0; for(int i=0;i<8;i++){int b=fgetc(f); if(b<0) die("eof"); v|=(uint64_t)b<<(i*8);} return v; }

static void run(const char *cmd) {
    int rc = system(cmd);
    if (rc != 0) { fprintf(stderr, "zxle: command failed (%d): %s\n", rc, cmd); exit(1); }
}

static long long fsize(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) die("stat");
    return (long long)st.st_size;
}

static void copy_n(FILE *src, FILE *dst, uint64_t n) {
    char buf[65536];
    while (n) {
        size_t want = n < sizeof(buf) ? (size_t)n : sizeof(buf);
        size_t got = fread(buf, 1, want, src);
        if (got == 0) die("fread");
        if (fwrite(buf, 1, got, dst) != got) die("fwrite");
        n -= got;
    }
}

static const char *basename_of(const char *p) {
    const char *s = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') s = q + 1;
    return s;
}

static int do_pack(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: zxle pack <out.zxle> <files...>\n"); return 1; }
    const char *out = argv[0];
    int n = argc - 1;
    char **files = argv + 1;

    char tmp_concat[1024], tmp_zst[1024];
    snprintf(tmp_concat, sizeof(tmp_concat), "%s.concat.tmp", out);
    snprintf(tmp_zst,    sizeof(tmp_zst),    "%s.zst.tmp",    out);

    typedef struct { const char *path; uint64_t size; uint32_t mode; } Entry;
    Entry *ents = calloc((size_t)n, sizeof(Entry));
    if (!ents) die("calloc");

    FILE *concat = fopen(tmp_concat, "wb");
    if (!concat) die("fopen concat");
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        struct stat st;
        if (stat(files[i], &st) != 0) { fprintf(stderr, "stat %s\n", files[i]); die("stat input"); }
        FILE *in = fopen(files[i], "rb");
        if (!in) { fprintf(stderr, "fopen %s\n", files[i]); die("fopen input"); }
        copy_n(in, concat, (uint64_t)st.st_size);
        fclose(in);
        ents[i].path = basename_of(files[i]);
        ents[i].size = (uint64_t)st.st_size;
        ents[i].mode = (uint32_t)st.st_mode;
        total += (uint64_t)st.st_size;
    }
    fclose(concat);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "zstd -19 --long=27 -q -f -o \"%s\" \"%s\"", tmp_zst, tmp_concat);
    run(cmd);

    size_t manifest_size = 0;
    for (int i = 0; i < n; i++) manifest_size += 2 + strlen(ents[i].path) + 8 + 4;

    FILE *o = fopen(out, "wb");
    if (!o) die("fopen out");
    fwrite(ZXLE_MAGIC, 1, 4, o);
    fputc(ZXLE_VER, o);
    fputc(0, o);
    wu32(o, (uint32_t)manifest_size);
    for (int i = 0; i < n; i++) {
        size_t plen = strlen(ents[i].path);
        wu16(o, (uint16_t)plen);
        fwrite(ents[i].path, 1, plen, o);
        wu64(o, ents[i].size);
        wu32(o, ents[i].mode);
    }
    FILE *zf = fopen(tmp_zst, "rb");
    if (!zf) die("fopen zst");
    char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), zf)) > 0)
        if (fwrite(buf, 1, got, o) != got) die("fwrite payload");
    fclose(zf);
    fclose(o);

    unlink(tmp_concat);
    unlink(tmp_zst);
    free(ents);

    long long osz = fsize(out);
    fprintf(stderr, "packed %d file(s), orig=%llu zxle=%lld ratio=%.4f\n",
            n, (unsigned long long)total, osz,
            total ? (double)osz / (double)total : 0.0);
    return 0;
}

static int do_unpack(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: zxle unpack <in.zxle> <outdir>\n"); return 1; }
    const char *in = argv[0];
    const char *outdir = argv[1];

    FILE *f = fopen(in, "rb");
    if (!f) die("fopen in");
    char magic[4];
    if (fread(magic,1,4,f) != 4 || memcmp(magic, ZXLE_MAGIC, 4) != 0) die("bad magic");
    int ver = fgetc(f), flags = fgetc(f);
    (void)flags;
    if (ver != ZXLE_VER) die("bad version");

    uint32_t mlen = ru32(f);

    typedef struct { char path[1024]; uint64_t size; uint32_t mode; } Entry;
    Entry *ents = NULL;
    int count = 0, cap = 0;
    long left = (long)mlen;
    while (left > 0) {
        if (count == cap) { cap = cap ? cap*2 : 16; ents = realloc(ents, (size_t)cap * sizeof(Entry)); if (!ents) die("realloc"); }
        uint16_t pl = ru16(f);
        if (pl >= sizeof(ents[0].path)) die("path too long");
        if (fread(ents[count].path, 1, pl, f) != pl) die("read path");
        ents[count].path[pl] = 0;
        ents[count].size = ru64(f);
        ents[count].mode = ru32(f);
        left -= 2 + pl + 8 + 4;
        count++;
    }

    char tmp_zst[1024], tmp_concat[1024];
    snprintf(tmp_zst,    sizeof(tmp_zst),    "%s.unpack.zst.tmp",    in);
    snprintf(tmp_concat, sizeof(tmp_concat), "%s.unpack.concat.tmp", in);

    FILE *zf = fopen(tmp_zst, "wb");
    if (!zf) die("fopen tmp zst");
    char buf[65536]; size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
        if (fwrite(buf, 1, got, zf) != got) die("fwrite tmp zst");
    fclose(zf);
    fclose(f);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "zstd -d -q -f -o \"%s\" \"%s\"", tmp_concat, tmp_zst);
    run(cmd);

    if (ZXLE_MKDIR(outdir) != 0 && errno != EEXIST) die("mkdir outdir");

    FILE *cf = fopen(tmp_concat, "rb");
    if (!cf) die("fopen tmp concat");
    for (int i = 0; i < count; i++) {
        char p[2048];
        snprintf(p, sizeof(p), "%s/%s", outdir, ents[i].path);
        FILE *of = fopen(p, "wb");
        if (!of) { fprintf(stderr, "fopen %s\n", p); die("fopen out"); }
        copy_n(cf, of, ents[i].size);
        fclose(of);
    }
    fclose(cf);

    unlink(tmp_zst);
    unlink(tmp_concat);
    free(ents);

    fprintf(stderr, "unpacked %d file(s) to %s\n", count, outdir);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "ZXL-E (M1 walking skeleton)\n"
        "  zxle pack   <out.zxle> <files...>\n"
        "  zxle unpack <in.zxle>  <outdir>\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    if (strcmp(argv[1], "pack")   == 0) return do_pack  (argc - 2, argv + 2);
    if (strcmp(argv[1], "unpack") == 0) return do_unpack(argc - 2, argv + 2);
    usage();
    return 1;
}
