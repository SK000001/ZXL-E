#include "recipe.h"
#include "kinds.h"
#include "deflate.h"
#include "preflate_shim.h"
#include "png.h"
#include "gz.h"
#include "bz2.h"
#include "zst.h"
#include "xz.h"

void unpack_recipe(const uint8_t *recipe, size_t rlen,
                   Solids *s,
                   FILE *out, uint64_t expected_size,
                   const char *tmp_prefix) {
    size_t r = 0;
    uint64_t written = 0;
    while (r < rlen) {
        if (r + 5 > rlen) die("recipe truncated");
        uint8_t op = recipe[r]; r += 1;
        uint32_t len = r32(recipe + r); r += 4;
        if (op == OP_STRUCT) {
            if (r + len > rlen) die("recipe STRUCT overflow");
            if (fwrite(recipe + r, 1, len, out) != len) die("fwrite STRUCT");
            r += len;
            written += len;
        } else if (op == OP_REDEFLATE) {
            if (r + 1 > rlen) die("recipe REDEFLATE bucket truncated");
            uint8_t bk = recipe[r]; r += 1;
            if (bk >= ZXLE_NUM_BUCKETS) die("REDEFLATE bucket oob");
            if (s->pos[bk] + len > s->len[bk]) die("solid REDEFLATE overflow");
            size_t df_len = 0;
            uint8_t *df = raw_deflate_l9(s->p[bk] + s->pos[bk], len, &df_len);
            if (!df) die("raw_deflate_l9");
            if (fwrite(df, 1, df_len, out) != df_len) die("fwrite REDEFLATE");
            free(df);
            s->pos[bk] += len;
            written += df_len;
        } else if (op == OP_STORE) {
            if (r + 1 > rlen) die("recipe STORE bucket truncated");
            uint8_t bk = recipe[r]; r += 1;
            if (bk >= ZXLE_NUM_BUCKETS) die("STORE bucket oob");
            if (s->pos[bk] + len > s->len[bk]) die("solid STORE overflow");
            if (fwrite(s->p[bk] + s->pos[bk], 1, len, out) != len) die("fwrite STORE");
            s->pos[bk] += len;
            written += len;
        } else if (op == OP_JPEG_STORE) {
            if (r + 4 > rlen) die("recipe JPEG_STORE brn_len truncated");
            uint32_t brn_len = r32(recipe + r); r += 4;
            if (r + brn_len > rlen) die("recipe JPEG_STORE brn overflow");
            char tmp_brn[2048], tmp_jpg[2048], cmd[4096];
            snprintf(tmp_brn, sizeof(tmp_brn), "%s.uj.brn.tmp", tmp_prefix);
            snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.uj.jpg.tmp", tmp_prefix);
            FILE *bf = fopen(tmp_brn, "wb");
            if (!bf) die("fopen tmp brn");
            if (brn_len > 0 && fwrite(recipe + r, 1, brn_len, bf) != brn_len) die("fwrite tmp brn");
            fclose(bf);
            snprintf(cmd, sizeof(cmd), "dbrunsli \"%s\" \"%s\" >%s 2>&1", tmp_brn, tmp_jpg, ZXLE_DEVNULL);
            run(cmd);
            unlink(tmp_brn);
            size_t got_n = 0;
            uint8_t *got = read_whole_file(tmp_jpg, &got_n);
            unlink(tmp_jpg);
            if (got_n != len) die("JPEG_STORE size mismatch");
            if (fwrite(got, 1, got_n, out) != got_n) die("fwrite JPEG_STORE");
            free(got);
            r += brn_len;
            written += len;
        } else if (op == OP_PNG_STORE) {
            if (r + 4 > rlen) die("recipe PNG_STORE recipe_len truncated");
            uint32_t prl = r32(recipe + r); r += 4;
            if (r + prl > rlen) die("recipe PNG_STORE recipe overflow");
            unpack_png(recipe + r, prl, s, out, len);
            r += prl;
            written += len;
        } else if (op == OP_GZIP_STORE) {
            if (r + 4 > rlen) die("recipe GZIP_STORE recipe_len truncated");
            uint32_t grl = r32(recipe + r); r += 4;
            if (r + grl > rlen) die("recipe GZIP_STORE recipe overflow");
            unpack_gz(recipe + r, grl, s, out, len, tmp_prefix);
            r += grl;
            written += len;
        } else if (op == OP_BZ2_STORE) {
            if (r + 4 > rlen) die("recipe BZ2_STORE recipe_len truncated");
            uint32_t brl = r32(recipe + r); r += 4;
            if (r + brl > rlen) die("recipe BZ2_STORE recipe overflow");
            unpack_bz2(recipe + r, brl, s, out, len, tmp_prefix);
            r += brl;
            written += len;
        } else if (op == OP_XZ_STORE) {
            if (r + 4 > rlen) die("recipe XZ_STORE recipe_len truncated");
            uint32_t xrl = r32(recipe + r); r += 4;
            if (r + xrl > rlen) die("recipe XZ_STORE recipe overflow");
            unpack_xz(recipe + r, xrl, s, out, len, tmp_prefix);
            r += xrl;
            written += len;
        } else if (op == OP_ZSTD_STORE) {
            if (r + 4 > rlen) die("recipe ZSTD_STORE recipe_len truncated");
            uint32_t zrl = r32(recipe + r); r += 4;
            if (r + zrl > rlen) die("recipe ZSTD_STORE recipe overflow");
            unpack_zst(recipe + r, zrl, s, out, len, tmp_prefix);
            r += zrl;
            written += len;
        } else if (op == OP_ZIP_STORE) {
            /* Nested ZIP recipe shares this OP vocabulary; recurse in place. */
            if (r + 4 > rlen) die("recipe ZIP_STORE recipe_len truncated");
            uint32_t zprl = r32(recipe + r); r += 4;
            if (r + zprl > rlen) die("recipe ZIP_STORE recipe overflow");
            unpack_recipe(recipe + r, zprl, s, out, len, tmp_prefix);
            r += zprl;
            written += len;
        } else if (op == OP_PREFLATE) {
            if (r + 1 > rlen) die("recipe PREFLATE bucket truncated");
            uint8_t bk = recipe[r]; r += 1;
            if (bk >= ZXLE_NUM_BUCKETS) die("PREFLATE bucket oob");
            if (r + 4 > rlen) die("recipe PREFLATE diff_len truncated");
            uint32_t diff_len = r32(recipe + r); r += 4;
            if (r + diff_len > rlen) die("recipe PREFLATE diff overflow");
            if (s->pos[bk] + len > s->len[bk]) die("solid PREFLATE overflow");
            uint8_t *def = NULL; size_t def_n = 0;
            if (!zxle_preflate_join(s->p[bk] + s->pos[bk], len,
                                    recipe + r, diff_len,
                                    &def, &def_n)) die("preflate_reencode");
            if (fwrite(def, 1, def_n, out) != def_n) die("fwrite PREFLATE");
            zxle_preflate_free(def);
            r += diff_len;
            s->pos[bk] += len;
            written += def_n;
        } else {
            die("unknown recipe op");
        }
    }
    if (written != expected_size) die("recipe size mismatch");
}
