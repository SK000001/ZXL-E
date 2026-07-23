#ifndef ZXLE_KINDS_H
#define ZXLE_KINDS_H

/* Container header / kind / op constants and the recipe-layout doc-comments
 * that describe the on-disk format. The doc here is the source of truth for
 * the manifest layout consumed by zxle.c (do_pack/do_unpack) and the OP
 * vocabulary consumed by recipe.c (unpack_recipe). */

#define ZXLE_MAGIC "ZXLE"
/* v3 (2026-05-07): solid stream codec switched from zstd -19 --long=27 to
 * xz -9e --threads=1. Manifest layout is unchanged; only the trailing
 * payload's compression scheme changed.
 * v4 (2026-05-08): M6 v1 per-content-type routing. KIND_OPAQUE entries gain
 * a u8 opaque_bucket immediately after the kind byte (manifest); the
 * trailing payload is now multi-bucket: u8 num_buckets, then per-bucket
 * (u8 codec_id, u32 csize, csize bytes). codec_id: 0=xz-9e, 1=xz-9e+x86,
 * 2=zpaq-m5.
 * v5 (2026-05-08): M6 v2 extends bucket routing to non-OPAQUE container
 * kinds. Each of KIND_ZIP/TAR/AR/GZIP/BZIP2/ZSTD/XZ entries now stores a
 * u8 unwrap_bucket after the kind byte (and BEFORE the recipe length),
 * so a PE-heavy ZIP/TAR/AR/wrapped stream routes its inflated bytes
 * through bucket 1 (xz+BCJ).
 * v6 (2026-05-09): M6 v3 per-OP bucket routing. Manifest u8 unwrap_bucket
 * dropped (now redundant -- recipes carry per-OP buckets). Each recipe op
 * that consumes solid bytes carries a u8 bucket field after its existing
 * length field: OP_STORE/OP_REDEFLATE/OP_PREFLATE all gain (u8 bucket) at
 * the position immediately following (u32 raw_size). PNG/GZIP/BZIP2/XZ/
 * ZSTD recipes gain a u8 bucket field that selects which bucket the
 * inflated body bytes come from when inner_kind==0; ignored when
 * inner_kind==1 (the inner recipe's per-OP buckets handle routing).
 * v7 (2026-07-14): compressed manifest + integrity. Header becomes
 * magic/ver/flags + u32 raw_mlen + u32 comp_mlen; comp_mlen>0 means the
 * manifest block is xz -9e compressed (recipes carry raw container-structure
 * bytes -- tar headers, ZIP central directories -- that were previously
 * stored uncompressed), comp_mlen==0 means stored raw (xz not smaller).
 * Each manifest entry gains u32 crc32 (of the original file bytes, zlib
 * polynomial) between mode and kind; unpack verifies every reconstructed
 * entry against it. Trailing payload per-bucket csize widens u32 -> u64.
 * v7 also adds OP_ZIP_STORE (0x0A): ZIP/JAR entries inside tar/ar recurse
 * through pack_zip instead of falling to OP_STORE. JPEG blobs (KIND_JPEG
 * manifest blob and OP_JPEG_STORE payload) gain a leading u8 codec byte:
 * 0 = brunsli, 1 = packJPG; pack tries both and keeps the smaller.
 * Flags bit 1 = merged manifest: when bucket 0 is xz and non-empty, the
 * manifest is the first raw_mlen bytes of decoded bucket 0 and no manifest
 * block follows the header (comp_mlen written as 0). Saves one xz container
 * overhead and shares context between structural bytes and content.
 * KIND_PDF (11): %PDF- files whose embedded zlib streams (FlateDecode)
 * verify via redeflate-L9 or preflate. Recipe is a plain OP_STRUCT /
 * OP_REDEFLATE / OP_PREFLATE sequence (zlib header + adler ride as STRUCT
 * bytes) consumed by unpack_recipe -- no PDF-specific unpack code.
 * 2026-07-17 (still v7, no layout change): PDF entries inside tar/ar and
 * stored ZIP entries dispatch through pack_pdf; the nested PDF recipe rides
 * OP_ZIP_STORE, whose semantics were always "reconstruct len bytes by
 * recursing unpack_recipe on the nested recipe" -- v7 decoders since
 * 2026-07-14 handle these archives unchanged. KIND_PDF also tags generic
 * flate-scanned opaque files (pack_flate_scan: any bucket-0 file whose
 * verified zlib/JPEG spans cover >= 5%) -- identical recipe format, so
 * decode is unchanged there too.
 * v8 (2026-07-23): redeflate ladder. New recipe op OP_REDEFLATE_P (0x0B):
 * like OP_REDEFLATE but carries two param bytes after (u32 raw_size)(u8 bucket)
 * that encode the zlib (level, memLevel, strategy, windowBits) set which
 * re-deflates the solid bytes byte-identically. Emitted by pack_zip /
 * pack_flate_scan when a stream misses the L9 fast path but a stock-zlib
 * parameter set (typically level 6 -- Python zipfile / Android / git) exactly
 * reproduces it, replacing a costlier OP_PREFLATE diff. Decode re-deflates at
 * the stored params (deflate.c redeflate_ladder_apply). No other layout change.
 * v3..v8 cannot interoperate. */
