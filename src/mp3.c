#include "mp3.h"

int looks_like_mp3(const uint8_t *p, size_t n) {
    if (n < 3) return 0;
    if (p[0]=='I' && p[1]=='D' && p[2]=='3') return 1;
    if (p[0]==0xFF && (p[1]&0xE0)==0xE0) return 1;
    return 0;
}

int try_packmp3_buf(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *out) {
    if (!looks_like_mp3(p, n)) return -1;

    char tmp_in[1024], tmp_pmp[1024], cmd[4096];
    snprintf(tmp_in,  sizeof(tmp_in),  "%s.in.mp3", tmp_prefix);
    snprintf(tmp_pmp, sizeof(tmp_pmp), "%s.in.pmp", tmp_prefix);

    FILE *jf = fopen(tmp_in, "wb");
    if (!jf) return -1;
    if (n > 0 && fwrite(p, 1, n, jf) != n) { fclose(jf); unlink(tmp_in); return -1; }
    fclose(jf);

    snprintf(cmd, sizeof(cmd), "packMP3 -o -np \"%s\" >%s 2>&1", tmp_in, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(tmp_in); unlink(tmp_pmp); return -1; }
    unlink(tmp_in);

    char tmp_rt_pmp[1024], tmp_rt_mp3[1024];
    snprintf(tmp_rt_pmp, sizeof(tmp_rt_pmp), "%s.rt.pmp", tmp_prefix);
    snprintf(tmp_rt_mp3, sizeof(tmp_rt_mp3), "%s.rt.mp3", tmp_prefix);
    if (rename(tmp_pmp, tmp_rt_pmp) != 0) { unlink(tmp_pmp); return -1; }
    snprintf(cmd, sizeof(cmd), "packMP3 -o -np \"%s\" >%s 2>&1", tmp_rt_pmp, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(tmp_rt_pmp); unlink(tmp_rt_mp3); return -1; }
    size_t rt_n = 0;
    uint8_t *rt = read_whole_file(tmp_rt_mp3, &rt_n);
    int ok = (rt && rt_n == n && memcmp(rt, p, n) == 0);
    free(rt);
    unlink(tmp_rt_mp3);
    if (!ok) { unlink(tmp_rt_pmp); return -1; }

    size_t pmp_n = 0;
    uint8_t *pmp = read_whole_file(tmp_rt_pmp, &pmp_n);
    unlink(tmp_rt_pmp);
    if (!pmp || pmp_n >= n) { free(pmp); return -1; }
    buf_append(out, pmp, pmp_n);
    free(pmp);
    return 0;
}
