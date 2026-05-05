#include "xz.h"
#include "tar.h"
#include "recipe.h"

int pack_xz(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid) {
    static const uint8_t XZ_MAGIC[6] = { 0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00 };
    if (n < 12) return -1;
    if (memcmp(p, XZ_MAGIC, 6) != 0) return -1;
    if (n > 0xFFFFFFFFu) return -1;

    char in_xz[1024], raw_path[1024], rt_xz[1024], cmd[4096];
    snprintf(in_xz,    sizeof(in_xz),    "%s.in.xz",   tmp_prefix);
    snprintf(raw_path, sizeof(raw_path), "%s.raw.bin", tmp_prefix);
    snprintf(rt_xz,    sizeof(rt_xz),    "%s.rt.xz",   tmp_prefix);

    FILE *xf = fopen(in_xz, "wb");
    if (!xf) return -1;
    if (n > 0 && fwrite(p, 1, n, xf) != n) { fclose(xf); unlink(in_xz); return -1; }
    fclose(xf);

    snprintf(cmd, sizeof(cmd), "xz -dc \"%s\" > \"%s\" 2>%s",
             in_xz, raw_path, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(in_xz); unlink(raw_path); return -1; }

    size_t raw_n = 0;
    uint8_t *raw = read_whole_file(raw_path, &raw_n);
    if (!raw || raw_n > 0xFFFFFFFFu) {
        free(raw); unlink(in_xz); unlink(raw_path); return -1;
    }

    static const struct { uint8_t level; uint8_t extreme; } ladder[] = {
        {9, 1}, {9, 0},
        {6, 1}, {6, 0},
        {3, 1}, {3, 0},
        {1, 0}, {0, 0},
    };
    int matched = -1;
    for (size_t i = 0; i < sizeof(ladder)/sizeof(ladder[0]); i++) {
        snprintf(cmd, sizeof(cmd),
                 "xz -%u%s -c --threads=1 \"%s\" > \"%s\" 2>%s",
                 (unsigned)ladder[i].level,
                 ladder[i].extreme ? "e" : "",
                 raw_path, rt_xz, ZXLE_DEVNULL);
        if (try_run(cmd) != 0) { unlink(rt_xz); continue; }
        size_t rt_n = 0;
        uint8_t *rt = read_whole_file(rt_xz, &rt_n);
        int ok = (rt && rt_n == n && memcmp(rt, p, n) == 0);
        free(rt);
        unlink(rt_xz);
        if (ok) { matched = (int)i; break; }
    }
    unlink(in_xz);
    if (matched < 0) { free(raw); unlink(raw_path); return -1; }
    uint8_t level   = ladder[matched].level;
    uint8_t flags   = ladder[matched].extreme ? 0x01 : 0x00;

    int inner_kind = 0;
    Buf tar_recipe; buf_init(&tar_recipe);
    Buf tar_solid;  buf_init(&tar_solid);
    if (raw_n >= 1024 && raw_n % 512 == 0 &&
        memcmp(raw + 257, "ustar", 5) == 0) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.xztar", tmp_prefix);
        if (pack_tar(raw, raw_n, tp, &tar_recipe, &tar_solid) == 0) {
            inner_kind = 1;
        } else {
            buf_free(&tar_recipe); buf_init(&tar_recipe);
            buf_free(&tar_solid);  buf_init(&tar_solid);
        }
    }

    buf_u8(recipe, level);
    buf_u8(recipe, flags);
    buf_u32(recipe, (uint32_t)raw_n);
    buf_u32(recipe, (uint32_t)n);
    buf_u8(recipe, (uint8_t)inner_kind);
    if (inner_kind == 1) {
        buf_u32(recipe, (uint32_t)tar_recipe.n);
        buf_append(recipe, tar_recipe.p, tar_recipe.n);
        buf_append(solid, tar_solid.p, tar_solid.n);
    } else {
        buf_append(solid, raw, raw_n);
    }

    fprintf(stderr, "    xz: orig=%zu raw=%zu level=%u%s%s\n",
            n, raw_n, level,
            (flags & 0x01) ? "e" : "",
            inner_kind == 1 ? " inner=tar" : "");

    free(raw);
    unlink(raw_path);
    buf_free(&tar_recipe);
    buf_free(&tar_solid);
    return 0;
}

void unpack_xz(const uint8_t *recipe, size_t rlen,
               const uint8_t *solid, size_t solid_len, size_t *solid_pos,
               FILE *out, uint64_t expected_size, const char *tmp_prefix) {
    size_t r = 0;
    if (r + 1 + 1 + 4 + 4 + 1 > rlen) die("xz recipe truncated");
    uint8_t level      = recipe[r]; r += 1;
    uint8_t flags      = recipe[r]; r += 1;
    uint32_t raw_len   = r32(recipe + r); r += 4;
    uint32_t orig_len  = r32(recipe + r); r += 4;
    uint8_t inner_kind = recipe[r]; r += 1;
    const uint8_t *tar_recipe = NULL; uint32_t tar_recipe_len = 0;
    if (inner_kind == 1) {
        if (r + 4 > rlen) die("xz tar recipe len truncated");
        tar_recipe_len = r32(recipe + r); r += 4;
        if (r + tar_recipe_len > rlen) die("xz tar recipe overflow");
        tar_recipe = recipe + r; r += tar_recipe_len;
    }
    if (r != rlen) die("xz recipe trailing bytes");
    if (level > 9) die("xz level out of range");

    char raw_path[2048], rt_xz[2048], cmd[4096];
    snprintf(raw_path, sizeof(raw_path), "%s.xz.raw.tmp", tmp_prefix);
    snprintf(rt_xz,    sizeof(rt_xz),    "%s.xz.rt.tmp",  tmp_prefix);

    FILE *rf = fopen(raw_path, "wb");
    if (!rf) die("fopen xz raw tmp");
    if (inner_kind == 0) {
        if (*solid_pos + raw_len > solid_len) die("xz solid overflow");
        if (raw_len > 0 && fwrite(solid + *solid_pos, 1, raw_len, rf) != raw_len)
            die("fwrite xz raw");
        *solid_pos += raw_len;
        fclose(rf);
    } else {
        unpack_recipe(tar_recipe, tar_recipe_len,
                      solid, solid_len, solid_pos,
                      rf, raw_len, raw_path);
        fclose(rf);
    }

    snprintf(cmd, sizeof(cmd),
             "xz -%u%s -c --threads=1 \"%s\" > \"%s\" 2>%s",
             (unsigned)level, (flags & 0x01) ? "e" : "",
             raw_path, rt_xz, ZXLE_DEVNULL);
    run(cmd);
    unlink(raw_path);

    size_t got_n = 0;
    uint8_t *got = read_whole_file(rt_xz, &got_n);
    unlink(rt_xz);
    if (!got || got_n != orig_len) die("xz size mismatch");
    if (got_n != expected_size) die("xz expected size mismatch");
    if (got_n > 0 && fwrite(got, 1, got_n, out) != got_n) die("fwrite xz out");
    free(got);
}
