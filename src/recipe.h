#ifndef ZXLE_RECIPE_H
#define ZXLE_RECIPE_H

#include "util.h"

/* Walks an OP_* recipe, consuming inflated bytes from `solid` (advancing
 * *solid_pos) and writing reconstructed output to `out`. tmp_prefix is the
 * base path used for any scratch files needed by JPEG/PNG/gz/bz2/xz/zst
 * STORE ops. */
void unpack_recipe(const uint8_t *recipe, size_t rlen,
                   const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                   FILE *out, uint64_t expected_size,
                   const char *tmp_prefix);

#endif
