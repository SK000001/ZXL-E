// M8a step 1 -- optimal-parse with MULTI-LENGTH match candidates per position.
//
// v1 (cpu_lz_optimal.c) considered only the longest match per position plus a
// geometric L/2 walk. That left +1.55 to +14.05% gap to zstd-19.
//
// v2 collects every distinct (length, offset) tier encountered during the SA
// neighborhood walk -- as cur_lcp decreases monotonically when walking away
// from rank r, each drop creates a new length tier. For each tier we keep
// the smallest offset (smaller offset = cheaper to encode). DP then picks
// freely among (literal) and ALL candidate (length, offset) at each position.

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
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

#define M8_MINMATCH 3
#define M8_MAX_CANDS 8     // cap candidates per position to bound DP work
#define M8_WALK_LIMIT 32   // max SA-ranks visited in each direction per position

// Candidate (length, offset) at a single text position.
typedef struct {
    uint32_t len;
    uint32_t off;
} cand_t;

// Per-position candidate count and pointer into a packed candidate array.
typedef struct {
    uint32_t start;
    uint8_t  count;
} cand_index_t;

// Collect candidates by walking SA in both directions from rank ISA[p].
// Within each direction, cur_lcp is monotonically non-increasing. Every time
// cur_lcp drops we observe a new length tier; we keep the smallest offset
// (= smallest p - SA[rp]) seen at each tier. After both walks, we have a set
// of (length, smallest_offset) pairs covering distinct lengths.

static long collect_candidates(
    long n, const int32_t *SA, const int32_t *ISA, const int32_t *LCP,
    cand_index_t *idx, cand_t *pool, long pool_cap)
{
    long pool_used = 0;
    for (long p = 0; p < n; p++) {
        int32_t r = ISA[p];

        // Temp buffer for this position: indexed by candidate slot.
        cand_t local[M8_MAX_CANDS];
        int n_local = 0;

        // Helper macro: try to insert (len, off). Keep entries sorted descending
        // by length and dedupe lengths by keeping smaller offset.
        #define TRY_INSERT(LEN, OFF) do { \
            uint32_t _l = (uint32_t)(LEN); \
            uint32_t _o = (uint32_t)(OFF); \
            if (_l < M8_MINMATCH) break; \
            int _found = 0; \
            for (int _k = 0; _k < n_local; _k++) { \
                if (local[_k].len == _l) { \
                    if (_o < local[_k].off) local[_k].off = _o; \
                    _found = 1; break; \
                } \
            } \
            if (!_found && n_local < M8_MAX_CANDS) { \
                local[n_local].len = _l; \
                local[n_local].off = _o; \
                n_local++; \
            } \
        } while (0)

        // Track smallest collected length for early-exit.
        // Once the local table is full AND cur_lcp <= smallest collected length,
        // no more useful candidates can be inserted (no new length tier above
        // any existing tier; only chance is a smaller offset at an existing
        // tier, which we accept losing for the >100x speed win).
        uint32_t min_collected_len = UINT32_MAX;

        // Walk left (depth-bounded).
        {
            int32_t cur = INT32_MAX;
            int steps = 0;
            for (int32_t rp = r - 1; rp >= 0 && steps < M8_WALK_LIMIT; rp--, steps++) {
                int32_t l = LCP[rp + 1];
                if (l < cur) cur = l;
                if (cur < M8_MINMATCH) break;
                if (n_local >= M8_MAX_CANDS && (uint32_t)cur <= min_collected_len) break;
                if (SA[rp] < (int32_t)p) {
                    int32_t off = (int32_t)p - SA[rp];
                    TRY_INSERT(cur, off);
                    if (n_local > 0 && (uint32_t)cur < min_collected_len) min_collected_len = (uint32_t)cur;
                }
            }
        }
        // Walk right (depth-bounded).
        {
            int32_t cur = INT32_MAX;
            int steps = 0;
            for (int32_t rp = r + 1; rp < (int32_t)n && steps < M8_WALK_LIMIT; rp++, steps++) {
                int32_t l = LCP[rp];
                if (l < cur) cur = l;
                if (cur < M8_MINMATCH) break;
                if (n_local >= M8_MAX_CANDS && (uint32_t)cur <= min_collected_len) break;
                if (SA[rp] < (int32_t)p) {
                    int32_t off = (int32_t)p - SA[rp];
                    TRY_INSERT(cur, off);
                    if (n_local > 0 && (uint32_t)cur < min_collected_len) min_collected_len = (uint32_t)cur;
                }
            }
        }

        // Clamp lengths to remaining input.
        for (int k = 0; k < n_local; k++) {
            if (local[k].len + p > (uint32_t)n) {
                local[k].len = (uint32_t)(n - p);
            }
        }

        // Filter out length < MINMATCH after clamp, then dedupe again.
        int n_keep = 0;
        for (int k = 0; k < n_local; k++) {
            if (local[k].len < M8_MINMATCH) continue;
            int found = 0;
            for (int j = 0; j < n_keep; j++) {
                if (local[j].len == local[k].len) {
                    if (local[k].off < local[j].off) local[j].off = local[k].off;
                    found = 1; break;
                }
            }
            if (!found) { local[n_keep++] = local[k]; }
        }

        idx[p].start = (uint32_t)pool_used;
        idx[p].count = (uint8_t)n_keep;
        if (pool_used + n_keep > pool_cap) return -1;
        for (int k = 0; k < n_keep; k++) pool[pool_used++] = local[k];

        #undef TRY_INSERT
    }
    return pool_used;
}

// ---- DP optimal parse with multi-length candidates -------------------------

static inline uint32_t log2_u32(uint32_t x) {
    if (x <= 1) return 0;
    return 31 - __builtin_clz(x);
}

