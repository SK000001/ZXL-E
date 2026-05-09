#include "bz2.h"
#include "kinds.h"
#include "tar.h"
#include "recipe.h"

int pack_bz2(const uint8_t *p, size_t n, const char *tmp_prefix,
             Buf *recipe, Buf *b0, Buf *b1, uint8_t bucket) {
    if (n < 14) return -1;
    if (p[0] != 'B' || p[1] != 'Z' || p[2] != 'h') return -1;
    if (p[3] < '1' || p[3] > '9') return -1;
    if (n > 0xFFFFFFFFu) return -1;
    uint8_t block_size = p[3];

    char in_bz2[1024], raw_path[1024], rt_bz2[1024], cmd[4096];
    snprintf(in_bz2,   sizeof(in_bz2),   "%s.in.bz2",  tmp_prefix);
    snprintf(raw_path, sizeof(raw_path), "%s.raw.bin", tmp_prefix);
    snprintf(rt_bz2,   sizeof(rt_bz2),   "%s.rt.bz2",  tmp_prefix);

    FILE *bf = fopen(in_bz2, "wb");
    if (!bf) return -1;
    if (n > 0 && fwrite(p, 1, n, bf) != n) { fclose(bf); unlink(in_bz2); return -1; }
    fclose(bf);

    snprintf(cmd, sizeof(cmd), "bzip2 -dc \"%s\" > \"%s\" 2>%s",
             in_bz2, raw_path, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(in_bz2); unlink(raw_path); return -1; }

    size_t raw_n = 0;
    uint8_t *raw = read_whole_file(raw_path, &raw_n);
    if (!raw || raw_n > 0xFFFFFFFFu) {
        free(raw); unlink(in_bz2); unlink(raw_path); return -1;
    }

    snprintf(cmd, sizeof(cmd), "bzip2 -%c -c \"%s\" > \"%s\" 2>%s",
             (char)block_size, raw_path, rt_bz2, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) {
        free(raw); unlink(in_bz2); unlink(raw_path); unlink(rt_bz2); return -1;
    }
    size_t rt_n = 0;
    uint8_t *rt = read_whole_file(rt_bz2, &rt_n);
    int ok = (rt && rt_n == n && memcmp(rt, p, n) == 0);
    free(rt);
    unlink(in_bz2); unlink(rt_bz2);
    if (!ok) { free(raw); unlink(raw_path); return -1; }

    bucket = bucket_for_bytes(raw, raw_n);
    Buf *body_buf = (bucket == 1) ? b1 : b0;

    int inner_kind = 0;
    Buf tar_recipe; buf_init(&tar_recipe);
    Buf tar_b0; buf_init(&tar_b0);
    Buf tar_b1; buf_init(&tar_b1);
    if (raw_n >= 1024 && raw_n % 512 == 0 &&
        memcmp(raw + 257, "ustar", 5) == 0) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.bz2tar", tmp_prefix);
        if (pack_tar(raw, raw_n, tp, &tar_recipe, &tar_b0, &tar_b1) == 0) {
            inner_kind = 1;
        } else {
            buf_free(&tar_recipe); buf_init(&tar_recipe);
            buf_free(&tar_b0); buf_init(&tar_b0);
            buf_free(&tar_b1); buf_init(&tar_b1);
        }
    }

    buf_u8(recipe, block_size);
    buf_u32(recipe, (uint32_t)raw_n);
    buf_u32(recipe, (uint32_t)n);
    buf_u8(recipe, (uint8_t)inner_kind);
    buf_u8(recipe, bucket);
    if (inner_kind == 1) {
        buf_u32(recipe, (uint32_t)tar_recipe.n);
        buf_append(recipe, tar_recipe.p, tar_recipe.n);
        buf_append(b0, tar_b0.p, tar_b0.n);
        buf_append(b1, tar_b1.p, tar_b1.n);
    } else {
        buf_append(body_buf, raw, raw_n);
    }

    fprintf(stderr, "    bz2: block=%c orig=%zu raw=%zu%s b=%u\n",
            (char)block_size, n, raw_n,
            inner_kind == 1 ? " inner=tar" : "",
            (unsigned)bucket);

    free(raw);
    unlink(raw_path);
    buf_free(&tar_recipe);
    buf_free(&tar_b0);
    buf_free(&tar_b1);
    return 0;
}

void unpack_bz2(const uint8_t *recipe, size_t rlen,
                Solids *s,
                FILE *out, uint64_t expected_size, const char *tmp_prefix) {
    size_t r = 0;
    if (r + 1 + 4 + 4 + 1 + 1 > rlen) die("bz2 recipe truncated");
    uint8_t block_size = recipe[r]; r += 1;
    uint32_t raw_len   = r32(recipe + r); r += 4;
    uint32_t orig_len  = r32(recipe + r); r += 4;
    uint8_t inner_kind = recipe[r]; r += 1;
    uint8_t bucket     = recipe[r]; r += 1;
    if (bucket >= ZXLE_NUM_BUCKETS) die("bz2 bucket oob");
    const uint8_t *tar_recipe = NULL; uint32_t tar_recipe_len = 0;
    if (inner_kind == 1) {
        if (r + 4 > rlen) die("bz2 tar recipe len truncated");
        tar_recipe_len = r32(recipe + r); r += 4;
        if (r + tar_recipe_len > rlen) die("bz2 tar recipe overflow");
        tar_recipe = recipe + r; r += tar_recipe_len;
    }
    if (r != rlen) die("bz2 recipe trailing bytes");
    if (block_size < '1' || block_size > '9') die("bz2 block size");

    char raw_path[2048], rt_bz2[2048], cmd[4096];
    snprintf(raw_path, sizeof(raw_path), "%s.bz2.raw.tmp", tmp_prefix);
    snprintf(rt_bz2,   sizeof(rt_bz2),   "%s.bz2.rt.tmp",  tmp_prefix);

    FILE *rf = fopen(raw_path, "wb");
    if (!rf) die("fopen bz2 raw tmp");
    if (inner_kind == 0) {
        if (s->pos[bucket] + raw_len > s->len[bucket]) die("bz2 solid overflow");
        if (raw_len > 0 && fwrite(s->p[bucket] + s->pos[bucket], 1, raw_len, rf) != raw_len)
            die("fwrite bz2 raw");
        s->pos[bucket] += raw_len;
        fclose(rf);
    } else {
        unpack_recipe(tar_recipe, tar_recipe_len, s, rf, raw_len, raw_path);
        fclose(rf);
    }

    snprintf(cmd, sizeof(cmd), "bzip2 -%c -c \"%s\" > \"%s\" 2>%s",
             (char)block_size, raw_path, rt_bz2, ZXLE_DEVNULL);
    run(cmd);
    unlink(raw_path);

    size_t got_n = 0;
    uint8_t *got = read_whole_file(rt_bz2, &got_n);
    unlink(rt_bz2);
    if (!got || got_n != orig_len) die("bz2 size mismatch");
    if (got_n != expected_size) die("bz2 expected size mismatch");
    if (got_n > 0 && fwrite(got, 1, got_n, out) != got_n) die("fwrite bz2 out");
    free(got);
}
