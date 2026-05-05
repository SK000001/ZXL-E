#ifndef ZXLE_ZST_H
#define ZXLE_ZST_H

#include "util.h"

int  pack_zst  (const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid);
void unpack_zst(const uint8_t *recipe, size_t rlen,
                const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                FILE *out, uint64_t expected_size, const char *tmp_prefix);

#endif
