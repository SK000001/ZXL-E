#include "xz.h"
#include "kinds.h"
#include "tar.h"
#include "recipe.h"

/* Parse xz block header to extract the LZMA2 dictionary-size byte (the single
 * filter property byte for filter ID 0x21). Returns 0 on success and stores
 * the byte in *dict_byte; -1 on parse failure or unexpected layout. We only
 * handle the common shape: stream header (12 B) + one block with one LZMA2
 * filter. */
static int xz_parse_lzma2_dict(const uint8_t *p, size_t n, uint8_t *dict_byte) {
    size_t off = 12;
    if (off + 1 > n) return -1;
    uint8_t hsize_b = p[off];
    if (hsize_b == 0) return -1; /* 0 means index, not a block */
    size_t hdr_size = ((size_t)hsize_b + 1) * 4;
    if (off + hdr_size + 4 > n) return -1; /* +4 for block-header CRC32 */
    size_t end = off + hdr_size; /* end of header, before its CRC32 */
    uint8_t bflags = p[off + 1];
    int n_filters  = (bflags & 0x03) + 1;
    if (n_filters != 1) return -1;
    int has_csize = (bflags >> 6) & 1;
    int has_usize = (bflags >> 7) & 1;
    size_t cur = off + 2;
    /* skip variable-length integers (xz VLI: 7 bits per byte, top bit = more) */
    int vli_n;
    if (has_csize) {
        for (vli_n = 0; vli_n < 9; vli_n++) {
            if (cur >= end) return -1;
            if ((p[cur++] & 0x80) == 0) break;
        }
        if (vli_n >= 9) return -1;
    }
    if (has_usize) {
        for (vli_n = 0; vli_n < 9; vli_n++) {
            if (cur >= end) return -1;
            if ((p[cur++] & 0x80) == 0) break;
        }
        if (vli_n >= 9) return -1;
    }
    /* Filter: filter_id (vli) + prop_size (vli) + props. LZMA2 has filter_id
     * 0x21 (1-byte vli) and prop_size 0x01 (1-byte vli); reject anything else. */
    if (cur + 3 > end) return -1;
    if (p[cur++] != 0x21) return -1;
    if (p[cur++] != 0x01) return -1;
    *dict_byte = p[cur];
    return 0;
}

/* Map LZMA2 dict byte to xz preset-level candidates (--extreme / non-extreme
 * share dict, so we'll probe both). Returns count; 0 means custom dict that
 * no preset can reproduce (caller should bail to KIND_OPAQUE). */
static int xz_dict_byte_to_levels(uint8_t b, uint8_t out[2]) {
    switch (b) {
    case 0x0C: out[0] = 0; return 1;                      /* 256 KiB */
    case 0x10: out[0] = 1; return 1;                      /*   1 MiB */
    case 0x12: out[0] = 2; return 1;                      /*   2 MiB */
    case 0x14: out[0] = 3; out[1] = 4; return 2;          /*   4 MiB */
    case 0x16: out[0] = 5; out[1] = 6; return 2;          /*   8 MiB */
    case 0x18: out[0] = 7; return 1;                      /*  16 MiB */
    case 0x1A: out[0] = 8; return 1;                      /*  32 MiB */
    case 0x1C: out[0] = 9; return 1;                      /*  64 MiB */
    default:   return 0;                                  /* custom */
    }
}

