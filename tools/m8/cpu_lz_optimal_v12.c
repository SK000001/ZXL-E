// M8a step 4 candidate v12 -- v11 + hash-chain augmentation for short matches.
//
// Diagnostic on v11 at 30 MB vs zstd-19 found:
//   * v11 has 60K fewer matches total.
//   * Gap concentrates in OFFSET classes 5-7 (32-256 bytes), where zstd has
//     120K MORE matches than v11.
//   * Cause: SA-based candidate enumeration visits SA-rank neighbors -- which
//     correspond to globally-similar suffixes, not text-locally-recent ones.
//     zstd's hash-chain match finder explicitly walks recent positions of the
//     same 3-byte prefix, which finds the medium-offset short matches we miss.
//
// v12 augments collect_candidates: in addition to the bidirectional SA walk,
// each position consults a 3-byte-prefix hash table that maps to a chain of
// the last K=8 positions of that prefix. Each chain entry is a candidate
// (max_match_length_at_that_offset, offset). The DP then chooses freely.
//
// Hash table: 16-bit hash of T[i..i+3], 2^16 buckets, each bucket holds the
// last K=8 positions. Update at every position (insert at head, shift older
// entries down). Total memory: 65K * 8 * 4 = 2 MB.
//
// v9 cand-set enumeration emits one (L, O) per length tier from the SA walk.
// DP then tries each as the FULL length L. But for any candidate (L, O), the
// match is also valid at every length L' in [MINMATCH, L] -- the same offset
// produces correct bytes for any prefix. We never tried (L', O) for L' < L.
//
// This matters when:
//   - shorter match yields a better LL/ML cost trade-off (e.g. avoids splitting
//     a future better match)
//   - shorter match leaves room for a high-value rep at i+L' the full-length
//     match would consume
//
// v11 expands each (L, O) candidate to L - MINMATCH + 1 sub-decisions in DP.
// Cost grows linearly with avg match length (~12-15 at 30 MB), so DP wall time
// ~12x. Measured baseline ~2.6s at 30 MB -> projected ~30s. Acceptable for one
// gate measurement.
//
// This is the structurally distinct thing remaining: it changes the DP's
// decision space, not the cost model. If v11 still can't clear 30 MB +2%, the
// wall is genuinely zstd-19's parser, not our cost model OR candidate set.
//
// v6 used the best cost model we found (per-offset-class FSE) but kept v5's
// candidate caps: M8_MAX_CANDS=8 and M8_WALK_LIMIT=128. At 30 MB the avg
// candidates/position is still ~1.1, suggesting the caps clip often.
//
// v9 bumps MAX_CANDS=16 and WALK_LIMIT=512. With learned per-offset-class
// costs (v6), DP can now choose between a small-offset short match (cheap
// in v6 cost) and a larger-offset longer match. The bidirectional SA walk
// only keeps the SMALLEST offset per length tier, so widening the walk
// surfaces additional length tiers we previously missed.
//
// Original v6 header follows:
// ----
//
// v5 replaced the flat overhead constant 4 -> 6 ("offset code is ~6 bits FSE
// on average"). In real zstd that constant is NOT flat: it depends on the
// offset class (log2(offset)). Common offset classes -- small offsets and
// classes that recur often -- get short FSE codes (~2-4 bits); rare classes
// (very large offsets) get long codes (~7-8 bits).
//
// v6 learns this distribution from pass 1:
//   pass 1: DP with v5 cost model
//   collect of_class[c] = count of pass-1 matches with log2(offset) == c
//   compute of_fse_cost[c] = -log2(of_class[c] / total_matches) bits,
//           clamped to [2, 12] bits, in 1/8-bit units
//   pass 2: match_cost(L, O) = of_fse_cost[log2(O)] + log2(O) * 8
//                            + log2(L) * 8 + 32     // 4 bits const for ml+ll
//
// Effect: matches at the most-used offset classes are priced ~2-3 bits cheaper
// than v5's flat 6-bit assumption; matches at exotic offset classes are priced
// dearer -- so the DP rejects more marginal far-offset short matches. This is
// the leverage zstd-19's per-block FSE retuning gives that we lack.
//
// Literal byte-cost from pass 1 (as in v4/v5) is also applied in pass 2.

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
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
#define M8_MAX_CANDS 24     // v12: 16 -> 24 (room for hash chain candidates)
#define M8_WALK_LIMIT 512
#define M8_HASH_BITS 16
#define M8_HASH_SIZE (1u << M8_HASH_BITS)
#define M8_CHAIN_K 8

