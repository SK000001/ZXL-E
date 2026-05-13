// M8 step 2c+2d -- CPU end-to-end reference for the M8 architecture.
//
// Pipeline:
//   1. Build SA + ISA + LCP via libsais.
//   2. Greedy longest-match LZ77 emission by walking SA neighborhoods.
//   3. Feed match-stream to zstd via ZSTD_compressSequences (whole-input
//      offline API; the block-level seq-producer API bans LDM and limits
//      matches to 128 KB blocks).
//   4. Decode through standard zstd, verify byte-identical.
//   5. Compare output size against plain zstd -19 on the same input.
//
// Result (greedy parse, silesia mix mozilla+webster+nci, gcc -O3):
//
//   size     M8 (greedy)  zstd-19    delta
//   102400   0.1946       0.1589    +22.49%  M8 worse
//   130000   0.1656       0.1347    +22.94%
//   200000   0.3181       0.2965     +7.30%
//   1000000  0.6513       0.6371     +2.24%
//
// Interpretation: greedy parsing of SA matches loses to zstd-19 by 2-22%
// depending on input. zstd-19 uses optimal parsing (DP over a cost model).
// To match or beat zstd-19 we'd need optimal parse on top of our SA
// neighborhood walks. The GPU speed win is real but the ratio story
// requires optimal-parse work to be useful.
//
// Known multi-block bug: some inputs that span >1 zstd block (>128 KB)
// produce sequences the validator rejects. The 1000000-byte case happens
// to land on a clean boundary; 500000 and 1048576 do not. Diagnosis
// deferred to a later session -- likely an edge case in greedy at a
// specific text position, since the smoke test of the API itself passes
// (tools/m8/compress_seq_smoke.c). Optimal-parse rewrite will replace
// this matcher entirely so debugging this exact greedy is low-value.

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>
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

// --- Greedy SA-based LZ77 match-finder ---------------------------------------
//
// At input position p (text order), find the longest match starting at p
// against any prior occurrence in T[0..p-1]. Walk neighbors in SA order
// from rank ISA[p]; LCP between rank r and any nearby rank is the min of
// LCP[] entries traversed.

typedef struct {
    uint32_t lit_start;   // input position where the run of literals begins
    uint32_t lit_len;     // literals before the match
    uint32_t match_pos;   // input position where the match begins
    uint32_t match_len;   // length of match (>= MINMATCH or 0 for tail-only literals)
    uint32_t match_off;   // back-distance to copy source
} m8_record_t;

#define M8_MINMATCH 3

// Returns number of records emitted.
static long emit_lz77(const uint8_t *T, long n,
                      const int32_t *SA, const int32_t *ISA, const int32_t *LCP,
                      m8_record_t *recs, long recs_cap)
{
    long nrec = 0;
    long p = 0;
    long lit_start = 0;

    while (p < n) {
        int32_t r = ISA[p];
        int32_t best_len = 0;
        int32_t best_pos = -1;

        // Walk left in SA.
        {
            int32_t cur = INT32_MAX;
            for (int32_t rp = r - 1; rp >= 0; rp--) {
                int32_t l = LCP[rp + 1];
                if (l < cur) cur = l;
                if (cur <= best_len) break;
                if (SA[rp] < (int32_t)p) {
                    best_len = cur;
                    best_pos = SA[rp];
                }
            }
        }
        // Walk right in SA.
        {
            int32_t cur = INT32_MAX;
            for (int32_t rp = r + 1; rp < (int32_t)n; rp++) {
                int32_t l = LCP[rp];
                if (l < cur) cur = l;
                if (cur <= best_len) break;
                if (SA[rp] < (int32_t)p) {
                    best_len = cur;
                    best_pos = SA[rp];
                }
            }
        }
        // Clamp match length so it doesn't overshoot input.
        if (best_pos >= 0 && (long)best_pos + best_len > p) {
            // Self-overlap is fine in LZ77 but we still must not run past p+remaining.
        }
        if (best_len + p > n) best_len = (int32_t)(n - p);

        if (best_len >= M8_MINMATCH) {
            if (nrec >= recs_cap) return -1;
            recs[nrec].lit_start = (uint32_t)lit_start;
            recs[nrec].lit_len   = (uint32_t)(p - lit_start);
            recs[nrec].match_pos = (uint32_t)p;
            recs[nrec].match_len = (uint32_t)best_len;
            recs[nrec].match_off = (uint32_t)(p - best_pos);
            nrec++;
            p += best_len;
            lit_start = p;
        } else {
            p += 1;
        }
    }
    // Final tail-literal record (matchLength = 0).
    if (lit_start < n) {
        if (nrec >= recs_cap) return -1;
        recs[nrec].lit_start = (uint32_t)lit_start;
        recs[nrec].lit_len   = (uint32_t)(n - lit_start);
        recs[nrec].match_pos = (uint32_t)n;
        recs[nrec].match_len = 0;
        recs[nrec].match_off = 0;
        nrec++;
    }
    return nrec;
}

