#include "deflate.h"
#include "util.h"
#include <zlib.h>

uint32_t r32be(const uint8_t *p) {
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | (uint32_t)p[3];
}
void w32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v>>24); p[1] = (uint8_t)(v>>16); p[2] = (uint8_t)(v>>8); p[3] = (uint8_t)v;
}

uint8_t *raw_inflate(const uint8_t *src, uint32_t comp_size, uint32_t raw_size) {
    uint8_t *out = malloc(raw_size ? raw_size : 1);
    if (!out) die("malloc inflate");
    z_stream z = {0};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) { free(out); return NULL; }
    z.next_in = (Bytef *)src;
    z.avail_in = comp_size;
    z.next_out = out;
    z.avail_out = raw_size;
    int rc = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (rc != Z_STREAM_END || z.total_out != raw_size) { free(out); return NULL; }
    return out;
}

uint8_t *raw_deflate_l9(const uint8_t *raw, uint32_t raw_size, size_t *out_len) {
    z_stream z = {0};
    if (deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;
    size_t bound = deflateBound(&z, raw_size);
    uint8_t *out = malloc(bound ? bound : 1);
    if (!out) { deflateEnd(&z); die("malloc deflate"); }
    z.next_in = (Bytef *)raw;
    z.avail_in = raw_size;
    z.next_out = out;
    z.avail_out = bound;
    int rc = deflate(&z, Z_FINISH);
    if (rc != Z_STREAM_END) { deflateEnd(&z); free(out); return NULL; }
    *out_len = z.total_out;
    deflateEnd(&z);
    return out;
}

uint8_t *raw_inflate_span(const uint8_t *src, size_t src_n,
                          size_t *consumed, size_t *out_n) {
    z_stream z = {0};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return NULL;
    size_t cap = src_n * 4 + 4096;
    uint8_t *out = malloc(cap);
    if (!out) { inflateEnd(&z); die("malloc raw_inflate_span"); }
    z.next_in = (Bytef *)src;
    z.avail_in = (uInt)(src_n > 0xFFFFFFFFu ? 0xFFFFFFFFu : src_n);
    z.next_out = out;
    z.avail_out = (uInt)cap;
    for (;;) {
        int rc = inflate(&z, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR || rc == Z_OK) {
            if (z.avail_in == 0 && z.avail_out != 0) { inflateEnd(&z); free(out); return NULL; }
            if (z.avail_out == 0) {
                size_t newcap = cap * 2;
                uint8_t *no = realloc(out, newcap);
                if (!no) { inflateEnd(&z); free(out); return NULL; }
                out = no;
                z.next_out = out + z.total_out;
                z.avail_out = (uInt)(newcap - z.total_out);
                cap = newcap;
            }
            continue;
        }
        inflateEnd(&z); free(out); return NULL;
    }
    *consumed = z.total_in;
    *out_n = z.total_out;
    inflateEnd(&z);
    return out;
}

uint8_t *zlib_inflate_dyn(const uint8_t *src, size_t src_n, size_t *out_n) {
    z_stream z = {0};
    if (inflateInit(&z) != Z_OK) return NULL;
    size_t cap = src_n * 4 + 4096;
    uint8_t *out = malloc(cap);
    if (!out) { inflateEnd(&z); die("malloc inflate"); }
    z.next_in = (Bytef *)src;
    z.avail_in = (uInt)src_n;
    z.next_out = out;
    z.avail_out = (uInt)cap;
    for (;;) {
        int rc = inflate(&z, Z_FINISH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR || rc == Z_OK) {
            if (z.avail_in == 0) { inflateEnd(&z); free(out); return NULL; }
            size_t newcap = cap * 2;
            uint8_t *no = realloc(out, newcap);
            if (!no) { inflateEnd(&z); free(out); return NULL; }
            out = no;
            z.next_out = out + z.total_out;
            z.avail_out = (uInt)(newcap - z.total_out);
            cap = newcap;
            continue;
        }
        inflateEnd(&z); free(out); return NULL;
    }
    *out_n = z.total_out;
    inflateEnd(&z);
    return out;
}

uint8_t *zlib_deflate_l9(const uint8_t *raw, size_t raw_n, size_t *out_n) {
    z_stream z = {0};
    if (deflateInit2(&z, 9, Z_DEFLATED, MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;
    size_t bound = deflateBound(&z, (uLong)raw_n);
    uint8_t *out = malloc(bound ? bound : 1);
    if (!out) { deflateEnd(&z); die("malloc zlib_deflate"); }
    z.next_in = (Bytef *)raw;
    z.avail_in = (uInt)raw_n;
    z.next_out = out;
    z.avail_out = (uInt)bound;
    int rc = deflate(&z, Z_FINISH);
    if (rc != Z_STREAM_END) { deflateEnd(&z); free(out); return NULL; }
    *out_n = z.total_out;
    deflateEnd(&z);
    return out;
}

/* Param packing for OP_REDEFLATE_P (two bytes):
 *   param0 = level(bits 0-3) | memLevel(bits 4-7)   -- both 1..9
 *   param1 = strategy(bits 0-2) | (-windowBits)(bits 3-7)  -- strat 0..4, wb 9..15
 */
static void ladder_pack(int level, int mem, int strat, int wbits,
                        uint8_t *p0, uint8_t *p1) {
    *p0 = (uint8_t)((level & 0x0F) | ((mem & 0x0F) << 4));
    *p1 = (uint8_t)((strat & 0x07) | (((-wbits) & 0x1F) << 3));
}
static int ladder_unpack(uint8_t p0, uint8_t p1,
                         int *level, int *mem, int *strat, int *wbits) {
    *level = p0 & 0x0F;
    *mem   = (p0 >> 4) & 0x0F;
    *strat = p1 & 0x07;
    *wbits = -((p1 >> 3) & 0x1F);
    if (*level < 1 || *level > 9) return 0;
    if (*mem   < 1 || *mem   > 9) return 0;
    if (*strat < 0 || *strat > 4) return 0;
    if (*wbits > -9 || *wbits < -15) return 0;
    return 1;
}

/* One raw-deflate at explicit params; returns malloc'd buffer or NULL. */
static uint8_t *deflate_at(const uint8_t *raw, uint32_t raw_n,
                           int level, int mem, int strat, int wbits,
                           size_t *out_n) {
    z_stream z = {0};
    if (deflateInit2(&z, level, Z_DEFLATED, wbits, mem, strat) != Z_OK) return NULL;
    size_t bound = deflateBound(&z, raw_n);
    uint8_t *out = malloc(bound ? bound : 1);
    if (!out) { deflateEnd(&z); return NULL; }
    z.next_in = (Bytef *)raw;
    z.avail_in = raw_n;
    z.next_out = out;
    z.avail_out = (uInt)bound;
    int rc = deflate(&z, Z_FINISH);
    if (rc != Z_STREAM_END) { deflateEnd(&z); free(out); return NULL; }
    *out_n = z.total_out;
    deflateEnd(&z);
    return out;
}

int redeflate_ladder_find(const uint8_t *raw, uint32_t raw_n,
                          const uint8_t *def, uint32_t def_n,
                          uint8_t *param0, uint8_t *param1) {
    /* Ordered by observed frequency (probe 2026-07-23): wbits -15 and level 6
     * first, so real-world zlib-L6 streams match on the first few tries. Only
     * streams that miss the L9 fast path reach here; a full miss (non-zlib
     * producer) walks the whole set, then the caller falls to preflate. */
    static const int wb[]  = { -15, -14, -13, -12, -11 };
    static const int lv[]  = { 6, 5, 4, 7, 8, 9, 3, 2, 1 };
    static const int mm[]  = { 8, 9 };
    for (size_t wi = 0; wi < sizeof(wb)/sizeof(wb[0]); wi++)
    for (size_t li = 0; li < sizeof(lv)/sizeof(lv[0]); li++)
    for (size_t mi = 0; mi < sizeof(mm)/sizeof(mm[0]); mi++) {
        size_t got = 0;
        uint8_t *out = deflate_at(raw, raw_n, lv[li], mm[mi],
                                  Z_DEFAULT_STRATEGY, wb[wi], &got);
        if (!out) continue;
        int ok = (got == def_n && memcmp(out, def, def_n) == 0);
        free(out);
        if (ok) {
            ladder_pack(lv[li], mm[mi], Z_DEFAULT_STRATEGY, wb[wi], param0, param1);
            return 1;
        }
    }
    return 0;
}

uint8_t *redeflate_ladder_apply(const uint8_t *raw, uint32_t raw_n,
                                uint8_t param0, uint8_t param1, size_t *out_n) {
    int level, mem, strat, wbits;
    if (!ladder_unpack(param0, param1, &level, &mem, &strat, &wbits)) return NULL;
    return deflate_at(raw, raw_n, level, mem, strat, wbits, out_n);
}
