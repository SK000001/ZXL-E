// Diagnostic: side-by-side comparison of v11 parse vs zstd-19 parse at 30 MB.
//
// Question: WHY is v11's output +2.36% larger than zstd-19's at 30 MB when both
// are valid LZ77 parses fed through ZSTD_compressSequences with the same level
// 19 entropy stage? Possible causes:
//   1. zstd's parse covers more bytes with matches (less literal volume)
//   2. zstd uses longer matches (each match covers more bytes / fewer LL+ML
//      transitions to encode)
//   3. zstd uses smaller / more-clustered offsets (cheaper FSE codes)
//   4. zstd uses more repcodes (cheap to encode)
//
// This probe runs v11's parser, also runs ZSTD_generateSequences on the same
// input, and prints distributional summaries.

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static long read_file_concat(const char *const *paths, int npaths, long cap, uint8_t **outbuf) {
    *outbuf = (uint8_t *)malloc(cap);
    long total = 0;
    for (int k = 0; k < npaths && total < cap; k++) {
        FILE *f = fopen(paths[k], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        long want = sz; if (cap - total < want) want = cap - total;
        fread(*outbuf + total, 1, want, f);
        fclose(f);
        total += want;
    }
    return total;
}

static inline int log2_u32(uint32_t x) { if (x <= 1) return 0; return 31 - __builtin_clz(x); }

static void summarize(const char *tag, const ZSTD_Sequence *seqs, size_t nseq, long n_input) {
    uint64_t lit_total = 0, match_total = 0, nrep = 0, nseq_eff = 0;
    uint64_t ml_hist[24] = {0}, of_hist[32] = {0};
    uint64_t ml_max = 0;
    for (size_t i = 0; i < nseq; i++) {
        lit_total += seqs[i].litLength;
        if (seqs[i].matchLength == 0) continue;
        nseq_eff++;
        match_total += seqs[i].matchLength;
        if (seqs[i].matchLength > ml_max) ml_max = seqs[i].matchLength;
        uint32_t mlc = log2_u32(seqs[i].matchLength);
        if (mlc >= 24) mlc = 23;
        ml_hist[mlc]++;
        uint32_t ofc = log2_u32(seqs[i].offset);
        if (ofc >= 32) ofc = 31;
        of_hist[ofc]++;
        if (seqs[i].rep) nrep++;
    }
    printf("\n== %s ==\n", tag);
    printf("  seqs total : %zu  (effective: %llu)\n", nseq, (unsigned long long)nseq_eff);
    printf("  literal B  : %llu  (%.2f%% of input)\n", (unsigned long long)lit_total, 100.0 * lit_total / (double)n_input);
    printf("  match B    : %llu  (%.2f%% of input)\n", (unsigned long long)match_total, 100.0 * match_total / (double)n_input);
    printf("  avg ML     : %.2f  (max %llu)\n", nseq_eff ? (double)match_total / (double)nseq_eff : 0.0, (unsigned long long)ml_max);
    printf("  rep matches: %llu  (%.2f%% of effective seqs)\n", (unsigned long long)nrep, nseq_eff ? 100.0 * nrep / (double)nseq_eff : 0.0);
    printf("  ML hist (count by log2(ml)):\n");
    for (int c = 0; c < 24; c++) {
        if (ml_hist[c] == 0) continue;
        printf("    [%2d: %d..%d) %12llu  (%.2f%%)\n", c, 1 << c, 1 << (c + 1),
               (unsigned long long)ml_hist[c], 100.0 * ml_hist[c] / (double)nseq_eff);
    }
    printf("  OF hist (count by log2(off)):\n");
    for (int c = 0; c < 32; c++) {
        if (of_hist[c] == 0) continue;
        printf("    [%2d: %d..%d) %12llu  (%.2f%%)\n", c, 1 << c, 1 << (c + 1),
               (unsigned long long)of_hist[c], 100.0 * of_hist[c] / (double)nseq_eff);
    }
}

int main(int argc, char **argv) {
    long want = (argc >= 2) ? atol(argv[1]) : 30000000;
    const char *paths[] = {
        "tests/corpus/silesia/mozilla",
        "tests/corpus/silesia/webster",
        "tests/corpus/silesia/nci",
    };
    uint8_t *T = NULL;
    long n = read_file_concat(paths, 3, want, &T);
    if (n == 0) { fprintf(stderr, "no input\n"); return 1; }
    printf("input: %ld bytes\n", n);

    // zstd-19's parse via deprecated debug API.
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 19);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 27);
    size_t bound = ZSTD_sequenceBound((size_t)n);
    ZSTD_Sequence *seqs = (ZSTD_Sequence *)malloc(sizeof(ZSTD_Sequence) * bound);
    size_t nseq = ZSTD_generateSequences(cctx, seqs, bound, T, (size_t)n);
    if (ZSTD_isError(nseq)) {
        fprintf(stderr, "ZSTD_generateSequences failed: %s\n", ZSTD_getErrorName(nseq));
        return 2;
    }
    summarize("zstd-19", seqs, nseq, n);

    free(seqs); free(T); ZSTD_freeCCtx(cctx);
    return 0;
}
