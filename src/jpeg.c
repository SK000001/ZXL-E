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
