#include "pdf.h"
#include "kinds.h"
#include "deflate.h"
#include "preflate_shim.h"
#include "jpeg.h"
#include <zlib.h>

/* Inflate a zlib stream of unknown compressed length starting at src; report
 * how many input bytes the stream actually consumed (header + deflate body +
 * adler32). Returns malloc'd output or NULL. */
static uint8_t *zlib_inflate_span(const uint8_t *src, size_t src_n,
                                  size_t *consumed, size_t *out_n) {
    z_stream s;
    memset(&s, 0, sizeof(s));
    if (inflateInit(&s) != Z_OK) return NULL;
    size_t cap = 1 << 16;
    uint8_t *out = malloc(cap);
    if (!out) { inflateEnd(&s); return NULL; }
    s.next_in  = (Bytef *)src;
    s.avail_in = src_n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uInt)src_n;
    int rc;
    do {
        if (s.total_out == cap) {
            cap *= 2;
            uint8_t *np = realloc(out, cap);
            if (!np) { free(out); inflateEnd(&s); return NULL; }
            out = np;
        }
        s.next_out  = out + s.total_out;
        s.avail_out = (uInt)(cap - s.total_out);
        rc = inflate(&s, Z_NO_FLUSH);
    } while (rc == Z_OK);
    if (rc != Z_STREAM_END) { free(out); inflateEnd(&s); return NULL; }
    *consumed = s.total_in;
    *out_n    = s.total_out;
    inflateEnd(&s);
    return out;
}

/* Walk JPEG segment structure from an FF D8 SOI; return the total length
 * through the EOI marker, or 0 if the structure doesn't parse. Handles
 * entropy-coded data (byte stuffing, RST markers) and progressive multi-SOS
 * layouts. Final say on validity stays with try_jpeg_buf's round-trip. */
static size_t jpeg_span(const uint8_t *p, size_t n) {
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return 0;
    size_t i = 2;
    while (i + 2 <= n) {
        if (p[i] != 0xFF) return 0;
        uint8_t m = p[i + 1];
        if (m == 0xD9) return i + 2;                           /* EOI */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        if (i + 4 > n) return 0;
        uint16_t len = (uint16_t)((p[i + 2] << 8) | p[i + 3]);
        if (len < 2 || i + 2 + len > n) return 0;
        i += 2 + len;
        if (m == 0xDA) {
            /* SOS: skip entropy-coded bytes to the next real marker. */
            while (i + 2 <= n) {
                if (p[i] != 0xFF || p[i + 1] == 0x00 ||
                    (p[i + 1] >= 0xD0 && p[i + 1] <= 0xD7)) { i++; continue; }
                break;
            }
        }
    }
    return 0;
}

