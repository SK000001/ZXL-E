// M8 step 2c-prime -- optimal-parse LZ77 from SA.
//
// Replaces the greedy matcher in cpu_lz_probe.c with a DP-based optimal
// parser. For each position i, we precompute the LONGEST match (len, off)
// available via SA neighborhood walk, then run a backward DP:
//
//   cost[n] = 0
//   cost[i] = min(
//     LIT_COST(T[i]) + cost[i+1],                             // literal
//     MATCH_COST(L_i, off_i) + cost[i + L_i]   if L_i >= 3    // longest match here
//   )
//
// This is "semi-optimal" -- we only consider the longest match at each
// position, not the full set of (L, off) at each position. zstd-19's
// internal optimal parser considers multiple match lengths and repcodes.
// Semi-optimal is the cheap-first test: if it doesn't beat greedy by a
// healthy margin, full optimal won't close the gap either.
//
// Cost model: approximates zstd's FSE-encoded sequence cost.
//   LIT_COST   = 8 bits (uncompressed literal; ignores Huffman residual)
//   MATCH_COST = 4 + log2(matchLen) + log2(offset)            (bits)
//
// Output: writes match-stream as zstd ZSTD_Sequence array, runs through
// ZSTD_compressSequences, compares output to plain zstd -19.

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>
#include <math.h>
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

#define M8_MINMATCH 3

// ---- precompute longest match at each position via SA neighborhood walk ----
//
// match_len[i], match_off[i] = (length, offset) of longest match starting at
// position i with a back-reference into T[0..i-1]. length 0 means no match.

static void precompute_longest_match(
    long n, const int32_t *SA, const int32_t *ISA, const int32_t *LCP,
    int32_t *match_len, int32_t *match_off)
{
    for (long p = 0; p < n; p++) {
        int32_t r = ISA[p];
        int32_t best_len = 0;
        int32_t best_pos = -1;

        // Walk left.
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
        // Walk right.
        cur = INT32_MAX;
        for (int32_t rp = r + 1; rp < (int32_t)n; rp++) {
            int32_t l = LCP[rp];
            if (l < cur) cur = l;
            if (cur <= best_len) break;
            if (SA[rp] < (int32_t)p) {
                best_len = cur;
                best_pos = SA[rp];
            }
        }
        if (best_len + p > n) best_len = (int32_t)(n - p);
        if (best_len >= M8_MINMATCH && best_pos >= 0) {
            match_len[p] = best_len;
            match_off[p] = (int32_t)(p - best_pos);
        } else {
            match_len[p] = 0;
            match_off[p] = 0;
        }
    }
}

// ---- DP optimal parse ------------------------------------------------------
//
// Cost in 1/8 bit units (fixed-point to avoid floating ops in hot loop).

static inline uint32_t lit_cost_units(void) {
    return 8 * 8;  // 8 bits = 64 eighth-bit units
}

static inline uint32_t log2_u32(uint32_t x) {
    // floor(log2). Returns 0 for x<=1.
    if (x <= 1) return 0;
    return 31 - __builtin_clz(x);
}

static inline uint32_t match_cost_units(uint32_t mlen, uint32_t moff) {
    // ~ 4 + log2(mlen) + log2(moff) bits.
    uint32_t bits = 4 + log2_u32(mlen) + log2_u32(moff);
    return bits * 8;
}

typedef struct {
    uint32_t match_len;  // 0 = literal
    uint32_t match_off;
} choice_t;

// Backward DP; returns the parse as a list of (lit_run, match_len, match_off).
typedef struct {
    uint32_t lit_len;
    uint32_t match_len;  // 0 = tail-only
    uint32_t match_off;
} parse_record_t;

static long optimal_parse(
    long n,
    const int32_t *match_len_arr, const int32_t *match_off_arr,
    parse_record_t *recs, long recs_cap)
{
    // cost[i] = minimum encoding cost (1/8 bit units) for suffix T[i..n-1].
    uint64_t *cost = (uint64_t *)malloc(sizeof(uint64_t) * (size_t)(n + 1));
    choice_t  *choice = (choice_t *)malloc(sizeof(choice_t) * (size_t)(n + 1));
    if (!cost || !choice) { free(cost); free(choice); return -1; }

    cost[n] = 0;
    choice[n].match_len = 0;
    choice[n].match_off = 0;

    for (long i = n - 1; i >= 0; i--) {
        // Option 1: literal.
        uint64_t best = lit_cost_units() + cost[i + 1];
        uint32_t best_ml = 0, best_mo = 0;

        // Option 2: longest match at i (if exists).
        int32_t L  = match_len_arr[i];
        int32_t off = match_off_arr[i];
        if (L >= M8_MINMATCH) {
            uint64_t c = (uint64_t)match_cost_units((uint32_t)L, (uint32_t)off) + cost[i + L];
            if (c < best) {
                best = c;
                best_ml = (uint32_t)L;
                best_mo = (uint32_t)off;
            }
            // Also consider sub-lengths of the longest match: shorter matches
            // sometimes leave a better tail. Try a few discrete lengths.
            for (int32_t sub = L - 1; sub >= M8_MINMATCH; sub = sub / 2) {
                uint64_t cc = (uint64_t)match_cost_units((uint32_t)sub, (uint32_t)off) + cost[i + sub];
                if (cc < best) {
                    best = cc;
                    best_ml = (uint32_t)sub;
                    best_mo = (uint32_t)off;
                }
                if (sub == M8_MINMATCH) break;
            }
        }
        cost[i] = best;
        choice[i].match_len = best_ml;
        choice[i].match_off = best_mo;
    }

    // Reconstruct parse forward.
    long nrec = 0;
    long i = 0;
    long lit_start = 0;
    while (i < n) {
        if (choice[i].match_len == 0) {
            i += 1;
        } else {
            if (nrec >= recs_cap) { free(cost); free(choice); return -1; }
            recs[nrec].lit_len   = (uint32_t)(i - lit_start);
            recs[nrec].match_len = choice[i].match_len;
            recs[nrec].match_off = choice[i].match_off;
            nrec++;
            i += choice[i].match_len;
            lit_start = i;
        }
    }
    if (lit_start < n) {
        if (nrec >= recs_cap) { free(cost); free(choice); return -1; }
        recs[nrec].lit_len   = (uint32_t)(n - lit_start);
        recs[nrec].match_len = 0;
        recs[nrec].match_off = 0;
        nrec++;
    }
    free(cost); free(choice);
    return nrec;
}

