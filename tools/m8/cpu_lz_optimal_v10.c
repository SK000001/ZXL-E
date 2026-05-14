// M8a step 4 candidate v10 -- v9 wide candidates + v8 per-block FSE tables.
//
// v9 (wide candidates over v6 cost model) brought 30 MB from +2.74% (v6) to
// +2.32% -- the largest single-iteration drop seen. v8 (per-block FSE tables)
// alone didn't help over v6/v7 because the candidate set wasn't wide enough
// for the per-block costs to differentiate. v10 combines them: each 128 KB
// block has its own (byte_cost, of_fse_cost, ml_fse_cost), and the DP picks
// from 2x the candidates per position with those block-local costs.
//
// Hypothesis: wide candidates make per-block costs more meaningful (more
// alternatives to choose between under each region's distribution), so the
// combination should beat the sum of either alone. Targeted at the last
// 0.32pp needed to clear the +2% abandon threshold at 30 MB.
//
// v5-v7 used a single global set of (byte_cost, of_fse_cost, ml_fse_cost)
// learned from a global pass 1. The mixed silesia corpus contains very
// different regions (mozilla = binary, webster = English text, nci = chemical
// text); averaging cost tables across them discards the per-region signal that
// zstd-19 captures via per-block FSE retuning.
//
// v8 segments the input into 128 KB blocks (matches zstd's internal block size
// at level 19), learns per-block tables from pass 1, and pass 2 uses
// cost_tables[i / 131072] at each position i. Candidate collection and the DP
// itself stay global (cross-block matches allowed via windowLog=27).
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
#define M8_MAX_CANDS 16     // v9/v10: 8 -> 16
#define M8_WALK_LIMIT 512   // v9/v10: 128 -> 512

typedef struct { uint32_t len; uint32_t off; } cand_t;
typedef struct { uint32_t start; uint8_t count; } cand_index_t;

static long collect_candidates(
    long n, const int32_t *SA, const int32_t *ISA, const int32_t *LCP,
    cand_index_t *idx, cand_t *pool, long pool_cap)
{
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
        if (pool_used + n_keep > pool_cap) return -1;
        for (int k = 0; k < n_keep; k++) pool[pool_used++] = local[k];
        #undef TRY_INSERT
    }
    return pool_used;
}

static inline uint32_t log2_u32(uint32_t x) {
    if (x <= 1) return 0;
    return 31 - __builtin_clz(x);
}

// v5 fallback: flat 6-bit overhead. Used in pass 1 (before of_fse_cost is built).
static inline uint32_t match_cost_flat(uint32_t l, uint32_t o) { return (6 + log2_u32(l) + log2_u32(o)) * 8; }

