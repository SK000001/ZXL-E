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

/* Generic form of the scanner (2026-07-17): scan any byte range for verified
 * zlib streams / JPEGs starting at scan_from; succeed only when verified
 * spans cover >= min_cover_permille of n (rolls back b0/b1/recipe
 * otherwise). pack_pdf is the (5, 0) wrapper gated on %PDF-; the opaque
 * fallback in zxle.c uses (0, 50) on bucket-0 files so a stray accidental
 * stream can't convert a whole binary. Recipes decode as KIND_PDF. */
int pack_flate_scan(const uint8_t *p, size_t n, const char *tmp_prefix,
                    Buf *recipe, Buf *b0, Buf *b1,
                    size_t scan_from, unsigned min_cover_permille);

#endif
