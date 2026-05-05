#ifndef ZXLE_JPEG_H
#define ZXLE_JPEG_H

#include "util.h"

/* Run cbrunsli on a JPEG buffer; verify by dbrunsli + cmp; append the brunsli
 * blob to *out. Returns 0 on success, -1 on detection miss / tooling failure /
 * round-trip mismatch / blob >= original. tmp_prefix derives scratch paths. */
int try_brunsli_buf(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *out);

#endif
