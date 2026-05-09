#ifndef ZXLE_PNG_H
#define ZXLE_PNG_H

#include "util.h"

extern const uint8_t PNG_SIG[8];

/* PNG inflated pixel bytes always go to bucket 0 (raw image data, not x86). */
int  pack_png  (const uint8_t *p, size_t n, Buf *recipe, Buf *solid);
void unpack_png(const uint8_t *recipe, size_t rlen,
                Solids *s,
                FILE *out, uint64_t expected_size);

#endif
