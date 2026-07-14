#include "jpeg.h"

int try_brunsli_buf(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *out) {
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8 || p[2] != 0xFF) return -1;

    char tmp_in[1024], tmp_brn[1024], tmp_jpg[1024], cmd[4096];
    snprintf(tmp_in,  sizeof(tmp_in),  "%s.in.jpg.tmp", tmp_prefix);
    snprintf(tmp_brn, sizeof(tmp_brn), "%s.brn.tmp",    tmp_prefix);
    snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.brnrt.tmp",  tmp_prefix);

    FILE *jf = fopen(tmp_in, "wb");
    if (!jf) return -1;
    if (n > 0 && fwrite(p, 1, n, jf) != n) { fclose(jf); unlink(tmp_in); return -1; }
    fclose(jf);

    snprintf(cmd, sizeof(cmd), "cbrunsli \"%s\" \"%s\" >%s 2>&1", tmp_in, tmp_brn, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(tmp_in); return -1; }
    unlink(tmp_in);

    snprintf(cmd, sizeof(cmd), "dbrunsli \"%s\" \"%s\" >%s 2>&1", tmp_brn, tmp_jpg, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(tmp_brn); return -1; }
    size_t rt_n = 0;
    uint8_t *rt = read_whole_file(tmp_jpg, &rt_n);
    int ok = (rt_n == n && memcmp(rt, p, n) == 0);
    free(rt);
    unlink(tmp_jpg);
    if (!ok) { unlink(tmp_brn); return -1; }

    size_t brn_n = 0;
    uint8_t *brn = read_whole_file(tmp_brn, &brn_n);
    unlink(tmp_brn);
    if (brn_n >= n) { free(brn); return -1; }
    buf_append(out, brn, brn_n);
    free(brn);
    return 0;
}

int try_packjpg_buf(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *out) {
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8 || p[2] != 0xFF) return -1;

    /* packJPG derives its output name by swapping the extension, so the
     * temp names must end in .jpg/.pjg. */
    char tmp_in[1024], tmp_pjg[1024], cmd[4096];
    snprintf(tmp_in,  sizeof(tmp_in),  "%s.pj.jpg", tmp_prefix);
    snprintf(tmp_pjg, sizeof(tmp_pjg), "%s.pj.pjg", tmp_prefix);

    FILE *jf = fopen(tmp_in, "wb");
    if (!jf) return -1;
    if (n > 0 && fwrite(p, 1, n, jf) != n) { fclose(jf); unlink(tmp_in); return -1; }
    fclose(jf);

    snprintf(cmd, sizeof(cmd), "packJPG -o -np \"%s\" >%s 2>&1", tmp_in, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(tmp_in); unlink(tmp_pjg); return -1; }
    unlink(tmp_in);

    char tmp_rt_pjg[1024], tmp_rt_jpg[1024];
    snprintf(tmp_rt_pjg, sizeof(tmp_rt_pjg), "%s.pjrt.pjg", tmp_prefix);
    snprintf(tmp_rt_jpg, sizeof(tmp_rt_jpg), "%s.pjrt.jpg", tmp_prefix);
    if (rename(tmp_pjg, tmp_rt_pjg) != 0) { unlink(tmp_pjg); return -1; }
    snprintf(cmd, sizeof(cmd), "packJPG -o -np \"%s\" >%s 2>&1", tmp_rt_pjg, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(tmp_rt_pjg); unlink(tmp_rt_jpg); return -1; }
    size_t rt_n = 0;
    uint8_t *rt = read_whole_file(tmp_rt_jpg, &rt_n);
    int ok = (rt && rt_n == n && memcmp(rt, p, n) == 0);
    free(rt);
    unlink(tmp_rt_jpg);
    if (!ok) { unlink(tmp_rt_pjg); return -1; }

    size_t pjg_n = 0;
    uint8_t *pjg = read_whole_file(tmp_rt_pjg, &pjg_n);
    unlink(tmp_rt_pjg);
    if (!pjg || pjg_n >= n) { free(pjg); return -1; }
    buf_append(out, pjg, pjg_n);
    free(pjg);
    return 0;
}

int try_jpeg_buf(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *out) {
    Buf brn; buf_init(&brn);
    Buf pjg; buf_init(&pjg);
    int has_brn = try_brunsli_buf(p, n, tmp_prefix, &brn) == 0;
    int has_pjg = try_packjpg_buf(p, n, tmp_prefix, &pjg) == 0;
    if (!has_brn && !has_pjg) { buf_free(&brn); buf_free(&pjg); return -1; }
    if (has_pjg && (!has_brn || pjg.n < brn.n)) {
        buf_u8(out, 1);
        buf_append(out, pjg.p, pjg.n);
    } else {
        buf_u8(out, 0);
        buf_append(out, brn.p, brn.n);
    }
    buf_free(&brn);
    buf_free(&pjg);
    return 0;
}

uint8_t *unpack_jpeg_blob(const uint8_t *blob, uint32_t blob_len,
                          const char *tmp_prefix, size_t *out_n) {
    if (blob_len < 1) die("jpeg blob empty");
    uint8_t codec = blob[0];
    const uint8_t *payload = blob + 1;
    uint32_t payload_len = blob_len - 1;
    char tmp_c[1024], tmp_jpg[1024], cmd[4096];

    if (codec == 0) {
        snprintf(tmp_c,   sizeof(tmp_c),   "%s.uj.brn.tmp", tmp_prefix);
        snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.uj.jpg.tmp", tmp_prefix);
        FILE *bf = fopen(tmp_c, "wb");
        if (!bf) die("fopen tmp brn");
        if (payload_len > 0 && fwrite(payload, 1, payload_len, bf) != payload_len)
            die("fwrite tmp brn");
        fclose(bf);
        snprintf(cmd, sizeof(cmd), "dbrunsli \"%s\" \"%s\" >%s 2>&1",
                 tmp_c, tmp_jpg, ZXLE_DEVNULL);
        run(cmd);
        unlink(tmp_c);
    } else if (codec == 1) {
        /* packJPG swaps the extension: x.pjg -> x.jpg. */
        snprintf(tmp_c,   sizeof(tmp_c),   "%s.uj.pjg", tmp_prefix);
        snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.uj.jpg", tmp_prefix);
        FILE *bf = fopen(tmp_c, "wb");
        if (!bf) die("fopen tmp pjg");
        if (payload_len > 0 && fwrite(payload, 1, payload_len, bf) != payload_len)
            die("fwrite tmp pjg");
        fclose(bf);
        snprintf(cmd, sizeof(cmd), "packJPG -o -np \"%s\" >%s 2>&1",
                 tmp_c, ZXLE_DEVNULL);
        run(cmd);
        unlink(tmp_c);
    } else {
        die("jpeg blob unknown codec");
    }

    uint8_t *jpg = read_whole_file(tmp_jpg, out_n);
    unlink(tmp_jpg);
    return jpg;
}
