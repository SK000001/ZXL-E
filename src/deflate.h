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

/* Raw-inflate a deflate stream of unknown length; reports how many input bytes
 * the stream consumed (*consumed) so the caller can find what follows (e.g. a
 * gzip member trailer, then the next member). Returns malloc'd output or NULL. */
uint8_t *raw_inflate_span(const uint8_t *src, size_t src_n,
                          size_t *consumed, size_t *out_n);

/* Raw-deflate raw_bytes at level 9 default strategy into a malloc'd buffer.
 * Sets *out_len. Returns NULL on failure. */
uint8_t *raw_deflate_l9(const uint8_t *raw, uint32_t raw_size, size_t *out_len);

/* zlib-wrapped inflate / deflate (with header + adler32). */
uint8_t *zlib_inflate_dyn(const uint8_t *src, size_t src_n, size_t *out_n);
uint8_t *zlib_deflate_l9 (const uint8_t *raw, size_t raw_n, size_t *out_n);

/* v8 redeflate ladder. `raw` inflates from a raw-deflate stream `def`; find a
 * zlib (level x memLevel x strategy x windowBits) parameter set that re-deflates
 * `raw` byte-identically to `def`. On success returns 1 and writes the two
 * packed parameter bytes stored in OP_REDEFLATE_P (see kinds.h); 0 if no
 * candidate in the search set matches (caller falls through to preflate). The
 * L9-default case is handled by raw_deflate_l9 before this is called. */
int redeflate_ladder_find(const uint8_t *raw, uint32_t raw_n,
                          const uint8_t *def, uint32_t def_n,
                          uint8_t *param0, uint8_t *param1);

/* Decode side: re-deflate `raw` at the params encoded in (param0,param1).
 * Returns a malloc'd buffer (sets *out_n) or NULL on bad params / failure. */
uint8_t *redeflate_ladder_apply(const uint8_t *raw, uint32_t raw_n,
                                uint8_t param0, uint8_t param1, size_t *out_n);

#endif
