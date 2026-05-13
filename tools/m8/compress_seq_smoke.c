// Minimal smoke test for ZSTD_compressSequences.
// Hand-crafted parse for "abcabcabcabcabcabc" (18 bytes).

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *src = "abcabcabcabcabcabc";  // 18 bytes
    size_t n = 18;

    // Parse: 3 literals "abc", then match of 15 bytes at offset 3.
    // (Self-overlapping match copies "abc" repeatedly.)
    ZSTD_Sequence seqs[2];
    seqs[0].litLength   = 3;
    seqs[0].matchLength = 15;
    seqs[0].offset      = 3;
    seqs[0].rep         = 0;
    // Final literal-only delimiter.
    seqs[1].litLength   = 0;
    seqs[1].matchLength = 0;
    seqs[1].offset      = 0;
    seqs[1].rep         = 0;

    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 19);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_minMatch, 3);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_validateSequences, 1);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_blockDelimiters, ZSTD_sf_noBlockDelimiters);

    char zbuf[256];
    size_t zsize = ZSTD_compressSequences(cctx, zbuf, sizeof(zbuf), seqs, 2, src, n);
    if (ZSTD_isError(zsize)) {
        fprintf(stderr, "compressSequences fail: %s (code=%u)\n",
                ZSTD_getErrorName(zsize), (unsigned)(size_t)(-(ptrdiff_t)zsize));
        return 1;
    }
    printf("ok: %zu -> %zu\n", n, zsize);

    char back[64];
    size_t got = ZSTD_decompress(back, sizeof(back), zbuf, zsize);
    if (ZSTD_isError(got)) { fprintf(stderr, "decompress: %s\n", ZSTD_getErrorName(got)); return 2; }
    if (got != n || memcmp(back, src, n) != 0) { fprintf(stderr, "mismatch\n"); return 3; }
    printf("roundtrip ok\n");
    ZSTD_freeCCtx(cctx);
    return 0;
}
