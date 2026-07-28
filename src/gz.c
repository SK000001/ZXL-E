#include "gz.h"
#include "kinds.h"
#include "deflate.h"
#include "preflate_shim.h"
#include "tar.h"
#include "recipe.h"
#include <zlib.h>

/* Parse a gzip member header starting at p[cur..n); returns the header length
 * (bytes) or (size_t)-1 on malformed magic/flags/overflow. */
static size_t gz_hdr_len(const uint8_t *p, size_t n, size_t cur) {
    if (cur + 10 > n) return (size_t)-1;
    if (p[cur] != 0x1F || p[cur+1] != 0x8B || p[cur+2] != 0x08) return (size_t)-1;
    uint8_t flg = p[cur+3];
    if (flg & 0xE0) return (size_t)-1;
    size_t h = cur + 10;
    if (flg & 0x04) {
        if (h + 2 > n) return (size_t)-1;
        uint16_t xlen = (uint16_t)(p[h] | (p[h+1] << 8));
        h += 2 + xlen; if (h > n) return (size_t)-1;
    }
    if (flg & 0x08) { while (h < n && p[h] != 0) h++; if (h >= n) return (size_t)-1; h++; }
    if (flg & 0x10) { while (h < n && p[h] != 0) h++; if (h >= n) return (size_t)-1; h++; }
    if (flg & 0x02) { if (h + 2 > n) return (size_t)-1; h += 2; }
    return h - cur;
}

/* Decide the reproduction mode for one member's deflate body: 0 = raw-deflate
 * L9 matches, 2 = redeflate ladder (fills p0,p1), 1 = preflate (fills diff),
 * -1 = neither. */
static int gz_member_mode(const uint8_t *def, size_t def_n,
                          const uint8_t *raw, size_t raw_n, Buf *diff,
                          uint8_t *p0, uint8_t *p1) {
    *p0 = 0; *p1 = 0;
    size_t rn = 0;
    uint8_t *redef = raw_deflate_l9(raw, (uint32_t)raw_n, &rn);
    int mode = -1;
    if (redef && rn == def_n && memcmp(redef, def, def_n) == 0) mode = 0;
    free(redef);
    if (mode < 0 && def_n <= 0xFFFFFFFFu &&
        redeflate_ladder_find(raw, (uint32_t)raw_n, def, (uint32_t)def_n, p0, p1))
        mode = 2;
    if (mode < 0) {
        uint8_t *unp = NULL, *df = NULL, *rj = NULL;
        size_t un = 0, dn = 0, rjn = 0;
        if (zxle_preflate_split(def, def_n, &unp, &un, &df, &dn)) {
            if (un == raw_n && memcmp(unp, raw, raw_n) == 0 &&
                zxle_preflate_join(unp, un, df, dn, &rj, &rjn) &&
                rjn == def_n && memcmp(rj, def, def_n) == 0) {
                buf_append(diff, df, dn); mode = 1;
            }
        }
        zxle_preflate_free(unp); zxle_preflate_free(df); zxle_preflate_free(rj);
    }
    return mode;
}

/* Emit one member block (inner_kind == 0) into recipe and its body into the
 * chosen bucket. Shared by the single-member and multi-member paths. */
static void emit_gz_member(Buf *recipe, Buf *b0, Buf *b1,
                           const uint8_t *hdr, size_t hdr_len, int mode,
                           const uint8_t *raw, size_t raw_n, size_t def_n,
                           const Buf *diff, uint8_t p0, uint8_t p1,
                           const uint8_t *trailer, uint8_t bucket) {
    buf_u32(recipe, (uint32_t)hdr_len);
    buf_append(recipe, hdr, hdr_len);
    buf_u8(recipe, (uint8_t)mode);
    buf_u32(recipe, (uint32_t)raw_n);
    buf_u32(recipe, (uint32_t)def_n);
    if (mode == 1) { buf_u32(recipe, (uint32_t)diff->n); buf_append(recipe, diff->p, diff->n); }
    else if (mode == 2) { buf_u8(recipe, p0); buf_u8(recipe, p1); }
    buf_append(recipe, trailer, 8);
    buf_u8(recipe, 0);          /* inner_kind = 0 */
    buf_u8(recipe, bucket);
    buf_append(bucket == 1 ? b1 : b0, raw, raw_n);
}

