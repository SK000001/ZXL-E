#ifndef ZXLE_XZ_H
#define ZXLE_XZ_H

#include "util.h"

int  pack_xz  (const uint8_t *p, size_t n, const char *tmp_prefix,
               Buf *recipe, Buf *b0, Buf *b1, uint8_t bucket);
void unpack_xz(const uint8_t *recipe, size_t rlen,
               Solids *s,
               FILE *out, uint64_t expected_size, const char *tmp_prefix);

#endif
