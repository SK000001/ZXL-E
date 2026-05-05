#ifndef ZXLE_AR_H
#define ZXLE_AR_H

#include "util.h"

int pack_ar(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid);

#endif