#define ZXLE_VER 8

/* Top-level container kind tag (one byte per manifest entry). */
#define KIND_OPAQUE 0
#define KIND_ZIP    1
#define KIND_JPEG   2
#define KIND_MP3    3
#define KIND_PNG    4
#define KIND_GZIP   5
#define KIND_TAR    6
#define KIND_AR     7
#define KIND_BZIP2  8
#define KIND_ZSTD   9
#define KIND_XZ     10
#define KIND_PDF    11

/* Recipe ops walked by unpack_recipe (used by KIND_ZIP / KIND_TAR / KIND_AR
 * recipes; nested recipes inside KIND_GZIP / KIND_BZIP2 / KIND_ZSTD / KIND_XZ
 * also use this vocabulary when inner_kind=1).
 *
 *   0x00 STRUCT     -- (u32 len)(len bytes verbatim) headers / EOCD / pad
 *   0x01 REDEFLATE  -- (u32 raw_size)(u8 bucket); consume raw_size bytes from
 *                      solid bucket, raw-deflate L9 default-strategy, emit.
 *   0x02 STORE      -- (u32 raw_size)(u8 bucket); consume raw_size bytes from
 *                      solid bucket, emit verbatim.
 *   0x03 PREFLATE   -- (u32 raw_size)(u8 bucket)(u32 diff_len)(diff_bytes);
 *                      consume raw_size bytes from solid bucket, preflate-
 *                      rejoin to reproduce the original deflate stream
 *                      byte-identically.
 *   0x04 JPEG_STORE -- (u32 blob_len)(u8 codec)(payload); codec 0 = brunsli,
 *                      1 = packJPG. Decode to `len` JPEG bytes, write to
 *                      output. Solid not consumed. blob_len includes the
 *                      codec byte.
 *   0x05 PNG_STORE  -- (u32 png_recipe_len)(png_recipe_bytes); call unpack_png
 *                      to reconstruct `len` PNG bytes; consumes inflated IDAT
 *                      bytes from the solid stream (per the PNG recipe).
 *   0x06 GZIP_STORE -- (u32 gz_recipe_len)(gz_recipe_bytes); call unpack_gz
 *                      to reconstruct `len` gzip bytes.
 *   0x07 BZ2_STORE  -- (u32 bz2_recipe_len)(bz2_recipe_bytes); call unpack_bz2.
 *   0x08 XZ_STORE   -- (u32 xz_recipe_len)(xz_recipe_bytes); call unpack_xz.
 *   0x09 ZSTD_STORE -- (u32 zst_recipe_len)(zst_recipe_bytes); call unpack_zst.
 *   0x0A ZIP_STORE  -- (u32 recipe_len)(recipe_bytes); the nested recipe
 *                      (from pack_zip, or pack_pdf since 2026-07-17) uses
 *                      this same OP vocabulary; unpack_recipe recurses to
 *                      reconstruct `len` bytes in place.
 *   0x0B REDEFLATE_P -- (u32 raw_size)(u8 bucket)(u8 param0)(u8 param1);
 *                      consume raw_size bytes from solid bucket, re-deflate at
 *                      the zlib params packed in param0/param1 (v8; see the
 *                      version-history note and deflate.c ladder_pack), emit.
 */
