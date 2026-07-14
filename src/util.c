#include "util.h"
#include <pthread.h>

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

typedef struct {
    const char *cmd;
    int rc;
} ProbeJob;

static void *probe_worker(void *arg) {
    ProbeJob *j = (ProbeJob *)arg;
    j->rc = system(j->cmd);
    return NULL;
}

int try_run_parallel(const char **cmds, int n_cmds, int *rcs_out) {
    if (n_cmds <= 0) return 0;
    if (n_cmds == 1) { rcs_out[0] = system(cmds[0]); return 0; }
    pthread_t *th = malloc(sizeof(pthread_t) * (size_t)n_cmds);
    ProbeJob  *jobs = malloc(sizeof(ProbeJob) * (size_t)n_cmds);
    if (!th || !jobs) { free(th); free(jobs); return -1; }
    int spawned = 0;
    for (int i = 0; i < n_cmds; i++) {
        jobs[i].cmd = cmds[i];
        jobs[i].rc  = -1;
        if (pthread_create(&th[i], NULL, probe_worker, &jobs[i]) != 0) break;
        spawned++;
    }
    for (int i = 0; i < spawned; i++) pthread_join(th[i], NULL);
    /* Any probes we failed to spawn run serially after the joined batch. */
    for (int i = spawned; i < n_cmds; i++) jobs[i].rc = system(cmds[i]);
    for (int i = 0; i < n_cmds; i++) rcs_out[i] = jobs[i].rc;
    free(th); free(jobs);
    return 0;
}

long long fsize(const char *path) {
#ifdef _WIN32
    /* MinGW's plain stat maps to _stat64i32 (32-bit st_size); use the 64-bit
     * variant so files >2 GiB report correctly. */
    struct _stati64 st;
    if (_stati64(path, &st) != 0) die("stat");
    return (long long)st.st_size;
#else
    struct stat st;
    if (stat(path, &st) != 0) die("stat");
    return (long long)st.st_size;
#endif
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
void buf_u16(Buf *b, uint16_t v) {
    uint8_t t[2] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF) };
    buf_append(b, t, 2);
}
void buf_u32(Buf *b, uint32_t v) {
    uint8_t t[4]; for (int i=0;i<4;i++) t[i] = (v>>(i*8))&0xFF;
    buf_append(b, t, 4);
}
void buf_u64(Buf *b, uint64_t v) {
    uint8_t t[8]; for (int i=0;i<8;i++) t[i] = (uint8_t)((v>>(i*8))&0xFF);
    buf_append(b, t, 8);
}

uint8_t bucket_for_bytes(const uint8_t *p, size_t n) {
    if (n < 4) return 0;
    if (p[0] == 0x4D && p[1] == 0x5A) return 1;                  /* PE: "MZ" */
    if (p[0] == 0x7F && p[1] == 0x45 && p[2] == 0x4C && p[3] == 0x46)
        return 1;                                                /* ELF */
    return 0;
}

uint8_t *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "fopen %s\n", path); die("fopen"); }
    /* 64-bit seek/tell: plain ftell returns 32-bit long on Windows, capping
     * inputs at 2 GiB. */
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) die("fseek");
    long long sz = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END) != 0) die("fseek");
    long long sz = (long long)ftello(f);
#endif
    if (sz < 0) die("ftell");
    rewind(f);
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) die("malloc");
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) die("fread whole");
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}
