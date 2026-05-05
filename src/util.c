#include "util.h"

void die(const char *msg) {
    fprintf(stderr, "zxle: %s", msg);
    if (errno) fprintf(stderr, " (%s)", strerror(errno));
    fputc('\n', stderr);
    exit(1);
}

void wu16(FILE *f, uint16_t v) { fputc(v&0xFF,f); fputc((v>>8)&0xFF,f); }
void wu32(FILE *f, uint32_t v) { for (int i=0;i<4;i++) fputc((v>>(i*8))&0xFF,f); }
void wu64(FILE *f, uint64_t v) { for (int i=0;i<8;i++) fputc((v>>(i*8))&0xFF,f); }

uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1]<<8)); }
uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

void run(const char *cmd) {
    int rc = system(cmd);
    if (rc != 0) { fprintf(stderr, "zxle: command failed (%d): %s\n", rc, cmd); exit(1); }
}

int try_run(const char *cmd) { return system(cmd); }

long long fsize(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) die("stat");
    return (long long)st.st_size;
}

const char *basename_of(const char *p) {
    const char *s = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') s = q + 1;
    return s;
}

void buf_init(Buf *b) { b->p = NULL; b->n = 0; b->cap = 0; }
void buf_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }
void buf_reserve(Buf *b, size_t want) {
    if (b->cap >= want) return;
    size_t c = b->cap ? b->cap : 64;
    while (c < want) c *= 2;
    uint8_t *np = realloc(b->p, c);
    if (!np) die("buf realloc");
    b->p = np; b->cap = c;
}
void buf_append(Buf *b, const void *d, size_t n) {
    buf_reserve(b, b->n + n);
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
void buf_u8(Buf *b, uint8_t v)   { buf_append(b, &v, 1); }
void buf_u32(Buf *b, uint32_t v) {
    uint8_t t[4]; for (int i=0;i<4;i++) t[i] = (v>>(i*8))&0xFF;
    buf_append(b, t, 4);
}

uint8_t *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "fopen %s\n", path); die("fopen"); }
    if (fseek(f, 0, SEEK_END) != 0) die("fseek");
    long sz = ftell(f);
    if (sz < 0) die("ftell");
    rewind(f);
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) die("malloc");
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) die("fread whole");
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}