#define OP_STRUCT     0x00
#define OP_REDEFLATE  0x01
#define OP_STORE      0x02
#define OP_PREFLATE   0x03
#define OP_JPEG_STORE 0x04
#define OP_PNG_STORE  0x05
#define OP_GZIP_STORE 0x06
#define OP_BZ2_STORE  0x07
#define OP_XZ_STORE   0x08
#define OP_ZSTD_STORE 0x09
#define OP_ZIP_STORE  0x0A
#define OP_REDEFLATE_P 0x0B

/* Per-kind recipe layouts (parsed by their respective pack_<kind> and
 * unpack_<kind>):
 *
 * KIND_PNG (4):
 *   u32 pre_len  pre_bytes        -- signature + chunks before the first IDAT
 *   u32 idat_count
 *   u32 idat_data_size[idat_count]
 *   u8  zlib_mode                  -- 0 = zlib L9 redeflate matches; 1 = preflate
 *   u32 raw_len                    -- inflated IDAT size (consumed from solid)
 *   u8  bucket                     -- v6: which solid bucket the raw bytes are in
 *   u32 zlib_total                 -- total length of original zlib stream
 *   [if zlib_mode==1] u8 zhdr[2] u8 adler[4] u32 diff_len diff_bytes
 *   u32 post_len  post_bytes       -- chunks after the last IDAT (incl. IEND)
 *
 * KIND_GZIP (5):
 *   u32 hdr_len  hdr_bytes
 *   u8  mode                      -- 0 = raw-deflate L9 redeflate matches; 1 = preflate
 *   u32 raw_len                   -- inflated body size
 *   u32 def_len                   -- length of original raw deflate body
 *   [if mode==1] u32 diff_len  diff_bytes
 *   u8  trailer[8]                -- CRC32 LE + ISIZE LE, verbatim
 *   u8  inner_kind                -- 0 = inflated bytes consumed verbatim; 1 = nested ustar tar
 *   u8  bucket                    -- v6: which solid bucket; only used when inner_kind==0
 *   [if inner_kind==1] u32 tar_recipe_len  tar_recipe_bytes
 *
 * KIND_BZIP2 (8):
 *   u8  block_size                -- '1'..'9'
 *   u32 raw_len   u32 orig_len
 *   u8  inner_kind
 *   u8  bucket                    -- v6: only used when inner_kind==0
 *   [if 1: u32 tar_recipe_len  tar_recipe_bytes]
 *
 * KIND_ZSTD (9):
 *   u8  level                     -- 1..22
 *   u8  long_window               -- 0 = no --long; else window log
 *   u8  flags                     -- 0x01 use_stdin (FCS suppressed); 0x02 no_check
 *   u32 raw_len   u32 orig_len
 *   u8  inner_kind
 *   u8  bucket                    -- v6: only used when inner_kind==0
 *   [if 1: u32 tar_recipe_len  tar_recipe_bytes]
 *
 * KIND_XZ (10):
 *   u8  level                     -- 0..9
 *   u8  flags                     -- 0x01 = --extreme (xz -<level>e)
 *   u32 raw_len   u32 orig_len
 *   u8  inner_kind
 *   u8  bucket                    -- v6: only used when inner_kind==0
 *   [if 1: u32 tar_recipe_len  tar_recipe_bytes]
 */

#endif
