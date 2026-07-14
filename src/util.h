#ifndef ZXLE_UTIL_H
#define ZXLE_UTIL_H

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
#define ZXLE_DEVNULL "NUL"
#else
#define ZXLE_MKDIR(p) mkdir((p), 0755)
#define ZXLE_DEVNULL "/dev/null"
#endif

void die(const char *msg);

void wu16(FILE *f, uint16_t v);
void wu32(FILE *f, uint32_t v);
void wu64(FILE *f, uint64_t v);

uint16_t r16(const uint8_t *p);
uint32_t r32(const uint8_t *p);

void run(const char *cmd);
int  try_run(const char *cmd);

/* Run N shell commands concurrently via system() in worker threads. Stores
 * each command's return code into rcs_out[i]. Returns 0 on success; -1 only
 * if no work was done (malloc failure for the worker arrays). On per-thread
 * failures the affected commands fall back to a serial system() call so the
 * rcs_out[] array is always fully populated. */
int try_run_parallel(const char **cmds, int n_cmds, int *rcs_out);

long long fsize(const char *path);
const char *basename_of(const char *p);
int zxle_ncpus(void);

uint8_t *read_whole_file(const char *path, size_t *out_len);

typedef struct {
    uint8_t *p;
    size_t   n;
    size_t   cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_reserve(Buf *b, size_t want);
void buf_append(Buf *b, const void *d, size_t n);
void buf_u8(Buf *b, uint8_t v);
void buf_u16(Buf *b, uint16_t v);
void buf_u32(Buf *b, uint32_t v);
void buf_u64(Buf *b, uint64_t v);

/* M6 v3: solid stream is split across N buckets (currently 2: bucket 0 main,
 * bucket 1 x86/BCJ). On decode side, Solids carries the decompressed bytes
 * and a per-bucket cursor; recipe ops carry a u8 bucket byte that selects
 * which one to consume from. */
#define ZXLE_NUM_BUCKETS 2
typedef struct {
    const uint8_t *p[ZXLE_NUM_BUCKETS];
    size_t         len[ZXLE_NUM_BUCKETS];
    size_t         pos[ZXLE_NUM_BUCKETS];
} Solids;

/* M6 v3: classify a payload into a solid bucket by its first few magic bytes.
 *   0 = main bucket  (text, mixed binary, already-compressed, etc.)
 *   1 = x86 bucket   (PE / ELF -- benefits from xz BCJ filter)
 * False positives still round-trip (BCJ is reversible) but lose a small ratio. */
uint8_t bucket_for_bytes(const uint8_t *p, size_t n);

#endif