int pack_flate_scan(const uint8_t *p, size_t n, const char *tmp_prefix,
                    Buf *recipe, Buf *b0, Buf *b1,
                    size_t scan_from, unsigned min_cover_permille) {
    if (n < 32) return -1;
    if (n > 0xFFFFFFFFu) return -1;

    /* Appends happen as streams verify; if the coverage gate fails at the
     * end, everything must roll back (callers share b0/b1). */
    size_t b0n = b0->n, b1n = b1->n, rn = recipe->n;
    size_t cursor = 0, covered = 0;
    int redeflated = 0, preflated = 0, jpegs = 0;

    for (size_t i = scan_from; i + 12 < n; i++) {
        /* Embedded JPEG (DCTDecode body): segment-walk to find the span,
         * then run the brunsli/packJPG race with round-trip verify. */
        if (p[i] == 0xFF && p[i + 1] == 0xD8 && p[i + 2] == 0xFF) {
            size_t jl = jpeg_span(p + i, n - i);
            if (jl >= 256) {
                char tp[1024];
                snprintf(tp, sizeof(tp), "%s.pdfj.%zu", tmp_prefix, i);
                Buf jb; buf_init(&jb);
                if (try_jpeg_buf(p + i, jl, tp, &jb) == 0) {
                    buf_u8(recipe, OP_STRUCT);
                    buf_u32(recipe, (uint32_t)(i - cursor));
                    buf_append(recipe, p + cursor, i - cursor);
                    buf_u8(recipe, OP_JPEG_STORE);
                    buf_u32(recipe, (uint32_t)jl);
                    buf_u32(recipe, (uint32_t)jb.n);
                    buf_append(recipe, jb.p, jb.n);
                    jpegs++;
                    covered += jl;
                    cursor = i + jl;
                    i = cursor - 1;
                    buf_free(&jb);
                    continue;
                }
                buf_free(&jb);
            }
        }
        /* zlib header candidate: CM=8 (deflate) with any window size (real
         * PDFs mix 0x78 and small-window CMFs like 0x48 -- Type-1 font
         * programs), FCHECK valid, FDICT unset (preset dictionaries don't
         * occur in PDFs). */
        if ((p[i] & 0x0F) != 8 || (p[i] >> 4) > 7) continue;
        if ((((unsigned)p[i] << 8) + p[i + 1]) % 31 != 0) continue;
        if (p[i + 1] & 0x20) continue;

        size_t consumed = 0, raw_n = 0;
        uint8_t *raw = zlib_inflate_span(p + i, n - i, &consumed, &raw_n);
        if (!raw) continue;
        /* Tiny streams aren't worth a recipe op; false positives that
         * happen to inflate are typically short. */
        if (raw_n < 64 || consumed < 12 || raw_n > 0xFFFFFFFFu) {
            free(raw);
            continue;
        }

        const uint8_t *def = p + i + 2;
        size_t def_n = consumed - 6;
        uint8_t bk = bucket_for_bytes(raw, raw_n);
        int op = -1;
        Buf diff; buf_init(&diff);

        size_t redef_n = 0;
        uint8_t *redef = raw_deflate_l9(raw, (uint32_t)raw_n, &redef_n);
        if (redef && redef_n == def_n && memcmp(redef, def, def_n) == 0)
            op = OP_REDEFLATE;
        free(redef);

        uint8_t lp0 = 0, lp1 = 0;
        if (op < 0 && redeflate_ladder_find(raw, (uint32_t)raw_n, def,
                                            (uint32_t)def_n, &lp0, &lp1))
            op = OP_REDEFLATE_P;

        if (op < 0) {
            uint8_t *unp = NULL, *df = NULL, *rejoin = NULL;
            size_t unp_n = 0, df_n = 0, rejoin_n = 0;
            if (zxle_preflate_split(def, def_n, &unp, &unp_n, &df, &df_n)) {
                if (unp_n == raw_n && memcmp(unp, raw, raw_n) == 0 &&
                    zxle_preflate_join(unp, unp_n, df, df_n, &rejoin, &rejoin_n) &&
                    rejoin_n == def_n &&
                    memcmp(rejoin, def, def_n) == 0) {
                    buf_append(&diff, df, df_n);
                    op = OP_PREFLATE;
                }
            }
            zxle_preflate_free(unp);
            zxle_preflate_free(df);
            zxle_preflate_free(rejoin);
        }

        if (op < 0) {
            free(raw);
            buf_free(&diff);
            continue;
        }

        /* Gap since the last stream plus the 2-byte zlib header. */
        buf_u8(recipe, OP_STRUCT);
        buf_u32(recipe, (uint32_t)(i + 2 - cursor));
        buf_append(recipe, p + cursor, i + 2 - cursor);

        buf_u8(recipe, (uint8_t)op);
        buf_u32(recipe, (uint32_t)raw_n);
        buf_u8(recipe, bk);
        if (op == OP_PREFLATE) {
            buf_u32(recipe, (uint32_t)diff.n);
            buf_append(recipe, diff.p, diff.n);
            preflated++;
        } else if (op == OP_REDEFLATE_P) {
            buf_u8(recipe, lp0);
            buf_u8(recipe, lp1);
            redeflated++;
        } else {
            redeflated++;
        }
        buf_append(bk == 1 ? b1 : b0, raw, raw_n);

        /* adler32 trailer, verbatim. */
        buf_u8(recipe, OP_STRUCT);
        buf_u32(recipe, 4);
        buf_append(recipe, p + i + consumed - 4, 4);

        covered += consumed;
        cursor = i + consumed;
        i = cursor - 1;
        free(raw);
        buf_free(&diff);
    }

    if (redeflated + preflated + jpegs == 0 ||
        (uint64_t)covered * 1000 < (uint64_t)n * min_cover_permille) {
        b0->n = b0n; b1->n = b1n; recipe->n = rn;
        return -1;
    }

    if (cursor < n) {
        buf_u8(recipe, OP_STRUCT);
        buf_u32(recipe, (uint32_t)(n - cursor));
        buf_append(recipe, p + cursor, n - cursor);
    }

    fprintf(stderr, "    flate: %d streams (%d redeflate, %d preflate, %d jpeg)\n",
            redeflated + preflated + jpegs, redeflated, preflated, jpegs);
    return 0;
}

int pack_pdf(const uint8_t *p, size_t n, const char *tmp_prefix,
             Buf *recipe, Buf *b0, Buf *b1) {
    if (n < 32 || memcmp(p, "%PDF-", 5) != 0) return -1;
    return pack_flate_scan(p, n, tmp_prefix, recipe, b0, b1, 5, 0);
}
