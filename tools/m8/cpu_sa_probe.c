// M8 step 2b -- CPU SA + LCP throughput baseline.
//
// Builds the suffix array (libsais) and the LCP array on a corpus sample.
// Used as the CPU comparator for GPU SA throughput, and as the SA source
// for the upcoming step 2c CPU LZ77 reference parser.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../../third_party/libsais/include/libsais.h"

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static long read_file(const char *path, uint8_t **buf, long cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (cap > 0 && sz > cap) sz = cap;
    *buf = (uint8_t *)malloc(sz);
    long got = (long)fread(*buf, 1, sz, f);
    fclose(f);
    return got;
}

int main(int argc, char **argv) {
    long target_mb = (argc >= 2) ? atol(argv[1]) : 10;
    long cap = target_mb * 1024L * 1024L;
    const char *files[] = {
        "tests/corpus/silesia/mozilla",
        "tests/corpus/silesia/webster",
        "tests/corpus/silesia/nci",
    };
    uint8_t *T = (uint8_t *)malloc(cap);
    long n = 0;
    for (size_t k = 0; k < sizeof(files)/sizeof(files[0]) && n < cap; k++) {
        uint8_t *part = NULL;
        long got = read_file(files[k], &part, cap - n);
        if (got <= 0) continue;
        memcpy(T + n, part, got);
        free(part);
        n += got;
        printf("loaded %s: %ld bytes (total %ld)\n", files[k], got, n);
    }
    if (n == 0) { fprintf(stderr, "no input\n"); return 1; }
    printf("input: %ld bytes (%.2f MB)\n", n, n / (1024.0*1024.0));

    int32_t *SA = (int32_t *)malloc((size_t)n * sizeof(int32_t));
    int32_t *PLCP = (int32_t *)malloc((size_t)n * sizeof(int32_t));
    int32_t *LCP  = (int32_t *)malloc((size_t)n * sizeof(int32_t));
    if (!SA || !PLCP || !LCP) { fprintf(stderr, "alloc fail\n"); return 2; }

    double t0 = now_sec();
    int32_t rc = libsais(T, SA, (int32_t)n, 0, NULL);
    double t1 = now_sec();
    if (rc != 0) { fprintf(stderr, "libsais fail: %d\n", rc); return 3; }
    double sa_ms = (t1 - t0) * 1000.0;
    double sa_mbps = (n / 1.0e6) / (t1 - t0);
    printf("SA   : %.1f ms  %.2f MB/s\n", sa_ms, sa_mbps);

    double t2 = now_sec();
    rc = libsais_plcp(T, SA, PLCP, (int32_t)n);
    double t3 = now_sec();
    if (rc != 0) { fprintf(stderr, "libsais_plcp fail: %d\n", rc); return 4; }
    double plcp_ms = (t3 - t2) * 1000.0;
    printf("PLCP : %.1f ms\n", plcp_ms);

    double t4 = now_sec();
    rc = libsais_lcp(PLCP, SA, LCP, (int32_t)n);
    double t5 = now_sec();
    if (rc != 0) { fprintf(stderr, "libsais_lcp fail: %d\n", rc); return 5; }
    double lcp_ms = (t5 - t4) * 1000.0;
    printf("LCP  : %.1f ms\n", lcp_ms);

    // Sanity check: SA[0..2] and a small LCP-walk-derived hint.
    printf("SA[0..4]   = %d %d %d %d %d\n", SA[0], SA[1], SA[2], SA[3], SA[4]);
    int64_t lcp_sum = 0;
    int32_t lcp_max = 0;
    for (long i = 1; i < n; i++) { lcp_sum += LCP[i]; if (LCP[i] > lcp_max) lcp_max = LCP[i]; }
    printf("LCP avg=%.1f max=%d (max-match length anywhere in input)\n",
           (double)lcp_sum / (double)(n - 1), lcp_max);

    double total_ms = sa_ms + plcp_ms + lcp_ms;
    double total_mbps = (n / 1.0e6) / (total_ms / 1000.0);
    printf("---\n");
    printf("total SA+PLCP+LCP: %.1f ms  %.2f MB/s\n", total_ms, total_mbps);

    free(T); free(SA); free(PLCP); free(LCP);
    return 0;
}
