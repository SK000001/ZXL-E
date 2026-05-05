#ifndef ZXLE_PNG_H
#define ZXLE_PNG_H

#include "util.h"

extern const uint8_t PNG_SIG[8];

int  pack_png  (const uint8_t *p, size_t n, Buf *recipe, Buf *solid);
void unpack_png(const uint8_t *recipe, size_t rlen,
                const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                FILE *out, uint64_t expected_size);

#endif
