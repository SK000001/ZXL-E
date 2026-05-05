#ifndef ZXLE_MP3_H
#define ZXLE_MP3_H

#include "util.h"

int looks_like_mp3(const uint8_t *p, size_t n);

/* Run packMP3 on an MP3 buffer; verify by packMP3 (decode side) + cmp; append
 * the .pmp blob to *out. Returns 0 on success, -1 on detection miss / tooling
 * failure / round-trip mismatch / blob >= original. */
int try_packmp3_buf(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *out);

#endif
