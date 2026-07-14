#ifndef ZXLE_JPEG_H
#define ZXLE_JPEG_H

#include "util.h"

/* Run cbrunsli on a JPEG buffer; verify by dbrunsli + cmp; append the brunsli
 * blob to *out. Returns 0 on success, -1 on detection miss / tooling failure /
 * round-trip mismatch / blob >= original. tmp_prefix derives scratch paths. */
int try_brunsli_buf(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *out);

/* Same contract via packJPG (packMP3's sibling; ~0.7% denser than brunsli on
 * the bench JPEG). */
int try_packjpg_buf(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *out);

/* v7 JPEG blob: u8 codec (0 = brunsli, 1 = packJPG) + payload. Tries both
 * codecs, keeps the smaller verified result, appends codec byte + payload to
 * *out. Returns 0 on success, -1 if neither codec round-trips smaller. */
int try_jpeg_buf(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *out);

/* Decode a v7 JPEG blob back to JPEG bytes (malloc'd, caller frees; *out_n
 * receives the length). Dies on unknown codec or tooling failure. */
uint8_t *unpack_jpeg_blob(const uint8_t *blob, uint32_t blob_len,
                          const char *tmp_prefix, size_t *out_n);

#endif