/* One member: parse header + span-inflate + verify crc/isize. Fills the out
 * pointers/lengths (raw is malloc'd, caller frees). Returns 0 or -1. */
static int gz_read_member(const uint8_t *p, size_t n, size_t cur,
                          size_t *hdr_len, uint8_t **raw, size_t *raw_n,
                          size_t *consumed, const uint8_t **trailer) {
    size_t h = gz_hdr_len(p, n, cur);
    if (h == (size_t)-1) return -1;
    size_t cons = 0, rn = 0;
    uint8_t *r = raw_inflate_span(p + cur + h, n - cur - h, &cons, &rn);
    if (!r) return -1;
    if (rn > 0xFFFFFFFFu || cur + h + cons + 8 > n) { free(r); return -1; }
    const uint8_t *tr = p + cur + h + cons;
    uLong c = crc32(0L, Z_NULL, 0);
    if (rn > 0) c = crc32(c, r, (uInt)rn);
    if ((uint32_t)c != r32(tr) || (uint32_t)(rn & 0xFFFFFFFFu) != r32(tr + 4)) { free(r); return -1; }
    *hdr_len = h; *raw = r; *raw_n = rn; *consumed = cons; *trailer = tr;
    return 0;
}

int pack_gz(const uint8_t *p, size_t n, const char *tmp_prefix,
            Buf *recipe, Buf *b0, Buf *b1, uint8_t bucket) {
    (void)bucket;
    if (n < 18) return -1;

    size_t hdr_len = 0, consumed = 0, raw_n = 0;
    uint8_t *raw = NULL; const uint8_t *trailer = NULL;
    if (gz_read_member(p, n, 0, &hdr_len, &raw, &raw_n, &consumed, &trailer) != 0)
        return -1;
    const uint8_t *def = p + hdr_len;
    size_t def_n = consumed;
    size_t member1_end = hdr_len + consumed + 8;

    if (member1_end == n) {
        /* ---- single member: full behavior incl. inner-tar dispatch ---- */
        Buf diff; buf_init(&diff);
        uint8_t p0 = 0, p1 = 0;
        int mode = gz_member_mode(def, def_n, raw, raw_n, &diff, &p0, &p1);
        if (mode < 0) { free(raw); buf_free(&diff); return -1; }
        uint8_t bk = bucket_for_bytes(raw, raw_n);

        int inner_kind = 0;
        Buf tar_recipe, tar_b0, tar_b1;
        buf_init(&tar_recipe); buf_init(&tar_b0); buf_init(&tar_b1);
        if (raw_n >= 1024 && raw_n % 512 == 0 && memcmp(raw + 257, "ustar", 5) == 0) {
            char tp[1024]; snprintf(tp, sizeof(tp), "%s.gztar", tmp_prefix);
            if (pack_tar(raw, raw_n, tp, &tar_recipe, &tar_b0, &tar_b1) == 0) {
                inner_kind = 1;
            } else {
                buf_free(&tar_recipe); buf_init(&tar_recipe);
                buf_free(&tar_b0); buf_init(&tar_b0);
                buf_free(&tar_b1); buf_init(&tar_b1);
            }
        }

        buf_u32(recipe, 1);            /* n_members */
        if (inner_kind == 1) {
            buf_u32(recipe, (uint32_t)hdr_len);
            buf_append(recipe, p, hdr_len);
            buf_u8(recipe, (uint8_t)mode);
            buf_u32(recipe, (uint32_t)raw_n);
            buf_u32(recipe, (uint32_t)def_n);
            if (mode == 1) { buf_u32(recipe, (uint32_t)diff.n); buf_append(recipe, diff.p, diff.n); }
            else if (mode == 2) { buf_u8(recipe, p0); buf_u8(recipe, p1); }
            buf_append(recipe, trailer, 8);
            buf_u8(recipe, 1);        /* inner_kind */
            buf_u8(recipe, bk);
            buf_u32(recipe, (uint32_t)tar_recipe.n);
            buf_append(recipe, tar_recipe.p, tar_recipe.n);
            buf_append(b0, tar_b0.p, tar_b0.n);
            buf_append(b1, tar_b1.p, tar_b1.n);
        } else {
            emit_gz_member(recipe, b0, b1, p, hdr_len, mode, raw, raw_n, def_n, &diff, p0, p1, trailer, bk);
        }
        fprintf(stderr, "    gz: 1 member def=%lu raw=%lu mode=%d%s b=%u\n",
                (unsigned long)def_n, (unsigned long)raw_n, mode,
                inner_kind == 1 ? " inner=tar" : "", (unsigned)bk);
        free(raw); buf_free(&diff);
        buf_free(&tar_recipe); buf_free(&tar_b0); buf_free(&tar_b1);
        return 0;
    }

    /* ---- multi-member (BGZF / pgzip / concatenated gzip) ---- */
    Buf rtmp, tb0, tb1; buf_init(&rtmp); buf_init(&tb0); buf_init(&tb1);
    uint32_t n_members = 0;
    int ok = 1;

    /* member 1 (already inflated above) */
    {
        Buf diff; buf_init(&diff);
        uint8_t p0 = 0, p1 = 0;
        int mode = gz_member_mode(def, def_n, raw, raw_n, &diff, &p0, &p1);
        if (mode < 0) { ok = 0; }
        else {
            uint8_t bk = bucket_for_bytes(raw, raw_n);
            emit_gz_member(&rtmp, &tb0, &tb1, p, hdr_len, mode, raw, raw_n, def_n, &diff, p0, p1, trailer, bk);
            n_members = 1;
        }
        buf_free(&diff);
    }
    free(raw);

    size_t cur = member1_end;
    while (ok && cur < n) {
        size_t hk = 0, ck = 0, rk = 0; uint8_t *rawk = NULL; const uint8_t *tk = NULL;
        if (gz_read_member(p, n, cur, &hk, &rawk, &rk, &ck, &tk) != 0) { ok = 0; break; }
        Buf diff; buf_init(&diff);
        uint8_t p0 = 0, p1 = 0;
        int mode = gz_member_mode(p + cur + hk, ck, rawk, rk, &diff, &p0, &p1);
        if (mode < 0) { ok = 0; }
        else {
            uint8_t bk = bucket_for_bytes(rawk, rk);
            emit_gz_member(&rtmp, &tb0, &tb1, p + cur, hk, mode, rawk, rk, ck, &diff, p0, p1, tk, bk);
            n_members++;
            cur += hk + ck + 8;
        }
        buf_free(&diff); free(rawk);
    }

    if (!ok || cur != n || n_members < 2) {
        buf_free(&rtmp); buf_free(&tb0); buf_free(&tb1); return -1;
    }

    buf_u32(recipe, n_members);
    buf_append(recipe, rtmp.p, rtmp.n);
    buf_append(b0, tb0.p, tb0.n);
    buf_append(b1, tb1.p, tb1.n);
    fprintf(stderr, "    gz: %u members (multi-member)\n", n_members);
    buf_free(&rtmp); buf_free(&tb0); buf_free(&tb1);
    return 0;
}