// v7 refined: use learned per-offset-class AND per-ml-class FSE cost.
//   of_fse_cost_arr[log2(o)]  -- offset code FSE cost (1/8-bit units)
//   ml_fse_cost_arr[log2(l)]  -- ml code FSE cost (1/8-bit units)
//   log2(o)*8 + log2(l)*8     -- raw bits below the code class
//   +16                       -- 2 bits constant for ll code (rough, the
//                                 LL FSE cost would need its own learned table;
//                                 deferred since LL has only ~16 useful classes
//                                 and ~3-bit cost on average).
// Per-block tables: of_cost[block * M8_OF_CLASSES + class], etc.
static inline uint32_t match_cost_learned(uint32_t l, uint32_t o,
                                          const uint32_t *of_row,
                                          const uint32_t *ml_row)
{
    uint32_t oc = log2_u32(o);
    uint32_t lc = log2_u32(l);
    return of_row[oc] + ml_row[lc] + (oc + lc) * 8 + 16;
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

#define M8_BLOCK_SIZE 131072
#define M8_OF_CLASSES 32
#define M8_ML_CLASSES 24

// Per-block tables when use_perblock != 0:
//   byte_cost_arr layout: [block * 256 + byte]
//   of_fse_cost_arr layout: [block * M8_OF_CLASSES + class]
//   ml_fse_cost_arr layout: [block * M8_ML_CLASSES + class]
// When use_perblock == 0, NULL means flat fallback (pass 1).
static long optimal_parse_pass(
    long n, const uint8_t *T,
    const cand_index_t *idx, const cand_t *pool,
    const uint32_t *lit_cost_arr,
    const uint32_t *of_fse_cost_arr,
    const uint32_t *ml_fse_cost_arr,
    int use_perblock,
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

        long blk = use_perblock ? (i / M8_BLOCK_SIZE) : 0;
        const uint32_t *lit_row = lit_cost_arr ? (use_perblock ? &lit_cost_arr[blk * 256] : lit_cost_arr) : NULL;
        const uint32_t *of_row  = of_fse_cost_arr ? (use_perblock ? &of_fse_cost_arr[blk * M8_OF_CLASSES] : of_fse_cost_arr) : NULL;
        const uint32_t *ml_row  = ml_fse_cost_arr ? (use_perblock ? &ml_fse_cost_arr[blk * M8_ML_CLASSES] : ml_fse_cost_arr) : NULL;

        uint32_t lc = lit_row ? lit_row[T[i]] : (uint32_t)(8 * 8);
        try_step(F, i, i + 1, lc, 0, 0);

        const cand_t *c = &pool[idx[i].start];
        for (int k = 0; k < idx[i].count; k++) {
            uint32_t L = c[k].len;
            uint32_t O = c[k].off;
            if ((long)i + L > n) continue;
            int is_rep = (O == F[i].rep[0]) || (O == F[i].rep[1]) || (O == F[i].rep[2]);
            uint64_t add;
            if (is_rep) add = rep_cost(L);
            else if (of_row && ml_row) add = match_cost_learned(L, O, of_row, ml_row);
            else add = match_cost_flat(L, O);
            try_step(F, i, i + L, add, L, O);
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
// Per-block cost builders. Walk pass-1 records once with a cumulative position
// counter; accumulate byte / of_class / ml_class histograms PER BLOCK; convert
// to costs. A literal byte at position p goes to block p/BLOCK_SIZE; a match
// starting at p attributes its (offset_class, ml_class) to block p/BLOCK_SIZE.
//
// Small-sample smoothing: if a block's literal-byte total < 256, blend with
// a global histogram; same for match-class total < 64. Without this, a block
// with very few samples produces noisy costs that hurt the parse.
static void build_perblock_costs(
    long n, const uint8_t *T,
    const parse_record_t *recs, long nrec,
    long num_blocks,
    uint32_t *byte_cost_out,        // [num_blocks * 256]
    uint32_t *of_fse_cost_out,      // [num_blocks * M8_OF_CLASSES]
    uint32_t *ml_fse_cost_out)      // [num_blocks * M8_ML_CLASSES]
{
    uint64_t *bfreq = (uint64_t *)calloc((size_t)num_blocks * 256, sizeof(uint64_t));
    uint64_t *oftab = (uint64_t *)calloc((size_t)num_blocks * M8_OF_CLASSES, sizeof(uint64_t));
    uint64_t *mltab = (uint64_t *)calloc((size_t)num_blocks * M8_ML_CLASSES, sizeof(uint64_t));
    uint64_t *btotal = (uint64_t *)calloc((size_t)num_blocks, sizeof(uint64_t));
    uint64_t *moftot = (uint64_t *)calloc((size_t)num_blocks, sizeof(uint64_t));
    uint64_t *mmltot = (uint64_t *)calloc((size_t)num_blocks, sizeof(uint64_t));
    uint64_t gbfreq[256] = {0}, gbtotal = 0;
    uint64_t goftab[M8_OF_CLASSES] = {0}, gmltab[M8_ML_CLASSES] = {0};
    uint64_t gmoftot = 0, gmmltot = 0;

    long pos = 0;
    for (long r = 0; r < nrec; r++) {
        for (uint32_t k = 0; k < recs[r].lit_len; k++) {
            if (pos >= n) break;
            long blk = pos / M8_BLOCK_SIZE;
            uint8_t b = T[pos];
            bfreq[blk * 256 + b]++;
            btotal[blk]++;
            gbfreq[b]++;
            gbtotal++;
            pos++;
        }
        if (recs[r].match_len) {
            long blk = pos / M8_BLOCK_SIZE;
            uint32_t oc = log2_u32(recs[r].match_off);
            uint32_t lc = log2_u32(recs[r].match_len);
            if (oc >= M8_OF_CLASSES) oc = M8_OF_CLASSES - 1;
            if (lc >= M8_ML_CLASSES) lc = M8_ML_CLASSES - 1;
            oftab[blk * M8_OF_CLASSES + oc]++;
            mltab[blk * M8_ML_CLASSES + lc]++;
            moftot[blk]++;
            mmltot[blk]++;
            goftab[oc]++;
            gmltab[lc]++;
            gmoftot++;
            gmmltot++;
            pos += recs[r].match_len;
        }
    }

    // Blend smoothing weight: alpha = block_total / (block_total + smoothing_N).
    // For literals smoothing_N = 256 (=1 sample per byte avg); for of/ml = 64.
    for (long blk = 0; blk < num_blocks; blk++) {
        double alpha_b = (double)btotal[blk] / (double)(btotal[blk] + 256);
        double alpha_o = (double)moftot[blk] / (double)(moftot[blk] + 64);
        double alpha_m = (double)mmltot[blk] / (double)(mmltot[blk] + 64);

        for (int b = 0; b < 256; b++) {
            double p_local  = btotal[blk] ? (double)bfreq[blk * 256 + b] / (double)btotal[blk] : 0.0;
            double p_global = gbtotal ? (double)gbfreq[b] / (double)gbtotal : (1.0 / 256.0);
            double p = alpha_b * p_local + (1.0 - alpha_b) * p_global;
            double bits = (p > 0.0) ? -log2(p) : 12.0;
            if (bits < 1.0) bits = 1.0;
            if (bits > 12.0) bits = 12.0;
            byte_cost_out[blk * 256 + b] = (uint32_t)(bits * 8.0 + 0.5);
        }
        for (int c = 0; c < M8_OF_CLASSES; c++) {
            double p_local  = moftot[blk] ? (double)oftab[blk * M8_OF_CLASSES + c] / (double)moftot[blk] : 0.0;
            double p_global = gmoftot ? (double)goftab[c] / (double)gmoftot : (1.0 / M8_OF_CLASSES);
            double p = alpha_o * p_local + (1.0 - alpha_o) * p_global;
            double bits = (p > 0.0) ? -log2(p) : 12.0;
            if (bits < 2.0) bits = 2.0;
            if (bits > 12.0) bits = 12.0;
            of_fse_cost_out[blk * M8_OF_CLASSES + c] = (uint32_t)(bits * 8.0 + 0.5);
        }
        for (int c = 0; c < M8_ML_CLASSES; c++) {
            double p_local  = mmltot[blk] ? (double)mltab[blk * M8_ML_CLASSES + c] / (double)mmltot[blk] : 0.0;
            double p_global = gmmltot ? (double)gmltab[c] / (double)gmmltot : (1.0 / M8_ML_CLASSES);
            double p = alpha_m * p_local + (1.0 - alpha_m) * p_global;
            double bits = (p > 0.0) ? -log2(p) : 10.0;
            if (bits < 1.0) bits = 1.0;
            if (bits > 10.0) bits = 10.0;
            ml_fse_cost_out[blk * M8_ML_CLASSES + c] = (uint32_t)(bits * 8.0 + 0.5);
        }
    }

    free(bfreq); free(oftab); free(mltab);
    free(btotal); free(moftot); free(mmltot);
}

static void build_ml_cost(
    const parse_record_t *recs, long nrec,
    uint32_t *ml_fse_cost_out)
{
    uint64_t ml_class[M8_ML_CLASSES] = {0};
    uint64_t total = 0;
    for (long r = 0; r < nrec; r++) {
        if (recs[r].match_len == 0) continue;
        uint32_t L = recs[r].match_len;
        uint32_t c = log2_u32(L);
        if (c >= M8_ML_CLASSES) c = M8_ML_CLASSES - 1;
        ml_class[c]++;
        total++;
    }
    for (int c = 0; c < M8_ML_CLASSES; c++) {
        double bits;
        if (total == 0 || ml_class[c] == 0) {
            bits = 10.0;
        } else {
            double p = (double)ml_class[c] / (double)total;
            bits = -log2(p);
            if (bits < 1.0) bits = 1.0;
            if (bits > 10.0) bits = 10.0;
        }
        ml_fse_cost_out[c] = (uint32_t)(bits * 8.0 + 0.5);
    }
}

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

    long pool_cap = (long)n * 8;  // v10: doubled because MAX_CANDS=16
    cand_index_t *idx = (cand_index_t *)malloc(sizeof(cand_index_t) * (size_t)n);
    cand_t *pool = (cand_t *)malloc(sizeof(cand_t) * (size_t)pool_cap);
    double tm0 = now_sec();
    long pool_used = collect_candidates(n, SA, ISA, LCP, idx, pool, pool_cap);
    double tm1 = now_sec();
    if (pool_used < 0) { fprintf(stderr, "candidate pool overflow\n"); return 3; }
    printf("candidates : %.0f ms  (pool %ld entries, avg %.2f per position)\n",
           (tm1 - tm0) * 1000.0, pool_used, (double)pool_used / (double)n);

    parse_record_t *recs = (parse_record_t *)malloc(sizeof(parse_record_t) * (size_t)(n + 1));

    double tp1_0 = now_sec();
    long nrec1 = optimal_parse_pass(n, T, idx, pool, NULL, NULL, NULL, 0, recs, n + 1);
    double tp1_1 = now_sec();
    if (nrec1 < 0) { fprintf(stderr, "pass1 DP fail\n"); return 4; }
    printf("pass1_dp   : %.0f ms  (%ld records)\n", (tp1_1 - tp1_0) * 1000.0, nrec1);

    long num_blocks = (n + M8_BLOCK_SIZE - 1) / M8_BLOCK_SIZE;
    uint32_t *byte_cost   = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)num_blocks * 256);
    uint32_t *of_fse_cost = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)num_blocks * M8_OF_CLASSES);
    uint32_t *ml_fse_cost = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)num_blocks * M8_ML_CLASSES);
    if (!byte_cost || !of_fse_cost || !ml_fse_cost) { fprintf(stderr, "perblock cost alloc fail\n"); return 5; }
    build_perblock_costs(n, T, recs, nrec1, num_blocks, byte_cost, of_fse_cost, ml_fse_cost);
    printf("perblock   : %ld blocks of %d bytes\n", num_blocks, M8_BLOCK_SIZE);

    double tp2_0 = now_sec();
    long nrec = optimal_parse_pass(n, T, idx, pool, byte_cost, of_fse_cost, ml_fse_cost, 1, recs, n + 1);
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
    printf("M8a v10(w+b): %.4f  (%zu / %ld)\n", m8_ratio, zsize, n);
    printf("zstd-19     : %.4f  (%zu / %ld)\n", ref_ratio, ref_size, n);
    printf("delta       : %+.2f%% (M8a v10 vs zstd-19)\n", delta);

    ZSTD_freeCCtx(cctx); ZSTD_freeCCtx(ref);
    free(T); free(SA); free(ISA); free(PLCP); free(LCP);
    free(idx); free(pool); free(recs); free(seqs); free(zout); free(back); free(ref_out);
    free(byte_cost); free(of_fse_cost); free(ml_fse_cost);
    return 0;
}
