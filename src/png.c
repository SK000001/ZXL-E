#include "png.h"
#include "deflate.h"
#include "preflate_shim.h"
#include <zlib.h>

const uint8_t PNG_SIG[8] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};

int pack_png(const uint8_t *p, size_t n, Buf *recipe, Buf *solid) {
    if (n < 8 + 12 || memcmp(p, PNG_SIG, 8) != 0) return -1;

    size_t pre_end = 0;
    size_t post_start = 0;
    Buf zlib_concat; buf_init(&zlib_concat);
    Buf idat_sizes; buf_init(&idat_sizes);
    uint32_t idat_count = 0;
    int seen_idat = 0, seen_post_idat = 0;

    size_t cur = 8;
    while (cur + 12 <= n) {
        uint32_t clen = r32be(p + cur);
        if ((size_t)clen + 12 > n - cur) { buf_free(&zlib_concat); buf_free(&idat_sizes); return -1; }
        const uint8_t *type = p + cur + 4;
        const uint8_t *data = p + cur + 8;
        int is_idat = (type[0]=='I' && type[1]=='D' && type[2]=='A' && type[3]=='T');

        if (is_idat) {
            if (seen_post_idat) { buf_free(&zlib_concat); buf_free(&idat_sizes); return -1; }
            if (!seen_idat) { pre_end = cur; seen_idat = 1; }
            buf_append(&zlib_concat, data, clen);
            buf_u32(&idat_sizes, clen);
            idat_count++;
        } else if (seen_idat) {
            if (!seen_post_idat) { post_start = cur; seen_post_idat = 1; }
        }

        cur += 12 + clen;
        if (type[0]=='I' && type[1]=='E' && type[2]=='N' && type[3]=='D') break;
    }
    if (!seen_idat || idat_count == 0) { buf_free(&zlib_concat); buf_free(&idat_sizes); return -1; }
    if (!seen_post_idat) post_start = cur;

    size_t raw_n = 0;
    uint8_t *raw = zlib_inflate_dyn(zlib_concat.p, zlib_concat.n, &raw_n);
    if (!raw) { buf_free(&zlib_concat); buf_free(&idat_sizes); return -1; }
    if (raw_n > 0xFFFFFFFFu) { free(raw); buf_free(&zlib_concat); buf_free(&idat_sizes); return -1; }

    int mode = -1;
    Buf preflate_diff; buf_init(&preflate_diff);
    uint8_t zhdr[2] = {0,0}, adler[4] = {0,0,0,0};

    size_t redef_n = 0;
    uint8_t *redef = zlib_deflate_l9(raw, raw_n, &redef_n);
    if (redef && redef_n == zlib_concat.n &&
        memcmp(redef, zlib_concat.p, zlib_concat.n) == 0) {
        mode = 0;
    }
    free(redef);

    if (mode < 0 && zlib_concat.n >= 6) {
        zhdr[0] = zlib_concat.p[0];
        zhdr[1] = zlib_concat.p[1];
        memcpy(adler, zlib_concat.p + zlib_concat.n - 4, 4);
        const uint8_t *def = zlib_concat.p + 2;
        size_t def_n = zlib_concat.n - 6;
        uint8_t *unp = NULL, *diff = NULL, *rejoin = NULL;
        size_t unp_n = 0, diff_n = 0, rejoin_n = 0;
        if (zxle_preflate_split(def, def_n, &unp, &unp_n, &diff, &diff_n)) {
            if (unp_n == raw_n && memcmp(unp, raw, raw_n) == 0 &&
                zxle_preflate_join(unp, unp_n, diff, diff_n, &rejoin, &rejoin_n) &&
                rejoin_n == def_n &&
                memcmp(rejoin, def, def_n) == 0) {
                buf_append(&preflate_diff, diff, diff_n);
                mode = 1;
            }
        }
        zxle_preflate_free(unp);
        zxle_preflate_free(diff);
        zxle_preflate_free(rejoin);
    }

    if (mode < 0) {
        free(raw);
        buf_free(&zlib_concat);
        buf_free(&idat_sizes);
        buf_free(&preflate_diff);
        return -1;
    }

    uint32_t pre_len  = (uint32_t)pre_end;
    uint32_t post_len = (uint32_t)(n - post_start);
    buf_u32(recipe, pre_len);
    buf_append(recipe, p, pre_len);
    buf_u32(recipe, idat_count);
    buf_append(recipe, idat_sizes.p, idat_sizes.n);
    buf_u8(recipe, (uint8_t)mode);
    buf_u32(recipe, (uint32_t)raw_n);
    buf_u8(recipe, 0);  /* PNG inflated bytes always go to bucket 0 */
    buf_u32(recipe, (uint32_t)zlib_concat.n);
    if (mode == 1) {
        buf_append(recipe, zhdr, 2);
        buf_append(recipe, adler, 4);
        buf_u32(recipe, (uint32_t)preflate_diff.n);
        buf_append(recipe, preflate_diff.p, preflate_diff.n);
    }
    buf_u32(recipe, post_len);
    buf_append(recipe, p + post_start, post_len);

    buf_append(solid, raw, raw_n);

    fprintf(stderr, "    png: %u IDAT chunk(s), zlib=%zu raw=%zu mode=%d%s\n",
            idat_count, zlib_concat.n, raw_n, mode,
            mode == 1 ? " (preflate)" : " (l9)");

    free(raw);
    buf_free(&zlib_concat);
    buf_free(&idat_sizes);
    buf_free(&preflate_diff);
    return 0;
}