static inline uint32_t m8_hash4(const uint8_t *p) {
    // FNV-style 4-byte hash, truncated to M8_HASH_BITS.
    uint32_t h = 2166136261u;
    h = (h ^ p[0]) * 16777619u;
    h = (h ^ p[1]) * 16777619u;
    h = (h ^ p[2]) * 16777619u;
    h = (h ^ p[3]) * 16777619u;
    return h & (M8_HASH_SIZE - 1);
}

typedef struct { uint32_t len; uint32_t off; } cand_t;
typedef struct { uint32_t start; uint8_t count; } cand_index_t;

static long collect_candidates(
    long n, const uint8_t *T,
    const int32_t *SA, const int32_t *ISA, const int32_t *LCP,
    cand_index_t *idx, cand_t *pool, long pool_cap)
{
    // Hash-chain table: [hash][chain_slot] -> last K positions.
    // chain_head[hash] points at the most-recently-written slot index.
    int32_t *chain = (int32_t *)malloc((size_t)M8_HASH_SIZE * M8_CHAIN_K * sizeof(int32_t));
    if (!chain) return -1;
    for (size_t i = 0; i < (size_t)M8_HASH_SIZE * M8_CHAIN_K; i++) chain[i] = -1;
    uint8_t *chain_head = (uint8_t *)calloc(M8_HASH_SIZE, 1);
    if (!chain_head) { free(chain); return -1; }

    long pool_used = 0;
    for (long p = 0; p < n; p++) {
        int32_t r = ISA[p];
        cand_t local[M8_MAX_CANDS];
        int n_local = 0;

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

        uint32_t min_collected_len = UINT32_MAX;
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

        // v12: hash-chain augmentation. Walk the chain at hash(T[p..p+3]) and
        // for each chain entry q < p, compute match length forward.
        // Update the chain AFTER reading so we don't match against ourselves.
        if (p + 4 <= n) {
            uint32_t h = m8_hash4(T + p);
            int32_t *bucket = &chain[(size_t)h * M8_CHAIN_K];
            for (int slot = 0; slot < M8_CHAIN_K; slot++) {
                int32_t q = bucket[slot];
                if (q < 0 || q >= (int32_t)p) continue;
                // Match-length forward.
                long limit = n - p;
                long L = 0;
                const uint8_t *a = T + p;
                const uint8_t *b = T + q;
                while (L < limit && a[L] == b[L]) L++;
                if (L >= M8_MINMATCH) {
                    TRY_INSERT(L, (uint32_t)(p - q));
                }
            }
            // Insert p into chain at head, shift older slots down.
            uint8_t head = chain_head[h];
            head = (head == 0) ? (M8_CHAIN_K - 1) : (head - 1);
            bucket[head] = (int32_t)p;
            chain_head[h] = head;
        }

        for (int k = 0; k < n_local; k++) {
            if (local[k].len + p > (uint32_t)n) local[k].len = (uint32_t)(n - p);
        }
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
            if (!found) local[n_keep++] = local[k];
        }

        idx[p].start = (uint32_t)pool_used;
        idx[p].count = (uint8_t)n_keep;
        if (pool_used + n_keep > pool_cap) { free(chain); free(chain_head); return -1; }
        for (int k = 0; k < n_keep; k++) pool[pool_used++] = local[k];
        #undef TRY_INSERT
    }
    free(chain); free(chain_head);
    return pool_used;
}

static inline uint32_t log2_u32(uint32_t x) {
    if (x <= 1) return 0;
    return 31 - __builtin_clz(x);
}

// v5 fallback: flat 6-bit overhead. Used in pass 1 (before of_fse_cost is built).
static inline uint32_t match_cost_flat(uint32_t l, uint32_t o) { return (6 + log2_u32(l) + log2_u32(o)) * 8; }

