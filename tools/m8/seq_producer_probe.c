// M8 step 2a -- validate zstd's external sequence-producer API.
//
// Hand-crafts an LZ77 parse for a known input ("abcabcabcabcabcabc"),
// registers it via ZSTD_registerSequenceProducer, compresses, then
// decompresses through the standard zstd decoder and checks byte-for-byte.
//
// If this works, the M8 step-2 architecture (GPU emits match-stream, zstd
// does entropy coding) is viable. If it doesn't, step 2 needs a different
// (much heavier) encoder integration.

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Encode "abcabcabc..." (n_blocks * 3 bytes) as: literal "abc", then a single
// long match of (n_blocks-1)*3 bytes at offset 3.
// Sum: litLength=3, matchLength=(n_blocks-1)*3, total = n_blocks*3 = input size.
typedef struct {
    int emitted;
    size_t input_size;
} producer_state_t;

static size_t my_seq_producer(
    void *state,
    ZSTD_Sequence *outSeqs, size_t outSeqsCapacity,
    const void *src, size_t srcSize,
    const void *dict, size_t dictSize,
    int compressionLevel,
    size_t windowSize)
{
    (void)dict; (void)dictSize; (void)compressionLevel; (void)windowSize;
    producer_state_t *st = (producer_state_t *)state;
    if (st->emitted) return 0;  // already produced
    if (srcSize < 6) return ZSTD_SEQUENCE_PRODUCER_ERROR;
    if (outSeqsCapacity < 2) return ZSTD_SEQUENCE_PRODUCER_ERROR;

    // Sequence 0: 3 literals "abc", then match of (srcSize-3) bytes at offset 3.
    outSeqs[0].litLength   = 3;
    outSeqs[0].matchLength = (unsigned int)(srcSize - 3);
    outSeqs[0].offset      = 3;
    outSeqs[0].rep         = 0;

    // Terminating "last literals" sequence: 0 literals, 0 match.
    // (Per zstd docs: a terminating sequence with matchLength==0 conveys
    // remaining literals; here we have none past the match.)
    outSeqs[1].litLength   = 0;
    outSeqs[1].matchLength = 0;
    outSeqs[1].offset      = 0;
    outSeqs[1].rep         = 0;

    st->emitted = 1;
    return 2;  // number of sequences written
}

int main(void) {
    // Build input: "abc" repeated.
    const int N_BLOCKS = 16;  // 48 bytes total
    char input[64];
    for (int i = 0; i < N_BLOCKS; i++) memcpy(input + i*3, "abc", 3);
    size_t input_size = N_BLOCKS * 3;
    printf("input: %zu bytes (%.*s...)\n", input_size, (int)(input_size < 24 ? input_size : 24), input);

    // Compress.
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    if (!cctx) { fprintf(stderr, "createCCtx fail\n"); return 1; }

    producer_state_t st = {0, input_size};
    ZSTD_registerSequenceProducer(cctx, &st, my_seq_producer);

    // Force the encoder to honor the external sequences (no internal match-finder).
    size_t e = ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableSeqProducerFallback, 0);
    if (ZSTD_isError(e)) { fprintf(stderr, "setParameter fail: %s\n", ZSTD_getErrorName(e)); return 2; }

    size_t bound = ZSTD_compressBound(input_size);
    char *zbuf = (char *)malloc(bound);
    size_t zsize = ZSTD_compress2(cctx, zbuf, bound, input, input_size);
    if (ZSTD_isError(zsize)) {
        fprintf(stderr, "ZSTD_compress2 fail: %s\n", ZSTD_getErrorName(zsize));
        return 3;
    }
    printf("compressed: %zu -> %zu bytes\n", input_size, zsize);

    // Decompress with the standard decoder.
    char out[256];
    size_t dsize = ZSTD_decompress(out, sizeof(out), zbuf, zsize);
    if (ZSTD_isError(dsize)) {
        fprintf(stderr, "ZSTD_decompress fail: %s\n", ZSTD_getErrorName(dsize));
        return 4;
    }
    if (dsize != input_size) {
        fprintf(stderr, "size mismatch: got %zu want %zu\n", dsize, input_size);
        return 5;
    }
    if (memcmp(out, input, input_size) != 0) {
        fprintf(stderr, "byte mismatch\n");
        return 6;
    }
    printf("decompressed: %zu bytes, byte-identical OK\n", dsize);

    ZSTD_freeCCtx(cctx);
    free(zbuf);
    printf("\nM8 step 2a: PASS -- external sequence producer works end-to-end.\n");
    return 0;
}