void unpack_png(const uint8_t *recipe, size_t rlen,
                Solids *s,
                FILE *out, uint64_t expected_size) {
    size_t r = 0;
    if (r + 4 > rlen) die("png recipe truncated");
    uint32_t pre_len = r32(recipe + r); r += 4;
    if (r + pre_len > rlen) die("png pre overflow");
    const uint8_t *pre = recipe + r; r += pre_len;
    if (r + 4 > rlen) die("png idat_count truncated");
    uint32_t idat_count = r32(recipe + r); r += 4;
    if (idat_count == 0 || r + (size_t)idat_count * 4 > rlen) die("png idat sizes overflow");
    const uint8_t *idat_sizes = recipe + r; r += (size_t)idat_count * 4;
    if (r + 1 + 4 + 1 + 4 > rlen) die("png header truncated");
    uint8_t mode = recipe[r]; r += 1;
    uint32_t raw_len = r32(recipe + r); r += 4;
    uint8_t bucket = recipe[r]; r += 1;
    if (bucket >= ZXLE_NUM_BUCKETS) die("png bucket oob");
    uint32_t zlib_total = r32(recipe + r); r += 4;
    uint8_t zhdr[2] = {0,0}, adler[4] = {0,0,0,0};
    const uint8_t *diff = NULL; uint32_t diff_len = 0;
    if (mode == 1) {
        if (r + 2 + 4 + 4 > rlen) die("png mode1 header truncated");
        memcpy(zhdr, recipe + r, 2); r += 2;
        memcpy(adler, recipe + r, 4); r += 4;
        diff_len = r32(recipe + r); r += 4;
        if (r + diff_len > rlen) die("png diff overflow");
        diff = recipe + r; r += diff_len;
    }
    if (r + 4 > rlen) die("png post_len truncated");
    uint32_t post_len = r32(recipe + r); r += 4;
    if (r + post_len > rlen) die("png post overflow");
    const uint8_t *post = recipe + r; r += post_len;
    if (r != rlen) die("png recipe trailing bytes");

    if (s->pos[bucket] + raw_len > s->len[bucket]) die("png solid overflow");
    const uint8_t *raw = s->p[bucket] + s->pos[bucket];

    uint8_t *zlib_buf = NULL;
    size_t zlib_n = 0;
    if (mode == 0) {
        zlib_buf = zlib_deflate_l9(raw, raw_len, &zlib_n);
        if (!zlib_buf) die("png zlib_deflate_l9");
    } else {
        uint8_t *def = NULL; size_t def_n = 0;
        if (!zxle_preflate_join(raw, raw_len, diff, diff_len, &def, &def_n))
            die("png preflate_join");
        zlib_n = 2 + def_n + 4;
        zlib_buf = malloc(zlib_n);
        if (!zlib_buf) die("png malloc zlib");
        memcpy(zlib_buf, zhdr, 2);
        memcpy(zlib_buf + 2, def, def_n);
        memcpy(zlib_buf + 2 + def_n, adler, 4);
        zxle_preflate_free(def);
    }
    if (zlib_n != zlib_total) die("png zlib size mismatch");

    size_t sum = 0;
    for (uint32_t i = 0; i < idat_count; i++) sum += r32(idat_sizes + i*4);
    if (sum != zlib_n) die("png idat sizes mismatch");

    if (pre_len > 0 && fwrite(pre, 1, pre_len, out) != pre_len) die("fwrite png pre");
    size_t zoff = 0;
    for (uint32_t i = 0; i < idat_count; i++) {
        uint32_t clen = r32(idat_sizes + i*4);
        uint8_t hdr[8];
        w32be(hdr, clen);
        hdr[4] = 'I'; hdr[5] = 'D'; hdr[6] = 'A'; hdr[7] = 'T';
        if (fwrite(hdr, 1, 8, out) != 8) die("fwrite png idat hdr");
        if (clen > 0 && fwrite(zlib_buf + zoff, 1, clen, out) != clen) die("fwrite png idat data");
        uLong c = crc32(0L, Z_NULL, 0);
        c = crc32(c, hdr + 4, 4);
        if (clen > 0) c = crc32(c, zlib_buf + zoff, clen);
        uint8_t crcb[4]; w32be(crcb, (uint32_t)c);
        if (fwrite(crcb, 1, 4, out) != 4) die("fwrite png idat crc");
        zoff += clen;
    }
    if (post_len > 0 && fwrite(post, 1, post_len, out) != post_len) die("fwrite png post");

    free(zlib_buf);
    s->pos[bucket] += raw_len;
    uint64_t written = (uint64_t)pre_len + (uint64_t)idat_count * 12 + zlib_n + post_len;
    if (written != expected_size) die("png size mismatch");
}
