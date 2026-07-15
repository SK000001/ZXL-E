#ifndef ZXLE_PDF_H
#define ZXLE_PDF_H

#include "util.h"

/* KIND_PDF: scan a %PDF- file for embedded zlib streams (FlateDecode bodies)
 * and JPEGs (DCTDecode bodies). zlib streams verify via redeflate-L9 or
 * preflate round-trip; JPEGs go through the brunsli/packJPG codec race. The
 * recipe is a plain OP_STRUCT / OP_REDEFLATE / OP_PREFLATE / OP_JPEG_STORE
 * sequence (zlib header + adler ride as STRUCT bytes) consumed by the
 * generic unpack_recipe walker -- there is no PDF-specific unpack code.
 * Returns 0 if at least one stream verified; -1 otherwise (caller falls
 * through to KIND_OPAQUE). */
int pack_pdf(const uint8_t *p, size_t n, const char *tmp_prefix,
             Buf *recipe, Buf *b0, Buf *b1);

#endif