// v6 refined: use learned per-offset-class FSE cost.
// of_fse_cost_arr[c] is in 1/8-bit units; log2(L)*8 and log2(O)*8 are raw bits.
// +32 = 4 bits constant for combined match-length + literal-length code overhead.
static inline uint32_t match_cost_learned(uint32_t l, uint32_t o, const uint32_t *of_fse_cost_arr) {
    uint32_t c = log2_u32(o);
    return of_fse_cost_arr[c] + (log2_u32(o) + log2_u32(l)) * 8 + 32;
}

static inline uint32_t rep_cost(uint32_t l) { return (3 + log2_u32(l)) * 8; }

typedef struct {
    uint64_t cost;
    uint32_t rep[3];
    uint32_t match_len;
    uint32_t match_off;
    uint32_t prev_pos;
} fstate_t;

typedef struct { uint32_t lit_len; uint32_t match_len; uint32_t match_off; } parse_record_t;

static inline void try_step(fstate_t *F, long i, long j, uint64_t add_cost,
                            uint32_t m_len, uint32_t m_off)
{
    uint64_t new_cost = F[i].cost + add_cost;
    if (new_cost >= F[j].cost) return;
    F[j].cost = new_cost;
    F[j].match_len = m_len;
    F[j].match_off = m_off;
    F[j].prev_pos  = (uint32_t)i;
    if (m_len == 0) {
        F[j].rep[0] = F[i].rep[0]; F[j].rep[1] = F[i].rep[1]; F[j].rep[2] = F[i].rep[2];
    } else if (m_off == F[i].rep[0]) {
        F[j].rep[0] = F[i].rep[0]; F[j].rep[1] = F[i].rep[1]; F[j].rep[2] = F[i].rep[2];
    } else if (m_off == F[i].rep[1]) {
        F[j].rep[0] = F[i].rep[1]; F[j].rep[1] = F[i].rep[0]; F[j].rep[2] = F[i].rep[2];
    } else if (m_off == F[i].rep[2]) {
        F[j].rep[0] = F[i].rep[2]; F[j].rep[1] = F[i].rep[0]; F[j].rep[2] = F[i].rep[1];
    } else {
        F[j].rep[0] = m_off; F[j].rep[1] = F[i].rep[0]; F[j].rep[2] = F[i].rep[1];
    }
}