// Cost in 1/8 bit units. Literal=8b, match≈4+log2(matchLen)+log2(offset).
static inline uint32_t lit_cost(void)              { return 8 * 8; }
static inline uint32_t match_cost(uint32_t l, uint32_t o) { return (4 + log2_u32(l) + log2_u32(o)) * 8; }

typedef struct {
    uint32_t match_len;
    uint32_t match_off;
} choice_t;

typedef struct {
    uint32_t lit_len;
    uint32_t match_len;
    uint32_t match_off;
} parse_record_t;

static long optimal_parse_multi(
    long n,
    const cand_index_t *idx, const cand_t *pool,
    parse_record_t *recs, long recs_cap)
{
    uint64_t *cost = (uint64_t *)malloc(sizeof(uint64_t) * (size_t)(n + 1));
    choice_t *choice = (choice_t *)malloc(sizeof(choice_t) * (size_t)(n + 1));
    if (!cost || !choice) { free(cost); free(choice); return -1; }

    cost[n] = 0;
    choice[n].match_len = 0;
    choice[n].match_off = 0;

    for (long i = n - 1; i >= 0; i--) {
        uint64_t best = (uint64_t)lit_cost() + cost[i + 1];
        uint32_t best_ml = 0, best_mo = 0;

        const cand_t *c = &pool[idx[i].start];
        for (int k = 0; k < idx[i].count; k++) {
            uint32_t L  = c[k].len;
            uint32_t O  = c[k].off;
            if ((long)i + L > n) continue;
            uint64_t cc = (uint64_t)match_cost(L, O) + cost[i + L];
            if (cc < best) { best = cc; best_ml = L; best_mo = O; }
        }
        cost[i] = best;
        choice[i].match_len = best_ml;
        choice[i].match_off = best_mo;
    }

    long nrec = 0;
    long i = 0, lit_start = 0;
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

    double t0 = now_sec();
    if (libsais(T, SA, (int32_t)n, 0, NULL) != 0) { fprintf(stderr, "libsais fail\n"); return 2; }
    libsais_plcp(T, SA, PLCP, (int32_t)n);
    libsais_lcp(PLCP, SA, LCP, (int32_t)n);
    for (long i = 0; i < n; i++) ISA[SA[i]] = (int32_t)i;
    double t1 = now_sec();
    printf("SA+ISA+LCP : %.0f ms\n", (t1 - t0) * 1000.0);

    long pool_cap = (long)n * M8_MAX_CANDS;
    cand_index_t *idx = (cand_index_t *)malloc(sizeof(cand_index_t) * (size_t)n);
    cand_t *pool = (cand_t *)malloc(sizeof(cand_t) * (size_t)pool_cap);
    double tm0 = now_sec();
    long pool_used = collect_candidates(n, SA, ISA, LCP, idx, pool, pool_cap);
    double tm1 = now_sec();
    if (pool_used < 0) { fprintf(stderr, "candidate pool overflow\n"); return 3; }
    printf("candidates : %.0f ms  (pool %ld entries, avg %.2f per position)\n",
           (tm1 - tm0) * 1000.0, pool_used, (double)pool_used / (double)n);

    parse_record_t *recs = (parse_record_t *)malloc(sizeof(parse_record_t) * (size_t)(n + 1));
    double td0 = now_sec();
    long nrec = optimal_parse_multi(n, idx, pool, recs, n + 1);
    double td1 = now_sec();
    if (nrec < 0) { fprintf(stderr, "DP overflow\n"); return 4; }
    printf("optimal_dp : %.0f ms  (%ld records)\n", (td1 - td0) * 1000.0, nrec);

    uint64_t total_lit = 0, total_match = 0;
    long match_count = 0;
    for (long i = 0; i < nrec; i++) {
        total_lit += recs[i].lit_len;
        if (recs[i].match_len) { total_match += recs[i].match_len; match_count++; }
    }
    printf("breakdown  : literals=%llu match=%llu (matches=%ld, avg=%.1f)\n",
           (unsigned long long)total_lit, (unsigned long long)total_match, match_count,
           match_count ? (double)total_match / (double)match_count : 0.0);

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
        return 5;
    }
    printf("zstd-encode: %.0f ms  out=%zu bytes\n", (tz1 - tz0) * 1000.0, zsize);

    uint8_t *back = (uint8_t *)malloc((size_t)n);
    size_t got = ZSTD_decompress(back, (size_t)n, zout, zsize);
    if (ZSTD_isError(got) || got != (size_t)n || memcmp(back, T, n) != 0) {
        fprintf(stderr, "roundtrip FAIL\n");
        return 6;
    }
    printf("roundtrip  : OK\n");

    ZSTD_CCtx *ref = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(ref, ZSTD_c_compressionLevel, 19);
    uint8_t *ref_out = (uint8_t *)malloc(bound);
    size_t ref_size = ZSTD_compress2(ref, ref_out, bound, T, (size_t)n);
    printf("zstd-19    : out=%zu bytes\n", ref_size);

    double m8_ratio = (double)zsize / (double)n;
    double ref_ratio = (double)ref_size / (double)n;
    double delta = (double)((long long)zsize - (long long)ref_size) / (double)ref_size * 100.0;
    printf("---\n");
    printf("M8a (multi): %.4f  (%zu / %ld)\n", m8_ratio, zsize, n);
    printf("zstd-19    : %.4f  (%zu / %ld)\n", ref_ratio, ref_size, n);
    printf("delta      : %+.2f%% (M8a vs zstd-19)\n", delta);

    ZSTD_freeCCtx(cctx); ZSTD_freeCCtx(ref);
    free(T); free(SA); free(ISA); free(PLCP); free(LCP);
    free(idx); free(pool); free(recs); free(seqs); free(zout); free(back); free(ref_out);
    return 0;
}
