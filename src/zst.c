#include "zst.h"
#include "kinds.h"
#include "tar.h"
#include "recipe.h"

int pack_zst(const uint8_t *p, size_t n, const char *tmp_prefix,
             Buf *recipe, Buf *b0, Buf *b1, uint8_t bucket) {
    if (n < 8) return -1;
    if (p[0] != 0x28 || p[1] != 0xB5 || p[2] != 0x2F || p[3] != 0xFD) return -1;
    if (n > 0xFFFFFFFFu) return -1;

    /* Parse Frame_Header_Descriptor + Window_Descriptor (RFC 8478 §3.1.1.1). */
    uint8_t fhd            = p[4];
    int fcs_flag           = (fhd >> 6) & 0x3;
    int single_segment     = (fhd >> 5) & 0x1;
    int has_checksum       = (fhd >> 2) & 0x1;
    int dict_id_flag       = fhd & 0x3;
    if (dict_id_flag != 0) return -1;
    int window_log = 0;
    if (!single_segment) {
        if (n < 6) return -1;
        window_log = 10 + ((p[5] >> 3) & 0x1F);
        if (window_log < 10 || window_log > 31) return -1;
    }
    int has_fcs = (fcs_flag != 0) || single_segment;

    /* Multi-frame fast-fail: zstd -T0 worker output emits one frame per
     * worker, each prefixed with the 28 B5 2F FD magic. We only reproduce
     * single-frame inputs, so any second magic occurrence means bail before
     * the probe ladder. False-positive prob (magic appearing by chance in
     * a compressed body) is ~n/2^32; a false positive just falls through
     * to KIND_OPAQUE which is the correct behavior anyway. */
    for (size_t i = 4; i + 3 < n; i++) {
        if (p[i]==0x28 && p[i+1]==0xB5 && p[i+2]==0x2F && p[i+3]==0xFD)
            return -1;
    }

    char in_zst[1024], raw_path[1024], cmd[4096];
    snprintf(in_zst,   sizeof(in_zst),   "%s.in.zst",  tmp_prefix);
    snprintf(raw_path, sizeof(raw_path), "%s.raw.bin", tmp_prefix);

    FILE *zf = fopen(in_zst, "wb");
    if (!zf) return -1;
    if (n > 0 && fwrite(p, 1, n, zf) != n) { fclose(zf); unlink(in_zst); return -1; }
    fclose(zf);

    snprintf(cmd, sizeof(cmd), "zstd -d -q -f -o \"%s\" \"%s\" >%s 2>&1",
             raw_path, in_zst, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(in_zst); unlink(raw_path); return -1; }

    size_t raw_n = 0;
    uint8_t *raw = read_whole_file(raw_path, &raw_n);
    if (!raw || raw_n > 0xFFFFFFFFu) {
        free(raw); unlink(in_zst); unlink(raw_path); return -1;
    }

    /* Levels ordered by real-world frequency: -3 is the zstd CLI default and
     * the typical encoder for makepkg/dpkg-style packagers; -19 is the common
     * high-effort default; -22 next; remainder rounds out coverage. The
     * 8-probe cap below assumes the matching encoder is in the first ~3
     * levels for any current fixture. */
    static const uint8_t levels[] = {3, 19, 22, 20, 18, 17, 9, 6, 1, 21};
    uint8_t long_tries[3];
    int n_long = 0;
    long_tries[n_long++] = 27;
    if (window_log != 0 && window_log != 27) long_tries[n_long++] = (uint8_t)window_log;
    long_tries[n_long++] = 0;

    const char *check_arg = has_checksum ? "--check" : "--no-check";

    /* M7 step 1: enumerate up to 8 (level, long) candidates, then run them
     * concurrently and pick the lowest-index match. Same 8-probe cap and
     * priority ordering as the prior serial loop. */
    const int PROBE_CAP = 8;
    uint8_t probe_level[8];
    uint8_t probe_long[8];
    int n_probes = 0;
    for (size_t li = 0; li < sizeof(levels) && n_probes < PROBE_CAP; li++) {
        for (int lj = 0; lj < n_long && n_probes < PROBE_CAP; lj++) {
            probe_level[n_probes] = levels[li];
            probe_long[n_probes]  = long_tries[lj];
            n_probes++;
        }
    }
    char (*rt_paths)[1024] = malloc(sizeof(char[1024]) * (size_t)n_probes);
    char (*cmd_bufs)[4096] = malloc(sizeof(char[4096]) * (size_t)n_probes);
    const char **cmd_ptrs  = malloc(sizeof(char *) * (size_t)n_probes);
    int *rcs               = malloc(sizeof(int) * (size_t)n_probes);
    if (!rt_paths || !cmd_bufs || !cmd_ptrs || !rcs) {
        free(rt_paths); free(cmd_bufs); free(cmd_ptrs); free(rcs);
        free(raw); unlink(in_zst); unlink(raw_path); return -1;
    }
    for (int i = 0; i < n_probes; i++) {
        snprintf(rt_paths[i], 1024, "%s.rt.%d.zst", tmp_prefix, i);
        char long_part[32];
        if (probe_long[i] == 0) long_part[0] = 0;
        else snprintf(long_part, sizeof(long_part), " --long=%u", probe_long[i]);
        if (has_fcs) {
            snprintf(cmd_bufs[i], 4096,
                     "zstd -%u%s %s -q -f -o \"%s\" \"%s\" >%s 2>&1",
                     probe_level[i], long_part, check_arg,
                     rt_paths[i], raw_path, ZXLE_DEVNULL);
        } else {
            snprintf(cmd_bufs[i], 4096,
                     "zstd -%u%s %s -q < \"%s\" > \"%s\" 2>%s",
                     probe_level[i], long_part, check_arg,
                     raw_path, rt_paths[i], ZXLE_DEVNULL);
        }
        cmd_ptrs[i] = cmd_bufs[i];
    }
    try_run_parallel(cmd_ptrs, n_probes, rcs);
    int matched_level = -1;
    uint8_t matched_long = 0;
    for (int i = 0; i < n_probes; i++) {
        if (rcs[i] != 0) { unlink(rt_paths[i]); continue; }
        if (matched_level >= 0) { unlink(rt_paths[i]); continue; }
        size_t rt_n = 0;
        uint8_t *rt = read_whole_file(rt_paths[i], &rt_n);
        int ok = (rt && rt_n == n && memcmp(rt, p, n) == 0);
        free(rt);
        unlink(rt_paths[i]);
        if (ok) { matched_level = probe_level[i]; matched_long = probe_long[i]; }
    }
    free(rt_paths); free(cmd_bufs); free(cmd_ptrs); free(rcs);
    unlink(in_zst);
    if (matched_level < 0) { free(raw); unlink(raw_path); return -1; }
    uint8_t level  = (uint8_t)matched_level;
    uint8_t window = matched_long;
    uint8_t flags  = 0;
    if (!has_fcs)      flags |= 0x01;
    if (!has_checksum) flags |= 0x02;

    bucket = bucket_for_bytes(raw, raw_n);
    Buf *body_buf = (bucket == 1) ? b1 : b0;

    int inner_kind = 0;
    Buf tar_recipe; buf_init(&tar_recipe);
    Buf tar_b0; buf_init(&tar_b0);
    Buf tar_b1; buf_init(&tar_b1);
    if (raw_n >= 1024 && raw_n % 512 == 0 &&
        memcmp(raw + 257, "ustar", 5) == 0) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.zsttar", tmp_prefix);
        if (pack_tar(raw, raw_n, tp, &tar_recipe, &tar_b0, &tar_b1) == 0) {
            inner_kind = 1;
        } else {
            buf_free(&tar_recipe); buf_init(&tar_recipe);
            buf_free(&tar_b0); buf_init(&tar_b0);
            buf_free(&tar_b1); buf_init(&tar_b1);
        }
    }

    buf_u8(recipe, level);
    buf_u8(recipe, window);
    buf_u8(recipe, flags);
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

    fprintf(stderr, "    zst: orig=%zu raw=%zu level=%u long=%u io=%s check=%s%s b=%u\n",
            n, raw_n, level, window,
            (flags & 0x01) ? "stdin" : "file",
            (flags & 0x02) ? "off" : "on",
            inner_kind == 1 ? " inner=tar" : "",
            (unsigned)bucket);

    free(raw);
    unlink(raw_path);
    buf_free(&tar_recipe);
    buf_free(&tar_b0);
    buf_free(&tar_b1);
    return 0;
}