// of_fse_cost_arr == NULL  -> use flat match_cost (v5 model)
// of_fse_cost_arr != NULL  -> use learned per-offset-class FSE cost
static long optimal_parse_pass(
    long n, const uint8_t *T,
    const cand_index_t *idx, const cand_t *pool,
    const uint32_t *lit_cost_arr,
    const uint32_t *of_fse_cost_arr,
    parse_record_t *recs, long recs_cap)
{
    fstate_t *F = (fstate_t *)malloc(sizeof(fstate_t) * (size_t)(n + 1));
    if (!F) return -1;
    for (long i = 0; i <= n; i++) {
        F[i].cost = UINT64_MAX;
        F[i].rep[0] = F[i].rep[1] = F[i].rep[2] = 0;
        F[i].match_len = 0; F[i].match_off = 0; F[i].prev_pos = 0;
    }
    F[0].cost = 0;
    F[0].rep[0] = 1; F[0].rep[1] = 4; F[0].rep[2] = 8;

    for (long i = 0; i < n; i++) {
        if (F[i].cost == UINT64_MAX) continue;
        uint32_t lc = lit_cost_arr ? lit_cost_arr[T[i]] : (uint32_t)(8 * 8);
        try_step(F, i, i + 1, lc, 0, 0);

        const cand_t *c = &pool[idx[i].start];
        for (int k = 0; k < idx[i].count; k++) {
            uint32_t Lmax = c[k].len;
            uint32_t O    = c[k].off;
            if ((long)i + Lmax > n) Lmax = (uint32_t)(n - i);
            if (Lmax < M8_MINMATCH) continue;
            int is_rep = (O == F[i].rep[0]) || (O == F[i].rep[1]) || (O == F[i].rep[2]);
            // v11: try every length L in [MINMATCH, Lmax] at this offset.
            // To bound the DP cost-growth at large Lmax (mozilla can produce
            // 1000+ length matches), step through small lengths densely and
            // long lengths sparsely: L in {MM, MM+1, ..., MM+7} U {16,32,64,
            // 128, 256, 512, 1024, ...} U {Lmax}. Caps explosion while keeping
            // structural choice.
            for (uint32_t L = M8_MINMATCH; L <= Lmax; L++) {
                uint64_t add = is_rep ? rep_cost(L)
                              : (of_fse_cost_arr ? match_cost_learned(L, O, of_fse_cost_arr)
                                                 : match_cost_flat(L, O));
                try_step(F, i, i + L, add, L, O);
                // Sparse stepping past MINMATCH+7: only powers-of-two ranges.
                if (L >= M8_MINMATCH + 7) {
                    uint32_t step = L >> 2;       // 25% growth
                    if (step < 1) step = 1;
                    L += step - 1;                // -1 because loop ++ adds back 1
                }
            }
            // Always also try the exact Lmax (may have been skipped by sparse step).
            uint64_t add = is_rep ? rep_cost(Lmax)
                          : (of_fse_cost_arr ? match_cost_learned(Lmax, O, of_fse_cost_arr)
                                             : match_cost_flat(Lmax, O));
            try_step(F, i, i + Lmax, add, Lmax, O);
        }

        for (int k = 0; k < 3; k++) {
            uint32_t O = F[i].rep[k];
            if (O == 0 || (long)O > i) continue;
            long lim = n - i;
            long L = 0;
            const uint8_t *a = T + i;
            const uint8_t *b = T + i - O;
            while (L < lim && a[L] == b[L]) L++;
            if (L >= M8_MINMATCH) {
                try_step(F, i, i + L, rep_cost((uint32_t)L), (uint32_t)L, O);
            }
        }
    }

    if (F[n].cost == UINT64_MAX) { free(F); return -1; }

    uint32_t *step_match_len = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(n + 1));
    uint32_t *step_match_off = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(n + 1));
    long    *step_pos        = (long    *)malloc(sizeof(long)     * (size_t)(n + 1));
    long ns = 0;
    {
        long p = n;
        while (p > 0) {
            step_pos[ns]       = p;
            step_match_len[ns] = F[p].match_len;
            step_match_off[ns] = F[p].match_off;
            ns++;
            p = (long)F[p].prev_pos;
        }
    }

    long nrec = 0;
    long lit_start = 0;
    long i = 0;
    for (long s = ns - 1; s >= 0; s--) {
        long end = step_pos[s];
        uint32_t ml = step_match_len[s];
        uint32_t mo = step_match_off[s];
        if (ml == 0) { i = end; continue; }
        if (nrec >= recs_cap) {
            free(F); free(step_match_len); free(step_match_off); free(step_pos);
            return -1;
        }
        recs[nrec].lit_len   = (uint32_t)(i - lit_start);
        recs[nrec].match_len = ml;
        recs[nrec].match_off = mo;
        nrec++;
        i = end;
        lit_start = i;
    }
    if (lit_start < n) {
        if (nrec >= recs_cap) {
            free(F); free(step_match_len); free(step_match_off); free(step_pos);
            return -1;
        }
        recs[nrec].lit_len   = (uint32_t)(n - lit_start);
        recs[nrec].match_len = 0;
        recs[nrec].match_off = 0;
        nrec++;
    }

    free(F); free(step_match_len); free(step_match_off); free(step_pos);
    return nrec;
}

// Walk pass-1 records, count match offset CLASSES (log2(off) bucket).
// Produce of_fse_cost_out[c] in 1/8-bit units, clamped to [2, 12] bits.
// 32 classes covers offsets up to 2^32.
#define M8_OF_CLASSES 32
static void build_offset_cost(
    const parse_record_t *recs, long nrec,
    uint32_t *of_fse_cost_out)
{
    uint64_t of_class[M8_OF_CLASSES] = {0};
    uint64_t total = 0;
    for (long r = 0; r < nrec; r++) {
        if (recs[r].match_len == 0) continue;
        uint32_t o = recs[r].match_off;
        if (o == 0) continue;
        uint32_t c = log2_u32(o);
        if (c >= M8_OF_CLASSES) c = M8_OF_CLASSES - 1;
        of_class[c]++;
        total++;
    }
    for (int c = 0; c < M8_OF_CLASSES; c++) {
        double bits;
        if (total == 0 || of_class[c] == 0) {
            bits = 12.0;
        } else {
            double p = (double)of_class[c] / (double)total;
            bits = -log2(p);
            if (bits < 2.0) bits = 2.0;
            if (bits > 12.0) bits = 12.0;
        }
        of_fse_cost_out[c] = (uint32_t)(bits * 8.0 + 0.5);
    }
}