// ---- main ------------------------------------------------------------------

int main(int argc, char **argv) {
    long arg = (argc >= 2) ? atol(argv[1]) : 100000;
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
    int32_t *ml   = (int32_t *)malloc((size_t)n * sizeof(int32_t));
    int32_t *mo   = (int32_t *)malloc((size_t)n * sizeof(int32_t));

    double t0 = now_sec();
    if (libsais(T, SA, (int32_t)n, 0, NULL) != 0) { fprintf(stderr, "libsais fail\n"); return 2; }
    libsais_plcp(T, SA, PLCP, (int32_t)n);
    libsais_lcp(PLCP, SA, LCP, (int32_t)n);
    for (long i = 0; i < n; i++) ISA[SA[i]] = (int32_t)i;
    double t1 = now_sec();
    printf("SA+ISA+LCP : %.0f ms\n", (t1 - t0) * 1000.0);

    double tm0 = now_sec();
    precompute_longest_match(n, SA, ISA, LCP, ml, mo);
    double tm1 = now_sec();
    printf("longest    : %.0f ms\n", (tm1 - tm0) * 1000.0);

    parse_record_t *recs = (parse_record_t *)malloc(sizeof(parse_record_t) * (size_t)(n + 1));
    double td0 = now_sec();
    long nrec = optimal_parse(n, ml, mo, recs, n + 1);
    double td1 = now_sec();
    if (nrec < 0) { fprintf(stderr, "optimal_parse fail\n"); return 3; }
    printf("optimal_dp : %.0f ms  (%ld records)\n", (td1 - td0) * 1000.0, nrec);

    uint64_t total_lit = 0, total_match = 0;
    long match_count = 0;
    for (long i = 0; i < nrec; i++) {
        total_lit += recs[i].lit_len;
        if (recs[i].match_len) { total_match += recs[i].match_len; match_count++; }
    }
    printf("breakdown  : literals=%llu match=%llu (matches=%ld, avg match-len=%.1f)\n",
           (unsigned long long)total_lit, (unsigned long long)total_match, match_count,
           match_count ? (double)total_match / (double)match_count : 0.0);

    // --- compress ---
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
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

    size_t bound = ZSTD_compressBound((size_t)n);
    uint8_t *zout = (uint8_t *)malloc(bound);
    double tz0 = now_sec();
    size_t zsize = ZSTD_compressSequences(cctx, zout, bound, seqs, (size_t)nrec, T, (size_t)n);
    double tz1 = now_sec();
    if (ZSTD_isError(zsize)) {
        fprintf(stderr, "compressSequences fail: %s\n", ZSTD_getErrorName(zsize));
        return 4;
    }
    printf("zstd-encode: %.0f ms  out=%zu bytes\n", (tz1 - tz0) * 1000.0, zsize);

    uint8_t *back = (uint8_t *)malloc((size_t)n);
    size_t got = ZSTD_decompress(back, (size_t)n, zout, zsize);
    if (ZSTD_isError(got) || got != (size_t)n || memcmp(back, T, n) != 0) {
        fprintf(stderr, "roundtrip FAIL\n");
        return 5;
    }
    printf("roundtrip  : OK\n");

    // --- zstd-19 reference ---
    ZSTD_CCtx *ref = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(ref, ZSTD_c_compressionLevel, 19);
    uint8_t *ref_out = (uint8_t *)malloc(bound);
    double tr0 = now_sec();
    size_t ref_size = ZSTD_compress2(ref, ref_out, bound, T, (size_t)n);
    double tr1 = now_sec();
    printf("zstd-19    : %.0f ms  out=%zu bytes\n", (tr1 - tr0) * 1000.0, ref_size);

    double m8_ratio = (double)zsize / (double)n;
    double ref_ratio = (double)ref_size / (double)n;
    double delta = (double)((long long)zsize - (long long)ref_size) / (double)ref_size * 100.0;
    printf("---\n");
    printf("M8 (opt)   : %.4f  (%zu / %ld)\n", m8_ratio, zsize, n);
    printf("zstd-19    : %.4f  (%zu / %ld)\n", ref_ratio, ref_size, n);
    printf("delta      : %+.2f%% (M8 vs zstd-19; negative = M8 smaller)\n", delta);

    ZSTD_freeCCtx(cctx); ZSTD_freeCCtx(ref);
    free(T); free(SA); free(ISA); free(PLCP); free(LCP); free(ml); free(mo);
    free(recs); free(seqs); free(zout); free(back); free(ref_out);
    return 0;
}