// --- main --------------------------------------------------------------------
// We use ZSTD_compressSequences() (whole-input, single-frame API) rather than
// ZSTD_registerSequenceProducer (which is block-level / 128 KB blocks and
// bans long-distance matching). For M8 we need a global parse across the
// whole input, which the offline API permits.

int main(int argc, char **argv) {
    // Argument: if it parses as an integer >= 1024, treat as bytes;
    // otherwise treat as MB.
    long arg = (argc >= 2) ? atol(argv[1]) : 10;
    long cap = (arg >= 1024) ? arg : (arg * 1024L * 1024L);
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
    }
    if (n == 0) { fprintf(stderr, "no input\n"); return 1; }
    printf("input: %ld bytes (%.2f MB)\n", n, n / (1024.0*1024.0));

    int32_t *SA   = (int32_t *)malloc((size_t)n * sizeof(int32_t));
    int32_t *ISA  = (int32_t *)malloc((size_t)n * sizeof(int32_t));
    int32_t *PLCP = (int32_t *)malloc((size_t)n * sizeof(int32_t));
    int32_t *LCP  = (int32_t *)malloc((size_t)n * sizeof(int32_t));

    double t_sa0 = now_sec();
    if (libsais(T, SA, (int32_t)n, 0, NULL) != 0) { fprintf(stderr, "libsais fail\n"); return 2; }
    libsais_plcp(T, SA, PLCP, (int32_t)n);
    libsais_lcp(PLCP, SA, LCP, (int32_t)n);
    for (long i = 0; i < n; i++) ISA[SA[i]] = (int32_t)i;
    double t_sa1 = now_sec();
    printf("SA+ISA+LCP : %.0f ms\n", (t_sa1 - t_sa0) * 1000.0);

    // Worst-case record count: one per byte (n + 1 for tail).
    m8_record_t *recs = (m8_record_t *)malloc(sizeof(m8_record_t) * (size_t)(n + 1));
    double t_lz0 = now_sec();
    long nrec;
    if (getenv("M8_ALL_LITERALS")) {
        recs[0].lit_start = 0;
        recs[0].lit_len   = (uint32_t)n;
        recs[0].match_pos = (uint32_t)n;
        recs[0].match_len = 0;
        recs[0].match_off = 0;
        nrec = 1;
    } else {
        nrec = emit_lz77(T, n, SA, ISA, LCP, recs, n + 1);
    }
    double t_lz1 = now_sec();
    if (nrec < 0) { fprintf(stderr, "emit_lz77 overflow\n"); return 3; }
    printf("emit_lz77  : %.0f ms  (%ld records, avg match-len = %.1f)\n",
           (t_lz1 - t_lz0) * 1000.0, nrec,
           nrec > 0 ? (double)n / (double)nrec : 0.0);

    // Bracket: average match-len / literals breakdown
    {
        uint64_t total_lit = 0, total_match = 0;
        long match_count = 0;
        for (long i = 0; i < nrec; i++) {
            total_lit += recs[i].lit_len;
            if (recs[i].match_len) { total_match += recs[i].match_len; match_count++; }
        }
        printf("breakdown  : literals=%llu match=%llu (matches=%ld, avg match-len=%.1f)\n",
               (unsigned long long)total_lit, (unsigned long long)total_match, match_count,
               match_count ? (double)total_match / (double)match_count : 0.0);
    }

    // --- compress via ZSTD_compressSequences (whole-input offline API) ---
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    if (!cctx) { fprintf(stderr, "createCCtx fail\n"); return 4; }

    // Build ZSTD_Sequence array from m8_record_t (one-to-one, identical fields).
    ZSTD_Sequence *seqs = (ZSTD_Sequence *)malloc(sizeof(ZSTD_Sequence) * (size_t)nrec);
    for (long i = 0; i < nrec; i++) {
        seqs[i].litLength   = recs[i].lit_len;
        seqs[i].matchLength = recs[i].match_len;
        seqs[i].offset      = recs[i].match_off;
        seqs[i].rep         = 0;
    }

    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 19);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_minMatch, M8_MINMATCH);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_validateSequences, 1);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_blockDelimiters, ZSTD_sf_noBlockDelimiters);
    ZSTD_CCtx_setPledgedSrcSize(cctx, (size_t)n);

    // Dump first/last few sequences for inspection.
    printf("seqs[0..3]: ");
    for (long i = 0; i < (nrec < 4 ? nrec : 4); i++)
        printf("(lit=%u match=%u off=%u) ", seqs[i].litLength, seqs[i].matchLength, seqs[i].offset);
    printf("\nseqs[N-2..N-1]: ");
    for (long i = (nrec >= 2 ? nrec - 2 : 0); i < nrec; i++)
        printf("(lit=%u match=%u off=%u) ", seqs[i].litLength, seqs[i].matchLength, seqs[i].offset);
    printf("\n");
    uint64_t sum_cov = 0;
    for (long i = 0; i < nrec; i++) sum_cov += seqs[i].litLength + seqs[i].matchLength;
    printf("sum(lit+match) = %llu vs n = %ld  (diff %lld)\n",
           (unsigned long long)sum_cov, n, (long long)((long long)sum_cov - n));

    size_t bound = ZSTD_compressBound((size_t)n);
    uint8_t *zout = (uint8_t *)malloc(bound);
    double t_z0 = now_sec();
    size_t zsize = ZSTD_compressSequences(cctx, zout, bound, seqs, (size_t)nrec, T, (size_t)n);
    double t_z1 = now_sec();
    if (ZSTD_isError(zsize)) {
        fprintf(stderr, "ZSTD_compress2 fail: %s\n", ZSTD_getErrorName(zsize));
        return 5;
    }
    printf("zstd-encode: %.0f ms  out=%zu bytes\n", (t_z1 - t_z0) * 1000.0, zsize);

    // --- roundtrip ---
    uint8_t *back = (uint8_t *)malloc((size_t)n);
    size_t got = ZSTD_decompress(back, (size_t)n, zout, zsize);
    if (ZSTD_isError(got) || got != (size_t)n || memcmp(back, T, n) != 0) {
        fprintf(stderr, "roundtrip FAIL: dec=%s got=%zu want=%ld\n",
                ZSTD_isError(got) ? ZSTD_getErrorName(got) : "ok",
                got, n);
        return 6;
    }
    printf("roundtrip  : OK\n");

    // --- plain zstd -19 reference ---
    ZSTD_CCtx *ref = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(ref, ZSTD_c_compressionLevel, 19);
    ZSTD_CCtx_setParameter(ref, ZSTD_c_windowLog, 27);
    ZSTD_CCtx_setParameter(ref, ZSTD_c_enableLongDistanceMatching, 1);
    uint8_t *ref_out = (uint8_t *)malloc(bound);
    double t_r0 = now_sec();
    size_t ref_size = ZSTD_compress2(ref, ref_out, bound, T, (size_t)n);
    double t_r1 = now_sec();
    printf("zstd-19    : %.0f ms  out=%zu bytes\n", (t_r1 - t_r0) * 1000.0, ref_size);

    // --- verdict ---
    double m8_ratio  = (double)zsize    / (double)n;
    double ref_ratio = (double)ref_size / (double)n;
    double delta = (double)((long long)zsize - (long long)ref_size) / (double)ref_size * 100.0;
    printf("---\n");
    printf("M8 ratio   : %.4f  (%zu / %ld)\n", m8_ratio, zsize, n);
    printf("zstd-19 ref: %.4f  (%zu / %ld)\n", ref_ratio, ref_size, n);
    printf("delta      : %+.2f%% (M8 vs zstd-19; negative = M8 smaller)\n", delta);

    ZSTD_freeCCtx(cctx); ZSTD_freeCCtx(ref);
    free(T); free(SA); free(ISA); free(PLCP); free(LCP);
    free(recs); free(seqs); free(zout); free(back); free(ref_out);
    return 0;
}