static void build_literal_cost(
    long n, const uint8_t *T, const parse_record_t *recs, long nrec,
    uint32_t *byte_cost_out)
{
    uint64_t freq[256] = {0};
    uint64_t total = 0;
    long pos = 0;
    for (long r = 0; r < nrec; r++) {
        for (uint32_t k = 0; k < recs[r].lit_len; k++) {
            if (pos < n) { freq[T[pos]]++; total++; }
            pos++;
        }
        pos += recs[r].match_len;
    }
    for (int b = 0; b < 256; b++) {
        double bits;
        if (total == 0 || freq[b] == 0) {
            bits = 12.0;
        } else {
            double p = (double)freq[b] / (double)total;
            bits = -log2(p);
            if (bits < 1.0) bits = 1.0;
            if (bits > 12.0) bits = 12.0;
        }
        byte_cost_out[b] = (uint32_t)(bits * 8.0 + 0.5);
    }
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

    // v9: M8_MAX_CANDS=16 doubled the upper bound. Use n*8 (1.9 GB at 30 MB)
    // for safety at small n; in practice pool_used << this on large inputs.
    long pool_cap = (long)n * 8;
    cand_index_t *idx = (cand_index_t *)malloc(sizeof(cand_index_t) * (size_t)n);
    cand_t *pool = (cand_t *)malloc(sizeof(cand_t) * (size_t)pool_cap);
    double tm0 = now_sec();
    long pool_used = collect_candidates(n, T, SA, ISA, LCP, idx, pool, pool_cap);
    double tm1 = now_sec();
    if (pool_used < 0) { fprintf(stderr, "candidate pool overflow\n"); return 3; }
    printf("candidates : %.0f ms  (pool %ld entries, avg %.2f per position)\n",
           (tm1 - tm0) * 1000.0, pool_used, (double)pool_used / (double)n);

    parse_record_t *recs = (parse_record_t *)malloc(sizeof(parse_record_t) * (size_t)(n + 1));

    double tp1_0 = now_sec();
    long nrec1 = optimal_parse_pass(n, T, idx, pool, NULL, NULL, recs, n + 1);
    double tp1_1 = now_sec();
    if (nrec1 < 0) { fprintf(stderr, "pass1 DP fail\n"); return 4; }
    printf("pass1_dp   : %.0f ms  (%ld records)\n", (tp1_1 - tp1_0) * 1000.0, nrec1);

    uint32_t byte_cost[256];
    build_literal_cost(n, T, recs, nrec1, byte_cost);
    uint32_t of_fse_cost[M8_OF_CLASSES];
    build_offset_cost(recs, nrec1, of_fse_cost);
    {
        uint32_t lo = UINT32_MAX, hi = 0;
        for (int c = 0; c < M8_OF_CLASSES; c++) {
            if (of_fse_cost[c] < lo) lo = of_fse_cost[c];
            if (of_fse_cost[c] > hi) hi = of_fse_cost[c];
        }
        printf("of_cost    : min=%.2fb max=%.2fb (per offset class, 1/8-bit units)\n",
               lo / 8.0, hi / 8.0);
    }

    double tp2_0 = now_sec();
    long nrec = optimal_parse_pass(n, T, idx, pool, byte_cost, of_fse_cost, recs, n + 1);
    double tp2_1 = now_sec();
    if (nrec < 0) { fprintf(stderr, "pass2 DP fail\n"); return 5; }
    printf("pass2_dp   : %.0f ms  (%ld records)\n", (tp2_1 - tp2_0) * 1000.0, nrec);

    uint64_t total_lit = 0, total_match = 0;
    long match_count = 0;
    for (long i = 0; i < nrec; i++) {
        total_lit += recs[i].lit_len;
        if (recs[i].match_len) { total_match += recs[i].match_len; match_count++; }
    }
    printf("breakdown  : literals=%llu match=%llu (matches=%ld, avg=%.1f)\n",
           (unsigned long long)total_lit, (unsigned long long)total_match, match_count,
           match_count ? (double)total_match / (double)match_count : 0.0);

    // Diagnostic: simulate zstd's rep history through our records to count
    // how many of our matches would resolve as repcodes (offset code 1/2/3).
    {
        uint32_t rep0 = 1, rep1 = 4, rep2 = 8;
        long nrep_total = 0;
        for (long i = 0; i < nrec; i++) {
            if (recs[i].match_len == 0) continue;
            uint32_t o = recs[i].match_off;
            if (o == rep0) {
                nrep_total++;
            } else if (o == rep1) {
                nrep_total++;
                uint32_t t = rep0; rep0 = rep1; rep1 = t;
            } else if (o == rep2) {
                nrep_total++;
                uint32_t t = rep0; rep0 = rep2; rep2 = rep1; rep1 = t;
            } else {
                rep2 = rep1; rep1 = rep0; rep0 = o;
            }
        }
        printf("rep usage  : %ld of %ld matches (%.2f%%)\n",
               nrep_total, match_count,
               match_count ? 100.0 * nrep_total / (double)match_count : 0.0);
    }

    {
        uint64_t ml_hist[24] = {0}, of_hist[32] = {0};
        for (long i = 0; i < nrec; i++) {
            if (recs[i].match_len == 0) continue;
            uint32_t mlc = log2_u32(recs[i].match_len); if (mlc >= 24) mlc = 23;
            uint32_t ofc = log2_u32(recs[i].match_off); if (ofc >= 32) ofc = 31;
            ml_hist[mlc]++;
            of_hist[ofc]++;
        }
        printf("ML hist (count by log2(ml)):\n");
        for (int c = 0; c < 24; c++) if (ml_hist[c]) printf("  [%2d: %d..%d) %12llu (%.2f%%)\n",
            c, 1 << c, 1 << (c + 1), (unsigned long long)ml_hist[c], 100.0 * ml_hist[c] / (double)match_count);
        printf("OF hist (count by log2(off)):\n");
        for (int c = 0; c < 32; c++) if (of_hist[c]) printf("  [%2d: %d..%d) %12llu (%.2f%%)\n",
            c, 1 << c, 1 << (c + 1), (unsigned long long)of_hist[c], 100.0 * of_hist[c] / (double)match_count);
    }

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
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 27);
    ZSTD_CCtx_setPledgedSrcSize(cctx, (size_t)n);

    size_t bound = ZSTD_compressBound((size_t)n);
    uint8_t *zout = (uint8_t *)malloc(bound);
    size_t zsize = ZSTD_compressSequences(cctx, zout, bound, seqs, (size_t)nrec, T, (size_t)n);
    if (ZSTD_isError(zsize)) {
        fprintf(stderr, "compressSequences fail: %s\n", ZSTD_getErrorName(zsize));
        return 6;
    }

    uint8_t *back = (uint8_t *)malloc((size_t)n);
    size_t got = ZSTD_decompress(back, (size_t)n, zout, zsize);
    if (ZSTD_isError(got) || got != (size_t)n || memcmp(back, T, n) != 0) {
        fprintf(stderr, "roundtrip FAIL\n");
        return 7;
    }
    printf("roundtrip  : OK\n");

    ZSTD_CCtx *ref = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(ref, ZSTD_c_compressionLevel, 19);
    uint8_t *ref_out = (uint8_t *)malloc(bound);
    size_t ref_size = ZSTD_compress2(ref, ref_out, bound, T, (size_t)n);

    double m8_ratio = (double)zsize / (double)n;
    double ref_ratio = (double)ref_size / (double)n;
    double delta = (double)((long long)zsize - (long long)ref_size) / (double)ref_size * 100.0;
    printf("---\n");
    printf("M8a v12(hc) : %.4f  (%zu / %ld)\n", m8_ratio, zsize, n);
    printf("zstd-19     : %.4f  (%zu / %ld)\n", ref_ratio, ref_size, n);
    printf("delta       : %+.2f%% (M8a v12 vs zstd-19)\n", delta);

    ZSTD_freeCCtx(cctx); ZSTD_freeCCtx(ref);
    free(T); free(SA); free(ISA); free(PLCP); free(LCP);
    free(idx); free(pool); free(recs); free(seqs); free(zout); free(back); free(ref_out);
    return 0;
}
