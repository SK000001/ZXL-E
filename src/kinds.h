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
 * through bucket 1 (xz+BCJ). v3/v4/v5 cannot interoperate. */
#define ZXLE_VER 5

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

/* Recipe ops walked by unpack_recipe (used by KIND_ZIP / KIND_TAR / KIND_AR
 * recipes; nested recipes inside KIND_GZIP / KIND_BZIP2 / KIND_ZSTD / KIND_XZ
 * also use this vocabulary when inner_kind=1).
 *
 *   0x00 STRUCT     -- (u32 len)(len bytes verbatim) headers / EOCD / pad
 *   0x01 REDEFLATE  -- (u32 raw_size) consume raw_size bytes from solid,
 *                      raw-deflate L9 default-strategy, emit deflate stream.
 *   0x02 STORE      -- (u32 raw_size) consume raw_size bytes from solid,
 *                      emit verbatim.
 *   0x03 PREFLATE   -- (u32 raw_size)(u32 diff_len)(diff_bytes); consume
 *                      raw_size bytes from solid, preflate-rejoin to
 *                      reproduce the original deflate stream byte-identically.
 *   0x04 JPEG_STORE -- (u32 brn_len)(brn_bytes); brunsli-decode brn to `len`
 *                      JPEG bytes, write to output. Solid not consumed.
 *   0x05 PNG_STORE  -- (u32 png_recipe_len)(png_recipe_bytes); call unpack_png
 *                      to reconstruct `len` PNG bytes; consumes inflated IDAT
 *                      bytes from the solid stream (per the PNG recipe).
 *   0x06 GZIP_STORE -- (u32 gz_recipe_len)(gz_recipe_bytes); call unpack_gz
 *                      to reconstruct `len` gzip bytes.
 *   0x07 BZ2_STORE  -- (u32 bz2_recipe_len)(bz2_recipe_bytes); call unpack_bz2.
 *   0x08 XZ_STORE   -- (u32 xz_recipe_len)(xz_recipe_bytes); call unpack_xz.
 *   0x09 ZSTD_STORE -- (u32 zst_recipe_len)(zst_recipe_bytes); call unpack_zst.
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

/* Per-kind recipe layouts (parsed by their respective pack_<kind> and
 * unpack_<kind>):
 *
 * KIND_PNG (4):
 *   u32 pre_len  pre_bytes        -- signature + chunks before the first IDAT
 *   u32 idat_count
 *   u32 idat_data_size[idat_count]
 *   u8  zlib_mode                  -- 0 = zlib L9 redeflate matches; 1 = preflate
 *   u32 raw_len                    -- inflated IDAT size (consumed from solid)
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
 *   [if inner_kind==1] u32 tar_recipe_len  tar_recipe_bytes
 *
 * KIND_BZIP2 (8):
 *   u8  block_size                -- '1'..'9'
 *   u32 raw_len   u32 orig_len
 *   u8  inner_kind  [if 1: u32 tar_recipe_len  tar_recipe_bytes]
 *
 * KIND_ZSTD (9):
 *   u8  level                     -- 1..22
 *   u8  long_window               -- 0 = no --long; else window log
 *   u8  flags                     -- 0x01 use_stdin (FCS suppressed); 0x02 no_check
 *   u32 raw_len   u32 orig_len
 *   u8  inner_kind  [if 1: u32 tar_recipe_len  tar_recipe_bytes]
 *
 * KIND_XZ (10):
 *   u8  level                     -- 0..9
 *   u8  flags                     -- 0x01 = --extreme (xz -<level>e)
 *   u32 raw_len   u32 orig_len
 *   u8  inner_kind  [if 1: u32 tar_recipe_len  tar_recipe_bytes]
 */

#endif