void unpack_zst(const uint8_t *recipe, size_t rlen,
                Solids *s,
                FILE *out, uint64_t expected_size, const char *tmp_prefix) {
    size_t r = 0;
    if (r + 1 + 1 + 1 + 4 + 4 + 1 + 1 > rlen) die("zst recipe truncated");
    uint8_t level      = recipe[r]; r += 1;
    uint8_t window     = recipe[r]; r += 1;
    uint8_t flags      = recipe[r]; r += 1;
    uint32_t raw_len   = r32(recipe + r); r += 4;
    uint32_t orig_len  = r32(recipe + r); r += 4;
    uint8_t inner_kind = recipe[r]; r += 1;
    uint8_t bucket     = recipe[r]; r += 1;
    if (bucket >= ZXLE_NUM_BUCKETS) die("zst bucket oob");
    if (level < 1 || level > 22) die("zst level out of range");
    const uint8_t *tar_recipe = NULL; uint32_t tar_recipe_len = 0;
    if (inner_kind == 1) {
        if (r + 4 > rlen) die("zst tar recipe len truncated");
        tar_recipe_len = r32(recipe + r); r += 4;
        if (r + tar_recipe_len > rlen) die("zst tar recipe overflow");
        tar_recipe = recipe + r; r += tar_recipe_len;
    }
    if (r != rlen) die("zst recipe trailing bytes");

    char raw_path[2048], rt_zst[2048], cmd[4096];
    snprintf(raw_path, sizeof(raw_path), "%s.zst.raw.tmp", tmp_prefix);
    snprintf(rt_zst,   sizeof(rt_zst),   "%s.zst.rt.tmp",  tmp_prefix);

    FILE *rf = fopen(raw_path, "wb");
    if (!rf) die("fopen zst raw tmp");
    if (inner_kind == 0) {
        if (s->pos[bucket] + raw_len > s->len[bucket]) die("zst solid overflow");
        if (raw_len > 0 && fwrite(s->p[bucket] + s->pos[bucket], 1, raw_len, rf) != raw_len)
            die("fwrite zst raw");
        s->pos[bucket] += raw_len;
        fclose(rf);
    } else {
        unpack_recipe(tar_recipe, tar_recipe_len, s, rf, raw_len, raw_path);
        fclose(rf);
    }

    char long_part[32];
    if (window == 0) long_part[0] = 0;
    else snprintf(long_part, sizeof(long_part), " --long=%u", window);
    const char *check_arg = (flags & 0x02) ? "--no-check" : "--check";
    if (flags & 0x01) {
        snprintf(cmd, sizeof(cmd),
                 "zstd -%u%s %s -q < \"%s\" > \"%s\" 2>%s",
                 level, long_part, check_arg, raw_path, rt_zst, ZXLE_DEVNULL);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "zstd -%u%s %s -q -f -o \"%s\" \"%s\" >%s 2>&1",
                 level, long_part, check_arg, rt_zst, raw_path, ZXLE_DEVNULL);
    }
    run(cmd);
    unlink(raw_path);

    size_t got_n = 0;
    uint8_t *got = read_whole_file(rt_zst, &got_n);
    unlink(rt_zst);
    if (!got || got_n != orig_len) die("zst size mismatch");
    if (got_n != expected_size) die("zst expected size mismatch");
    if (got_n > 0 && fwrite(got, 1, got_n, out) != got_n) die("fwrite zst out");
    free(got);
}
