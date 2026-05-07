#include "zst.h"
#include "tar.h"
#include "recipe.h"

int pack_zst(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid) {
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

    char in_zst[1024], raw_path[1024], rt_zst[1024], cmd[4096];
    snprintf(in_zst,   sizeof(in_zst),   "%s.in.zst",  tmp_prefix);
    snprintf(raw_path, sizeof(raw_path), "%s.raw.bin", tmp_prefix);
    snprintf(rt_zst,   sizeof(rt_zst),   "%s.rt.zst",  tmp_prefix);

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

    int matched_level = -1;
    uint8_t matched_long = 0;
    /* Cap probes at 8 to bound encode time on inputs the CLI can't reproduce
     * (e.g., dpkg-deb's libzstd-direct uses non-CLI-reachable encoder params).
     * With the {3, 19, 22, ...} reorder above, every current matching fixture
     * lands by probe 5 (mixed.tar.zst3 at probe 3, mixed.tar.zst at 3-4,
     * which.pkg.tar.zst at 5). On no-match inputs this turns ~30 probes
     * (~50 s on a 1.4 MB stream) into 8 probes (~12 s). */
    int probes = 0;
    for (size_t li = 0; li < sizeof(levels); li++) {
        if (probes >= 8) break;
        uint8_t level = levels[li];
        for (int lj = 0; lj < n_long; lj++) {
            if (probes >= 8) break;
            probes++;
            uint8_t lw = long_tries[lj];
            char long_part[32];
            if (lw == 0) long_part[0] = 0;
            else snprintf(long_part, sizeof(long_part), " --long=%u", lw);

            if (has_fcs) {
                snprintf(cmd, sizeof(cmd),
                         "zstd -%u%s %s -q -f -o \"%s\" \"%s\" >%s 2>&1",
                         level, long_part, check_arg, rt_zst, raw_path,
                         ZXLE_DEVNULL);
            } else {
                snprintf(cmd, sizeof(cmd),
                         "zstd -%u%s %s -q < \"%s\" > \"%s\" 2>%s",
                         level, long_part, check_arg, raw_path, rt_zst,
                         ZXLE_DEVNULL);
            }
            if (try_run(cmd) != 0) { unlink(rt_zst); continue; }
            size_t rt_n = 0;
            uint8_t *rt = read_whole_file(rt_zst, &rt_n);
            int ok = (rt && rt_n == n && memcmp(rt, p, n) == 0);
            free(rt);
            unlink(rt_zst);
            if (ok) { matched_level = level; matched_long = lw; break; }
        }
        if (matched_level >= 0) break;
    }
    unlink(in_zst);
    if (matched_level < 0) { free(raw); unlink(raw_path); return -1; }
    uint8_t level  = (uint8_t)matched_level;
    uint8_t window = matched_long;
    uint8_t flags  = 0;
    if (!has_fcs)      flags |= 0x01;
    if (!has_checksum) flags |= 0x02;

    int inner_kind = 0;
    Buf tar_recipe; buf_init(&tar_recipe);
    Buf tar_solid;  buf_init(&tar_solid);
    if (raw_n >= 1024 && raw_n % 512 == 0 &&
        memcmp(raw + 257, "ustar", 5) == 0) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.zsttar", tmp_prefix);
        if (pack_tar(raw, raw_n, tp, &tar_recipe, &tar_solid) == 0) {
            inner_kind = 1;
        } else {
            buf_free(&tar_recipe); buf_init(&tar_recipe);
            buf_free(&tar_solid);  buf_init(&tar_solid);
        }
    }

    buf_u8(recipe, level);
    buf_u8(recipe, window);
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

    fprintf(stderr, "    zst: orig=%zu raw=%zu level=%u long=%u io=%s check=%s%s\n",
            n, raw_n, level, window,
            (flags & 0x01) ? "stdin" : "file",
            (flags & 0x02) ? "off" : "on",
            inner_kind == 1 ? " inner=tar" : "");

    free(raw);
    unlink(raw_path);
    buf_free(&tar_recipe);
    buf_free(&tar_solid);
    return 0;
}

void unpack_zst(const uint8_t *recipe, size_t rlen,
                const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                FILE *out, uint64_t expected_size, const char *tmp_prefix) {
    size_t r = 0;
    if (r + 1 + 1 + 1 + 4 + 4 + 1 > rlen) die("zst recipe truncated");
    uint8_t level      = recipe[r]; r += 1;
    uint8_t window     = recipe[r]; r += 1;
    uint8_t flags      = recipe[r]; r += 1;
    uint32_t raw_len   = r32(recipe + r); r += 4;
    uint32_t orig_len  = r32(recipe + r); r += 4;
    uint8_t inner_kind = recipe[r]; r += 1;
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
        if (*solid_pos + raw_len > solid_len) die("zst solid overflow");
        if (raw_len > 0 && fwrite(solid + *solid_pos, 1, raw_len, rf) != raw_len)
            die("fwrite zst raw");
        *solid_pos += raw_len;
        fclose(rf);
    } else {
        unpack_recipe(tar_recipe, tar_recipe_len,
                      solid, solid_len, solid_pos,
                      rf, raw_len, raw_path);
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