int pack_xz(const uint8_t *p, size_t n, const char *tmp_prefix,
            Buf *recipe, Buf *b0, Buf *b1, uint8_t bucket) {
    static const uint8_t XZ_MAGIC[6] = { 0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00 };
    if (n < 12) return -1;
    if (memcmp(p, XZ_MAGIC, 6) != 0) return -1;
    if (n > 0xFFFFFFFFu) return -1;

    /* Multi-stream fast-fail: xz -T0 emits one stream per worker, each with
     * the 6-byte magic. We only reproduce single-stream inputs; bail on any
     * second magic. False-positive prob ~n/2^48 (cost: KIND_OPAQUE). */
    for (size_t i = 6; i + 5 < n; i++) {
        if (p[i]==0xFD && p[i+1]==0x37 && p[i+2]==0x7A &&
            p[i+3]==0x58 && p[i+4]==0x5A && p[i+5]==0x00)
            return -1;
    }

    /* Dict-driven probe pruning: parse the LZMA2 dict byte from the block
     * header and probe only preset levels whose dict matches. If parsing
     * fails (multi-block, multi-filter, unexpected shape), fall back to the
     * full 8-probe ladder for compatibility. */
    uint8_t dict_byte = 0xFF;
    int dict_ok = (xz_parse_lzma2_dict(p, n, &dict_byte) == 0);
    uint8_t dict_levels[2];
    int n_dict_levels = dict_ok ? xz_dict_byte_to_levels(dict_byte, dict_levels) : 0;
    if (dict_ok && n_dict_levels == 0) return -1; /* custom dict, unreachable */

    char in_xz[1024], raw_path[1024], cmd[4096];
    snprintf(in_xz,    sizeof(in_xz),    "%s.in.xz",   tmp_prefix);
    snprintf(raw_path, sizeof(raw_path), "%s.raw.bin", tmp_prefix);

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

    struct LP { uint8_t level; uint8_t extreme; };
    struct LP ladder[8];
    int n_ladder;
    if (dict_ok) {
        n_ladder = 0;
        for (int li = 0; li < n_dict_levels; li++) {
            ladder[n_ladder++] = (struct LP){dict_levels[li], 1};
            ladder[n_ladder++] = (struct LP){dict_levels[li], 0};
        }
    } else {
        static const struct LP fallback[] = {
            {9, 1}, {9, 0}, {6, 1}, {6, 0},
            {3, 1}, {3, 0}, {1, 0}, {0, 0},
        };
        memcpy(ladder, fallback, sizeof(fallback));
        n_ladder = (int)(sizeof(fallback)/sizeof(fallback[0]));
    }
    /* M7 step 1: run all probe candidates concurrently (each into a unique
     * rt path); after join, pick the lowest-ladder-index match. Worst case
     * (no match) drops from sum-of-probe-times to max-of-probe-times; best
     * case (match at probe 0) does extra work but doesn't extend wall time. */
    char (*rt_paths)[1024] = malloc(sizeof(char[1024]) * (size_t)n_ladder);
    char (*cmd_bufs)[4096] = malloc(sizeof(char[4096]) * (size_t)n_ladder);
    const char **cmd_ptrs  = malloc(sizeof(char *) * (size_t)n_ladder);
    int *rcs               = malloc(sizeof(int) * (size_t)n_ladder);
    if (!rt_paths || !cmd_bufs || !cmd_ptrs || !rcs) {
        free(rt_paths); free(cmd_bufs); free(cmd_ptrs); free(rcs);
        free(raw); unlink(in_xz); unlink(raw_path); return -1;
    }
    for (int i = 0; i < n_ladder; i++) {
        snprintf(rt_paths[i], 1024, "%s.rt.%d.xz", tmp_prefix, i);
        snprintf(cmd_bufs[i], 4096,
                 "xz -%u%s -c --threads=1 \"%s\" > \"%s\" 2>%s",
                 (unsigned)ladder[i].level,
                 ladder[i].extreme ? "e" : "",
                 raw_path, rt_paths[i], ZXLE_DEVNULL);
        cmd_ptrs[i] = cmd_bufs[i];
    }
    try_run_parallel(cmd_ptrs, n_ladder, rcs);
    int matched = -1;
    for (int i = 0; i < n_ladder; i++) {
        if (rcs[i] != 0) { unlink(rt_paths[i]); continue; }
        if (matched >= 0) { unlink(rt_paths[i]); continue; }
        size_t rt_n = 0;
        uint8_t *rt = read_whole_file(rt_paths[i], &rt_n);
        int ok = (rt && rt_n == n && memcmp(rt, p, n) == 0);
        free(rt);
        unlink(rt_paths[i]);
        if (ok) matched = i;
    }
    free(rt_paths); free(cmd_bufs); free(cmd_ptrs); free(rcs);
    unlink(in_xz);
    if (matched < 0) { free(raw); unlink(raw_path); return -1; }
    uint8_t level   = ladder[matched].level;
    uint8_t flags   = ladder[matched].extreme ? 0x01 : 0x00;

    bucket = bucket_for_bytes(raw, raw_n);
    Buf *body_buf = (bucket == 1) ? b1 : b0;

    int inner_kind = 0;
    Buf tar_recipe; buf_init(&tar_recipe);
    Buf tar_b0; buf_init(&tar_b0);
    Buf tar_b1; buf_init(&tar_b1);
    if (raw_n >= 1024 && raw_n % 512 == 0 &&
        memcmp(raw + 257, "ustar", 5) == 0) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.xztar", tmp_prefix);
        if (pack_tar(raw, raw_n, tp, &tar_recipe, &tar_b0, &tar_b1) == 0) {
            inner_kind = 1;
        } else {
            buf_free(&tar_recipe); buf_init(&tar_recipe);
            buf_free(&tar_b0); buf_init(&tar_b0);
            buf_free(&tar_b1); buf_init(&tar_b1);
        }
    }

    buf_u8(recipe, level);
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

    fprintf(stderr, "    xz: orig=%zu raw=%zu level=%u%s%s b=%u\n",
            n, raw_n, level,
            (flags & 0x01) ? "e" : "",
            inner_kind == 1 ? " inner=tar" : "",
            (unsigned)bucket);

    free(raw);
    unlink(raw_path);
    buf_free(&tar_recipe);
    buf_free(&tar_b0);
    buf_free(&tar_b1);
    return 0;
}

void unpack_xz(const uint8_t *recipe, size_t rlen,
               Solids *s,
               FILE *out, uint64_t expected_size, const char *tmp_prefix) {
    size_t r = 0;
    if (r + 1 + 1 + 4 + 4 + 1 + 1 > rlen) die("xz recipe truncated");
    uint8_t level      = recipe[r]; r += 1;
    uint8_t flags      = recipe[r]; r += 1;
    uint32_t raw_len   = r32(recipe + r); r += 4;
    uint32_t orig_len  = r32(recipe + r); r += 4;
    uint8_t inner_kind = recipe[r]; r += 1;
    uint8_t bucket     = recipe[r]; r += 1;
    if (bucket >= ZXLE_NUM_BUCKETS) die("xz bucket oob");
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
        if (s->pos[bucket] + raw_len > s->len[bucket]) die("xz solid overflow");
        if (raw_len > 0 && fwrite(s->p[bucket] + s->pos[bucket], 1, raw_len, rf) != raw_len)
            die("fwrite xz raw");
        s->pos[bucket] += raw_len;
        fclose(rf);
    } else {
        unpack_recipe(tar_recipe, tar_recipe_len, s, rf, raw_len, raw_path);
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
