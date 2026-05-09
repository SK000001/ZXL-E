#ifndef ZXLE_TAR_H
#define ZXLE_TAR_H

#include "util.h"

int pack_tar(const uint8_t *p, size_t n, const char *tmp_prefix,
             Buf *recipe, Buf *b0, Buf *b1);

#endif
