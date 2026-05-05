#ifndef ZXLE_BZ2_H
#define ZXLE_BZ2_H

#include "util.h"

int  pack_bz2  (const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid);
void unpack_bz2(const uint8_t *recipe, size_t rlen,
                const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                FILE *out, uint64_t expected_size, const char *tmp_prefix);

#endif
