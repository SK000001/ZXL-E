#ifndef ZXLE_GZ_H
#define ZXLE_GZ_H

#include "util.h"

/* pack_gz: appends inflated body bytes to b[bucket] when inner_kind==0;
 * when the inflated body is itself a ustar tar, recurses into pack_tar
 * which routes per-entry across both buckets. */
int  pack_gz  (const uint8_t *p, size_t n, const char *tmp_prefix,
               Buf *recipe, Buf *b0, Buf *b1, uint8_t bucket);
void unpack_gz(const uint8_t *recipe, size_t rlen,
               Solids *s,
               FILE *out, uint64_t expected_size, const char *tmp_prefix);

#endif
