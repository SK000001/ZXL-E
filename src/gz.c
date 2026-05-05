#include "gz.h"
#include "deflate.h"
#include "preflate_shim.h"
#include "tar.h"
#include "recipe.h"
#include <zlib.h>

int pack_gz(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid) {
    if (n < 18) return -1;
    if (p[0] != 0x1F || p[1] != 0x8B || p[2] != 0x08) return -1;
    uint8_t flg = p[3];
    if (flg & 0xE0) return -1;
    size_t hdr = 10;
    if (flg & 0x04) {
        if (hdr + 2 > n) return -1;
        uint16_t xlen = (uint16_t)(p[hdr] | (p[hdr+1] << 8));
        hdr += 2 + xlen;
        if (hdr > n) return -1;
    }
    if (flg & 0x08) {
        while (hdr < n && p[hdr] != 0) hdr++;
        if (hdr >= n) return -1;
        hdr++;
    }
    if (flg & 0x10) {
        while (hdr < n && p[hdr] != 0) hdr++;
        if (hdr >= n) return -1;
        hdr++;
    }
    if (flg & 0x02) {
        if (hdr + 2 > n) return -1;
        hdr += 2;
    }
    if (hdr + 8 > n) return -1;
    size_t def_n = n - hdr - 8;
    const uint8_t *def = p + hdr;
    const uint8_t *trailer = p + n - 8;

    size_t raw_n = 0;
    uint8_t *raw = raw_inflate_dyn(def, def_n, &raw_n);
    if (!raw) return -1;
    if (raw_n > 0xFFFFFFFFu) { free(raw); return -1; }

    uint32_t want_crc = r32(trailer);
    uint32_t want_isize = r32(trailer + 4);
    uLong c = crc32(0L, Z_NULL, 0);
    if (raw_n > 0) c = crc32(c, raw, (uInt)raw_n);
    if ((uint32_t)c != want_crc || (uint32_t)(raw_n & 0xFFFFFFFFu) != want_isize) {
        free(raw); return -1;
    }

    int mode = -1;
    Buf preflate_diff; buf_init(&preflate_diff);

    size_t redef_n = 0;
    uint8_t *redef = raw_deflate_l9(raw, (uint32_t)raw_n, &redef_n);
    if (redef && redef_n == def_n && memcmp(redef, def, def_n) == 0) {
        mode = 0;
    }
    free(redef);

    if (mode < 0) {
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
        free(raw); buf_free(&preflate_diff); return -1;
    }

    int inner_kind = 0;
    Buf tar_recipe; buf_init(&tar_recipe);
    Buf tar_solid;  buf_init(&tar_solid);
    if (raw_n >= 1024 && raw_n % 512 == 0 &&
        memcmp(raw + 257, "ustar", 5) == 0) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.gztar", tmp_prefix);
        if (pack_tar(raw, raw_n, tp, &tar_recipe, &tar_solid) == 0) {
            inner_kind = 1;
        } else {
            buf_free(&tar_recipe); buf_init(&tar_recipe);
            buf_free(&tar_solid);  buf_init(&tar_solid);
        }
    }

    buf_u32(recipe, (uint32_t)hdr);
    buf_append(recipe, p, hdr);
    buf_u8(recipe, (uint8_t)mode);
    buf_u32(recipe, (uint32_t)raw_n);
    buf_u32(recipe, (uint32_t)def_n);
    if (mode == 1) {
        buf_u32(recipe, (uint32_t)preflate_diff.n);
        buf_append(recipe, preflate_diff.p, preflate_diff.n);
    }
    buf_append(recipe, trailer, 8);
    buf_u8(recipe, (uint8_t)inner_kind);
    if (inner_kind == 1) {
        buf_u32(recipe, (uint32_t)tar_recipe.n);
        buf_append(recipe, tar_recipe.p, tar_recipe.n);
        buf_append(solid, tar_solid.p, tar_solid.n);
    } else {
        buf_append(solid, raw, raw_n);
    }

    fprintf(stderr, "    gz: hdr=%zu def=%zu raw=%zu mode=%d%s%s\n",
            hdr, def_n, raw_n, mode,
            mode == 1 ? " (preflate)" : " (l9)",
            inner_kind == 1 ? " inner=tar" : "");

    free(raw);
    buf_free(&preflate_diff);
    buf_free(&tar_recipe);
    buf_free(&tar_solid);
    return 0;
}

void unpack_gz(const uint8_t *recipe, size_t rlen,
               const uint8_t *solid, size_t solid_len, size_t *solid_pos,
               FILE *out, uint64_t expected_size, const char *tmp_prefix) {
    size_t r = 0;
    if (r + 4 > rlen) die("gz recipe truncated");
    uint32_t hdr_len = r32(recipe + r); r += 4;
    if (r + hdr_len > rlen) die("gz hdr overflow");
    const uint8_t *hdr = recipe + r; r += hdr_len;
    if (r + 1 + 4 + 4 > rlen) die("gz header truncated");
    uint8_t mode = recipe[r]; r += 1;
    uint32_t raw_len = r32(recipe + r); r += 4;
    uint32_t def_len = r32(recipe + r); r += 4;
    const uint8_t *diff = NULL; uint32_t diff_len = 0;
    if (mode == 1) {
        if (r + 4 > rlen) die("gz diff len truncated");
        diff_len = r32(recipe + r); r += 4;
        if (r + diff_len > rlen) die("gz diff overflow");
        diff = recipe + r; r += diff_len;
    }
    if (r + 8 > rlen) die("gz trailer truncated");
    const uint8_t *trailer = recipe + r; r += 8;
    if (r + 1 > rlen) die("gz inner_kind truncated");
    uint8_t inner_kind = recipe[r]; r += 1;
    const uint8_t *tar_recipe = NULL; uint32_t tar_recipe_len = 0;
    if (inner_kind == 1) {
        if (r + 4 > rlen) die("gz tar recipe len truncated");
        tar_recipe_len = r32(recipe + r); r += 4;
        if (r + tar_recipe_len > rlen) die("gz tar recipe overflow");
        tar_recipe = recipe + r; r += tar_recipe_len;
    }
    if (r != rlen) die("gz recipe trailing bytes");

    uint8_t *raw_buf = NULL;
    const uint8_t *raw = NULL;
    if (inner_kind == 0) {
        if (*solid_pos + raw_len > solid_len) die("gz solid overflow");
        raw = solid + *solid_pos;
        *solid_pos += raw_len;
    } else {
        char tp[2048];
        snprintf(tp, sizeof(tp), "%s.gztar.tmp", tmp_prefix);
        FILE *tf = fopen(tp, "wb");
        if (!tf) die("fopen gz tar tmp");
        unpack_recipe(tar_recipe, tar_recipe_len,
                      solid, solid_len, solid_pos,
                      tf, raw_len, tp);
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
    } else {
        if (!zxle_preflate_join(raw, raw_len, diff, diff_len, &def_buf, &def_n))
            die("gz preflate_join");
    }
    if (def_n != def_len) die("gz def size mismatch");

    if (hdr_len > 0 && fwrite(hdr, 1, hdr_len, out) != hdr_len) die("fwrite gz hdr");
    if (def_n > 0 && fwrite(def_buf, 1, def_n, out) != def_n) die("fwrite gz body");
    if (fwrite(trailer, 1, 8, out) != 8) die("fwrite gz trailer");

    if (mode == 0) free(def_buf); else zxle_preflate_free(def_buf);
    free(raw_buf);
    uint64_t written = (uint64_t)hdr_len + def_n + 8;
    if (written != expected_size) die("gz size mismatch");
}