void unpack_gz(const uint8_t *recipe, size_t rlen,
               Solids *s,
               FILE *out, uint64_t expected_size, const char *tmp_prefix) {
    size_t r = 0;
    if (r + 4 > rlen) die("gz members truncated");
    uint32_t n_members = r32(recipe + r); r += 4;
    if (n_members == 0) die("gz zero members");
    uint64_t written = 0;

    for (uint32_t m = 0; m < n_members; m++) {
        if (r + 4 > rlen) die("gz hdr len truncated");
        uint32_t hdr_len = r32(recipe + r); r += 4;
        if (r + hdr_len > rlen) die("gz hdr overflow");
        const uint8_t *hdr = recipe + r; r += hdr_len;
        if (r + 1 + 4 + 4 > rlen) die("gz header truncated");
        uint8_t mode = recipe[r]; r += 1;
        uint32_t raw_len = r32(recipe + r); r += 4;
        uint32_t def_len = r32(recipe + r); r += 4;
        const uint8_t *diff = NULL; uint32_t diff_len = 0;
        uint8_t lp0 = 0, lp1 = 0;
        if (mode == 1) {
            if (r + 4 > rlen) die("gz diff len truncated");
            diff_len = r32(recipe + r); r += 4;
            if (r + diff_len > rlen) die("gz diff overflow");
            diff = recipe + r; r += diff_len;
        } else if (mode == 2) {
            if (r + 2 > rlen) die("gz ladder params truncated");
            lp0 = recipe[r]; lp1 = recipe[r + 1]; r += 2;
        }
        if (r + 8 > rlen) die("gz trailer truncated");
        const uint8_t *trailer = recipe + r; r += 8;
        if (r + 1 + 1 > rlen) die("gz inner_kind/bucket truncated");
        uint8_t inner_kind = recipe[r]; r += 1;
        uint8_t bucket = recipe[r]; r += 1;
        if (bucket >= ZXLE_NUM_BUCKETS) die("gz bucket oob");
        const uint8_t *tar_recipe = NULL; uint32_t tar_recipe_len = 0;
        if (inner_kind == 1) {
            if (r + 4 > rlen) die("gz tar recipe len truncated");
            tar_recipe_len = r32(recipe + r); r += 4;
            if (r + tar_recipe_len > rlen) die("gz tar recipe overflow");
            tar_recipe = recipe + r; r += tar_recipe_len;
        }

        uint8_t *raw_buf = NULL;
        const uint8_t *raw = NULL;
        if (inner_kind == 0) {
            if (s->pos[bucket] + raw_len > s->len[bucket]) die("gz solid overflow");
            raw = s->p[bucket] + s->pos[bucket];
            s->pos[bucket] += raw_len;
        } else {
            char tp[2048];
            snprintf(tp, sizeof(tp), "%s.gztar.tmp", tmp_prefix);
            FILE *tf = fopen(tp, "wb");
            if (!tf) die("fopen gz tar tmp");
            unpack_recipe(tar_recipe, tar_recipe_len, s, tf, raw_len, tp);
            fclose(tf);
            size_t got_n = 0;
            raw_buf = read_whole_file(tp, &got_n);
            unlink(tp);
            if (got_n != raw_len) die("gz tar size mismatch");
            raw = raw_buf;
        }

        uint8_t *def_buf = NULL; size_t def_n = 0;
        if (mode == 0) {
            def_buf = raw_deflate_l9(raw, raw_len, &def_n);
            if (!def_buf) die("gz raw_deflate_l9");
        } else if (mode == 2) {
            def_buf = redeflate_ladder_apply(raw, raw_len, lp0, lp1, &def_n);
            if (!def_buf) die("gz redeflate_ladder_apply");
        } else {
            if (!zxle_preflate_join(raw, raw_len, diff, diff_len, &def_buf, &def_n))
                die("gz preflate_join");
        }
        if (def_n != def_len) die("gz def size mismatch");

        if (hdr_len > 0 && fwrite(hdr, 1, hdr_len, out) != hdr_len) die("fwrite gz hdr");
        if (def_n > 0 && fwrite(def_buf, 1, def_n, out) != def_n) die("fwrite gz body");
        if (fwrite(trailer, 1, 8, out) != 8) die("fwrite gz trailer");

        if (mode == 1) zxle_preflate_free(def_buf); else free(def_buf);
        free(raw_buf);
        written += (uint64_t)hdr_len + def_n + 8;
    }

    if (r != rlen) die("gz recipe trailing bytes");
    if (written != expected_size) die("gz size mismatch");
}
