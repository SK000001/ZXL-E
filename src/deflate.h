#ifndef ZXLE_DEFLATE_H
#define ZXLE_DEFLATE_H

#include <stdint.h>
#include <stddef.h>

/* Big-endian u32 helpers (PNG chunk lengths/CRCs). */
uint32_t r32be(const uint8_t *p);
void     w32be(uint8_t *p, uint32_t v);

/* Raw-inflate a deflate stream of known compressed and uncompressed sizes.
 * Returns NULL on failure. Caller frees. */
uint8_t *raw_inflate(const uint8_t *src, uint32_t comp_size, uint32_t raw_size);

/* Raw-inflate src into a freshly allocated buffer of unknown size. */
uint8_t *raw_inflate_dyn(const uint8_t *src, size_t src_n, size_t *out_n);

/* Raw-deflate raw_bytes at level 9 default strategy into a malloc'd buffer.
 * Sets *out_len. Returns NULL on failure. */
uint8_t *raw_deflate_l9(const uint8_t *raw, uint32_t raw_size, size_t *out_len);

/* zlib-wrapped inflate / deflate (with header + adler32). */
uint8_t *zlib_inflate_dyn(const uint8_t *src, size_t src_n, size_t *out_n);
uint8_t *zlib_deflate_l9 (const uint8_t *raw, size_t raw_n, size_t *out_n);

#endif
