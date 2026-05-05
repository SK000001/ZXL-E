/* ZXL-E (M2: ZIP-family unwrap)
 *
 * pack:   zxle pack   <out.zxle> <files...>
 * unpack: zxle unpack <in.zxle>  <outdir>
 *
 * Container layout (v2):
 *   "ZXLE" (4)  version=2 (1)  flags (1)  manifest_size (4 LE)
 *   manifest: per entry:
 *     u16 path_len, path bytes,
 *     u64 original_size,
 *     u32 mode,
 *     u8  kind                    -- 0=opaque, 1=zip-unwrap, 2=jpeg-brunsli,
 *                                    3=mp3-packmp3, 4=png-idat, 5=gzip, 6=tar
 *     [if kind==1] u32 recipe_len, recipe_bytes
 *     [if kind==2] u32 brn_len,    brn_bytes      -- brunsli blob; entry
 *                                                    contributes nothing to
 *                                                    the solid stream
 *     [if kind==3] u32 pmp_len,    pmp_bytes      -- packmp3 blob; entry
 *                                                    contributes nothing to
 *                                                    the solid stream
 *     [if kind==4] u32 recipe_len, recipe_bytes   -- PNG recipe (see below);
 *                                                    inflated IDAT bytes go to
 *                                                    the solid stream
 *     [if kind==5] u32 recipe_len, recipe_bytes   -- gzip recipe (see below);
 *                                                    inflated body bytes go to
 *                                                    the solid stream
 *     [if kind==6] u32 recipe_len, recipe_bytes   -- tar recipe; same OP_*
 *                                                    vocabulary as kind==1.
 *                                                    Per-entry payloads route
 *                                                    via OP_STORE / OP_JPEG_STORE
 *                                                    / OP_PNG_STORE / OP_GZIP_STORE;
 *                                                    headers and padding go in
 *                                                    OP_STRUCT.
 *     [if kind==8] u32 recipe_len, recipe_bytes   -- bzip2 recipe (see below);
 *                                                    inflated body bytes go to
 *                                                    the solid stream
 *     [if kind==9] u32 recipe_len, recipe_bytes   -- zstd recipe (see below);
 *                                                    inflated body bytes go to
 *                                                    the solid stream
 *     [if kind==7] u32 recipe_len, recipe_bytes   -- ar recipe (Unix archive,
 *                                                    .a / .deb). Same OP_*
 *                                                    vocabulary; per-entry
 *                                                    payloads route via OP_STORE
 *                                                    / OP_GZIP_STORE / OP_PNG_STORE
 *                                                    / OP_JPEG_STORE; headers
 *                                                    and 2-byte alignment pad
 *                                                    go in OP_STRUCT.
 *   zstd-19 payload: solid concatenation of "raw input bytes" (kind=0) or
 *     "raw entry bytes from re-deflatable / stored ZIP entries" (kind=1),
 *     in manifest order.
 *
 * Recipe ops (kind=1): each op is (u8 tag, u32 len), followed for STRUCT by
 * len verbatim bytes:
 *   0x00 STRUCT     -- copy `len` bytes from recipe to output verbatim
 *   0x01 REDEFLATE  -- consume `len` bytes from solid stream, raw-deflate at
 *                      level 9 default strategy, write the deflate stream
 *   0x02 STORE      -- copy `len` bytes from solid stream to output verbatim
 *   0x03 PREFLATE   -- (u32 diff_len)(diff_bytes); consume `len` bytes from
 *                      solid stream, call preflate_reencode(unp, diff) to
 *                      reproduce the original deflate stream byte-identically.
 *   0x04 JPEG_STORE -- (u32 brn_len)(brn_bytes); brunsli-decode brn to `len`
 *                      JPEG bytes, write to output. Solid not consumed. Used
 *                      for STORED ZIP entries whose payload is a JPEG.
 *   0x05 PNG_STORE  -- (u32 png_recipe_len)(png_recipe_bytes); call unpack_png
 *                      to reconstruct `len` PNG bytes; consumes inflated IDAT
 *                      bytes from the solid stream (per the PNG recipe). Used
 *                      for STORED ZIP entries whose payload is a PNG.
 *   0x06 GZIP_STORE -- (u32 gz_recipe_len)(gz_recipe_bytes); call unpack_gz
 *                      to reconstruct `len` gzip bytes; consumes inflated body
 *                      bytes from the solid stream (per the embedded gzip
 *                      recipe). Used for gzip files inside tar/ar entries.
 *   0x07 BZ2_STORE  -- (u32 bz2_recipe_len)(bz2_recipe_bytes); call unpack_bz2
 *                      to reconstruct `len` bzip2 bytes; consumes inflated body
 *                      bytes from the solid stream (per the embedded bzip2
 *                      recipe). Used for bzip2 files inside tar/ar entries.
 *
 * ZIP unwrap scope: zlib-DEFLATE / stored entries; ZIP64 / encryption /
 * DEFLATE64 / BZIP2 / LZMA / prefix bytes -> falls back to KIND_OPAQUE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <zlib.h>
#ifdef _WIN32
#include <direct.h>
#define ZXLE_MKDIR(p) _mkdir(p)
#define ZXLE_DEVNULL "NUL"
#else
#define ZXLE_MKDIR(p) mkdir((p), 0755)
#define ZXLE_DEVNULL "/dev/null"
#endif

#define ZXLE_MAGIC "ZXLE"
#define ZXLE_VER 2

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

/* PNG (kind=4) recipe layout (parsed by pack_png/unpack_png; not by
 * unpack_recipe — KIND_PNG does not use the OP_* tags):
 *   u32 pre_len  pre_bytes        -- signature + chunks before the first IDAT
 *   u32 idat_count
 *   u32 idat_data_size[idat_count] -- data length of each original IDAT chunk
 *   u8  zlib_mode                  -- 0 = zlib L9 redeflate matches original,
 *                                     1 = preflate over the raw deflate body
 *   u32 raw_len                    -- inflated IDAT size (consumed from solid)
 *   u32 zlib_total                 -- total length of original zlib stream
 *   [if zlib_mode==1]
 *     u8  zhdr[2]                  -- original zlib header
 *     u8  adler[4]                 -- original zlib adler32 trailer (BE)
 *     u32 diff_len  diff_bytes     -- preflate reconstruction info
 *   u32 post_len  post_bytes       -- chunks after the last IDAT (incl. IEND)
 */

/* Gzip (kind=5) recipe layout (parsed by pack_gz/unpack_gz):
 *   u32 hdr_len  hdr_bytes        -- gzip header (10 + optional FEXTRA/FNAME/
 *                                     FCOMMENT/FHCRC) verbatim
 *   u8  mode                      -- 0 = raw-deflate L9 redeflate matches,
 *                                     1 = preflate over the raw deflate body
 *   u32 raw_len                   -- inflated body size
 *   u32 def_len                   -- length of original raw deflate body
 *   [if mode==1] u32 diff_len  diff_bytes
 *   u8  trailer[8]                -- CRC32 LE + ISIZE LE, verbatim
 *   u8  inner_kind                -- 0 = inflated body bytes consumed from
 *                                     solid verbatim;
 *                                   1 = inflated body is a ustar tar; followed
 *                                     by a nested tar recipe whose ops consume
 *                                     from solid (M3e-targz).
 *   [if inner_kind==1] u32 tar_recipe_len  tar_recipe_bytes
 *
 * Single-member only; multi-member gzip falls back to KIND_OPAQUE.
 */

/* Bzip2 (kind=8) recipe layout (parsed by pack_bz2/unpack_bz2):
 *   u8  block_size                -- '1'..'9' (BZh<n>); replayed at unpack time
 *   u32 raw_len                   -- decompressed payload size
 *   u32 orig_len                  -- original .bz2 file size (sanity)
 *   u8  inner_kind                -- 0 = inflated bytes consumed from solid
 *                                     verbatim; 1 = inflated body is a ustar
 *                                     tar (nested tar recipe consumes solid).
 *   [if inner_kind==1] u32 tar_recipe_len  tar_recipe_bytes
 *
 * Reproducibility: pack-time runs `bzip2 -<n>` on the inflated bytes and cmp's
 * against the original. On any mismatch we fall through to KIND_OPAQUE, so the
 * exact bzip2 binary used at pack/unpack time must produce byte-identical
 * output for the chosen block size (verified by the bench round-trip step).
 */

/* Zstd (kind=9) recipe layout (parsed by pack_zst/unpack_zst):
 *   u8  level                     -- zstd compression level (1..22)
 *   u8  long_window               -- 0 = no --long; else window log (e.g. 27)
 *   u32 raw_len                   -- decompressed payload size
 *   u32 orig_len                  -- original .zst file size (sanity)
 *   u8  inner_kind                -- 0 = inflated bytes consumed from solid
 *                                     verbatim; 1 = inflated body is a ustar
 *                                     tar (nested tar recipe consumes solid).
 *   [if inner_kind==1] u32 tar_recipe_len  tar_recipe_bytes
 *
 * Reproducibility: pack-time probes a small ladder of (level, long_window)
 * combos and cmp's the re-encode against the original. First match wins;
 * fall through to KIND_OPAQUE if none match.
 */

#define OP_STRUCT     0x00
#define OP_REDEFLATE  0x01
#define OP_STORE      0x02
#define OP_PREFLATE   0x03
#define OP_JPEG_STORE 0x04
#define OP_PNG_STORE  0x05
#define OP_GZIP_STORE 0x06
#define OP_BZ2_STORE  0x07

/* Defined in preflate_shim.cpp; both return 1 on success, 0 on failure.
 * Out buffers are malloc'd; release with zxle_preflate_free. */
int  zxle_preflate_split(const uint8_t *deflate, size_t n,
                         uint8_t **out_unp, size_t *out_unp_n,
                         uint8_t **out_diff, size_t *out_diff_n);
int  zxle_preflate_join (const uint8_t *unp, size_t unp_n,
                         const uint8_t *diff, size_t diff_n,
                         uint8_t **out_def, size_t *out_def_n);
void zxle_preflate_free (void *p);

static void die(const char *msg) {
    fprintf(stderr, "zxle: %s", msg);
    if (errno) fprintf(stderr, " (%s)", strerror(errno));
    fputc('\n', stderr);
    exit(1);
}

static void wu16(FILE *f, uint16_t v) { fputc(v&0xFF,f); fputc((v>>8)&0xFF,f); }
static void wu32(FILE *f, uint32_t v) { for (int i=0;i<4;i++) fputc((v>>(i*8))&0xFF,f); }
static void wu64(FILE *f, uint64_t v) { for (int i=0;i<8;i++) fputc((v>>(i*8))&0xFF,f); }

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

static void run(const char *cmd) {
    int rc = system(cmd);
    if (rc != 0) { fprintf(stderr, "zxle: command failed (%d): %s\n", rc, cmd); exit(1); }
}

/* Quietly try a command. Returns 0 on success, non-zero rc otherwise.
 * Used for availability checks and best-effort routing. */
static int try_run(const char *cmd) { return system(cmd); }

static long long fsize(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) die("stat");
    return (long long)st.st_size;
}

static const char *basename_of(const char *p) {
    const char *s = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') s = q + 1;
    return s;
}

/* Growable byte buffer. */
typedef struct {
    uint8_t *p;
    size_t   n;
    size_t   cap;
} Buf;

static void buf_init(Buf *b) { b->p = NULL; b->n = 0; b->cap = 0; }
static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }
static void buf_reserve(Buf *b, size_t want) {
    if (b->cap >= want) return;
    size_t c = b->cap ? b->cap : 64;
    while (c < want) c *= 2;
    uint8_t *np = realloc(b->p, c);
    if (!np) die("buf realloc");
    b->p = np; b->cap = c;
}
static void buf_append(Buf *b, const void *d, size_t n) {
    buf_reserve(b, b->n + n);
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void buf_u8(Buf *b, uint8_t v)   { buf_append(b, &v, 1); }
static void buf_u32(Buf *b, uint32_t v) {
    uint8_t t[4]; for (int i=0;i<4;i++) t[i] = (v>>(i*8))&0xFF;
    buf_append(b, t, 4);
}

static uint8_t *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "fopen %s\n", path); die("fopen"); }
    if (fseek(f, 0, SEEK_END) != 0) die("fseek");
    long sz = ftell(f);
    if (sz < 0) die("ftell");
    rewind(f);
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) die("malloc");
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) die("fread whole");
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

/* ---------- ZIP parsing ---------- */

typedef struct {
    uint64_t lfh_off;       /* offset of local file header in original ZIP */
    uint64_t payload_off;   /* offset of compressed payload */
    uint32_t comp_size;
    uint32_t raw_size;
    uint16_t method;        /* 0 = stored, 8 = deflate */
    uint16_t gp_flag;
    uint32_t crc32;
} ZipEntry;

/* Find EOCD by scanning backward from end. Returns offset, or (size_t)-1. */
static size_t find_eocd(const uint8_t *p, size_t n) {
    if (n < 22) return (size_t)-1;
    size_t lo = n > 65557 ? n - 65557 : 0;
    for (size_t i = n - 22; i + 1 > lo; i--) {
        if (p[i]==0x50 && p[i+1]==0x4B && p[i+2]==0x05 && p[i+3]==0x06) return i;
        if (i == 0) break;
    }
    return (size_t)-1;
}

/* Try to parse a ZIP central directory. Returns 0 on success, -1 if not a
 * supported ZIP (unknown method, ZIP64, encryption, prefix bytes, etc.).
 * On success, fills *out_entries (caller frees), *out_count, *out_cd_off,
 * *out_cd_len, *out_eocd_off, *out_eocd_len. */
static int zip_parse(const uint8_t *p, size_t n,
                     ZipEntry **out_entries, uint32_t *out_count,
                     size_t *out_cd_off, size_t *out_cd_len,
                     size_t *out_eocd_off, size_t *out_eocd_len) {
    if (n < 22) return -1;
    if (!(p[0]==0x50 && p[1]==0x4B && (p[2]==0x03 || p[2]==0x05))) return -1;

    size_t eocd = find_eocd(p, n);
    if (eocd == (size_t)-1) return -1;

    uint16_t disk         = r16(p + eocd + 4);
    uint16_t cd_disk      = r16(p + eocd + 6);
    uint16_t entries_disk = r16(p + eocd + 8);
    uint16_t entries_tot  = r16(p + eocd + 10);
    uint32_t cd_size      = r32(p + eocd + 12);
    uint32_t cd_off       = r32(p + eocd + 16);
    uint16_t comment_len  = r16(p + eocd + 20);

    if (disk != 0 || cd_disk != 0) return -1;
    if (entries_disk != entries_tot) return -1;
    if (cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu || entries_tot == 0xFFFFu) return -1; /* ZIP64 */
    if ((size_t)eocd + 22 + comment_len != n) return -1; /* trailing data */
    if ((size_t)cd_off + cd_size != eocd) return -1;     /* CD must end at EOCD */

    ZipEntry *ents = calloc(entries_tot ? entries_tot : 1, sizeof(ZipEntry));
    if (!ents) die("calloc entries");

    size_t cur = cd_off;
    for (uint32_t i = 0; i < entries_tot; i++) {
        if (cur + 46 > eocd) { free(ents); return -1; }
        if (!(p[cur]==0x50 && p[cur+1]==0x4B && p[cur+2]==0x01 && p[cur+3]==0x02)) { free(ents); return -1; }
        uint16_t method   = r16(p + cur + 10);
        uint16_t gpflag   = r16(p + cur + 8);
        uint32_t crc      = r32(p + cur + 16);
        uint32_t comp     = r32(p + cur + 20);
        uint32_t raw      = r32(p + cur + 24);
        uint16_t fnlen    = r16(p + cur + 28);
        uint16_t exlen    = r16(p + cur + 30);
        uint16_t cmlen    = r16(p + cur + 32);
        uint16_t diskno   = r16(p + cur + 34);
        uint32_t lfh_off  = r32(p + cur + 42);

        if (diskno != 0) { free(ents); return -1; }
        if (comp == 0xFFFFFFFFu || raw == 0xFFFFFFFFu || lfh_off == 0xFFFFFFFFu) { free(ents); return -1; } /* ZIP64 */
        if (gpflag & 0x0001) { free(ents); return -1; } /* encrypted */
        if (method != 0 && method != 8) { free(ents); return -1; } /* stored or deflate only */
        if ((size_t)lfh_off + 30 > n) { free(ents); return -1; }
        if (!(p[lfh_off]==0x50 && p[lfh_off+1]==0x4B && p[lfh_off+2]==0x03 && p[lfh_off+3]==0x04)) { free(ents); return -1; }

        uint16_t lf_fnlen = r16(p + lfh_off + 26);
        uint16_t lf_exlen = r16(p + lfh_off + 28);
        size_t payload = (size_t)lfh_off + 30 + lf_fnlen + lf_exlen;
        if (payload + comp > n) { free(ents); return -1; }

        ents[i].lfh_off     = lfh_off;
        ents[i].payload_off = payload;
        ents[i].comp_size   = comp;
        ents[i].raw_size    = raw;
        ents[i].method      = method;
        ents[i].gp_flag     = gpflag;
        ents[i].crc32       = crc;

        cur += 46 + fnlen + exlen + cmlen;
    }
    if (cur != eocd) { free(ents); return -1; }

    /* Verify first LFH starts at offset 0 (no prefix bytes). Sort by lfh_off
     * just in case CD order differs from physical order. */
    for (uint32_t i = 0; i < entries_tot; i++) {
        for (uint32_t j = i+1; j < entries_tot; j++) {
            if (ents[j].lfh_off < ents[i].lfh_off) {
                ZipEntry t = ents[i]; ents[i] = ents[j]; ents[j] = t;
            }
        }
    }
    if (entries_tot > 0 && ents[0].lfh_off != 0) { free(ents); return -1; }

    *out_entries = ents;
    *out_count   = entries_tot;
    *out_cd_off  = cd_off;
    *out_cd_len  = cd_size;
    *out_eocd_off = eocd;
    *out_eocd_len = 22 + comment_len;
    return 0;
}

static int try_brunsli_buf(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *out);
static const uint8_t PNG_SIG[8];
static int  pack_png  (const uint8_t *p, size_t n, Buf *recipe, Buf *solid);
static void unpack_png(const uint8_t *recipe, size_t rlen,
                       const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                       FILE *out, uint64_t expected_size);
static int  pack_gz   (const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid);
static void unpack_gz (const uint8_t *recipe, size_t rlen,
                       const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                       FILE *out, uint64_t expected_size, const char *tmp_prefix);
static int  pack_bz2  (const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid);
static void unpack_bz2(const uint8_t *recipe, size_t rlen,
                       const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                       FILE *out, uint64_t expected_size, const char *tmp_prefix);
static int  pack_zst  (const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid);
static void unpack_zst(const uint8_t *recipe, size_t rlen,
                       const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                       FILE *out, uint64_t expected_size, const char *tmp_prefix);
static int  pack_tar  (const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid);
static void unpack_recipe(const uint8_t *recipe, size_t rlen,
                          const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                          FILE *out, uint64_t expected_size,
                          const char *tmp_prefix);

/* Raw-inflate `comp_size` bytes at src into a freshly allocated buffer of
 * exactly raw_size bytes. Returns NULL on failure. */
static uint8_t *raw_inflate(const uint8_t *src, uint32_t comp_size, uint32_t raw_size) {
    uint8_t *out = malloc(raw_size ? raw_size : 1);
    if (!out) die("malloc inflate");
    z_stream z = {0};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) { free(out); return NULL; }
    z.next_in = (Bytef *)src;
    z.avail_in = comp_size;
    z.next_out = out;
    z.avail_out = raw_size;
    int rc = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (rc != Z_STREAM_END || z.total_out != raw_size) { free(out); return NULL; }
    return out;
}

/* Raw-deflate raw_bytes at level 9 default strategy into a malloc'd buffer.
 * Sets *out_len. Returns NULL on failure. */
static uint8_t *raw_deflate_l9(const uint8_t *raw, uint32_t raw_size, size_t *out_len) {
    z_stream z = {0};
    if (deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;
    size_t bound = deflateBound(&z, raw_size);
    uint8_t *out = malloc(bound ? bound : 1);
    if (!out) { deflateEnd(&z); die("malloc deflate"); }
    z.next_in = (Bytef *)raw;
    z.avail_in = raw_size;
    z.next_out = out;
    z.avail_out = bound;
    int rc = deflate(&z, Z_FINISH);
    if (rc != Z_STREAM_END) { deflateEnd(&z); free(out); return NULL; }
    *out_len = z.total_out;
    deflateEnd(&z);
    return out;
}

/* ---------- pack: kind=1 ZIP unwrap ---------- */

/* On success: builds recipe in *recipe and appends raw entry bytes to *solid.
 * Returns 0 on success, -1 if input isn't a usable ZIP (caller falls back to
 * opaque). */
static int pack_zip(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid) {
    ZipEntry *ents = NULL;
    uint32_t count = 0;
    size_t cd_off = 0, cd_len = 0, eocd_off = 0, eocd_len = 0;
    if (zip_parse(p, n, &ents, &count, &cd_off, &cd_len, &eocd_off, &eocd_len) != 0) return -1;

    size_t cursor = 0;
    int redeflated = 0, preflated = 0, store_orig = 0, stored_method = 0, jpeg_stored = 0, png_stored = 0;

    for (uint32_t i = 0; i < count; i++) {
        ZipEntry *e = &ents[i];
        /* Gap (or LFH). cursor..lfh_off then lfh_off..payload_off */
        if (e->lfh_off > cursor) {
            buf_u8(recipe, OP_STRUCT);
            buf_u32(recipe, (uint32_t)(e->lfh_off - cursor));
            buf_append(recipe, p + cursor, e->lfh_off - cursor);
        }
        size_t lfh_size = e->payload_off - e->lfh_off;
        buf_u8(recipe, OP_STRUCT);
        buf_u32(recipe, (uint32_t)lfh_size);
        buf_append(recipe, p + e->lfh_off, lfh_size);

        if (e->method == 0) {
            /* Stored: raw bytes are the payload bytes. Try brunsli for JPEGs. */
            if (e->raw_size != e->comp_size) { free(ents); return -1; }
            int handled = 0;
            if (!handled && e->raw_size >= 8 &&
                memcmp(p + e->payload_off, PNG_SIG, 8) == 0) {
                Buf png_recipe; buf_init(&png_recipe);
                if (pack_png(p + e->payload_off, e->raw_size, &png_recipe, solid) == 0) {
                    buf_u8(recipe, OP_PNG_STORE);
                    buf_u32(recipe, e->raw_size);
                    buf_u32(recipe, (uint32_t)png_recipe.n);
                    buf_append(recipe, png_recipe.p, png_recipe.n);
                    png_stored++;
                    handled = 1;
                }
                buf_free(&png_recipe);
            }
            if (!handled && e->raw_size >= 4 &&
                p[e->payload_off]   == 0xFF &&
                p[e->payload_off+1] == 0xD8 &&
                p[e->payload_off+2] == 0xFF) {
                char tp[2048];
                snprintf(tp, sizeof(tp), "%s.zj.%u", tmp_prefix, i);
                Buf brn; buf_init(&brn);
                if (try_brunsli_buf(p + e->payload_off, e->raw_size, tp, &brn) == 0) {
                    buf_u8(recipe, OP_JPEG_STORE);
                    buf_u32(recipe, e->raw_size);
                    buf_u32(recipe, (uint32_t)brn.n);
                    buf_append(recipe, brn.p, brn.n);
                    jpeg_stored++;
                    handled = 1;
                }
                buf_free(&brn);
            }
            if (!handled) {
                buf_u8(recipe, OP_STORE);
                buf_u32(recipe, e->raw_size);
                buf_append(solid, p + e->payload_off, e->raw_size);
                stored_method++;
            }
        } else {
            /* Deflate: try re-deflate L9 default strategy. */
            uint8_t *raw = raw_inflate(p + e->payload_off, e->comp_size, e->raw_size);
            if (!raw) {
                /* malformed deflate: store original verbatim */
                buf_u8(recipe, OP_STRUCT);
                buf_u32(recipe, e->comp_size);
                buf_append(recipe, p + e->payload_off, e->comp_size);
                store_orig++;
            } else {
                size_t redef_len = 0;
                uint8_t *redef = raw_deflate_l9(raw, e->raw_size, &redef_len);
                if (redef && redef_len == e->comp_size && memcmp(redef, p + e->payload_off, e->comp_size) == 0) {
                    buf_u8(recipe, OP_REDEFLATE);
                    buf_u32(recipe, e->raw_size);
                    buf_append(solid, raw, e->raw_size);
                    redeflated++;
                    free(redef);
                    free(raw);
                } else {
                    free(redef);
                    /* Try preflate split + verify-by-rejoin. */
                    uint8_t *unp = NULL, *diff = NULL, *rejoin = NULL;
                    size_t unp_n = 0, diff_n = 0, rejoin_n = 0;
                    int pf_ok = 0;
                    if (zxle_preflate_split(p + e->payload_off, e->comp_size,
                                            &unp, &unp_n, &diff, &diff_n)) {
                        if (unp_n == e->raw_size &&
                            zxle_preflate_join(unp, unp_n, diff, diff_n,
                                               &rejoin, &rejoin_n) &&
                            rejoin_n == e->comp_size &&
                            memcmp(rejoin, p + e->payload_off, e->comp_size) == 0) {
                            buf_u8(recipe, OP_PREFLATE);
                            buf_u32(recipe, e->raw_size);
                            buf_u32(recipe, (uint32_t)diff_n);
                            buf_append(recipe, diff, diff_n);
                            buf_append(solid, unp, e->raw_size);
                            preflated++;
                            pf_ok = 1;
                        }
                    }
                    zxle_preflate_free(unp);
                    zxle_preflate_free(diff);
                    zxle_preflate_free(rejoin);
                    if (!pf_ok) {
                        buf_u8(recipe, OP_STRUCT);
                        buf_u32(recipe, e->comp_size);
                        buf_append(recipe, p + e->payload_off, e->comp_size);
                        store_orig++;
                    }
                    free(raw);
                }
            }
        }
        cursor = e->payload_off + e->comp_size;
    }

    /* Tail: from cursor to end (CD + EOCD + comment). */
    if (cursor < n) {
        buf_u8(recipe, OP_STRUCT);
        buf_u32(recipe, (uint32_t)(n - cursor));
        buf_append(recipe, p + cursor, n - cursor);
    }

    fprintf(stderr, "    zip: %u entries (%d redeflate, %d preflate, %d store-orig, %d stored, %d jpeg-store, %d png-store)\n",
            count, redeflated, preflated, store_orig, stored_method, jpeg_stored, png_stored);

    free(ents);
    return 0;
}

/* ---------- unpack: walk recipe ---------- */

static void unpack_recipe(const uint8_t *recipe, size_t rlen,
                          const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                          FILE *out, uint64_t expected_size,
                          const char *tmp_prefix) {
    size_t r = 0;
    uint64_t written = 0;
    while (r < rlen) {
        if (r + 5 > rlen) die("recipe truncated");
        uint8_t op = recipe[r]; r += 1;
        uint32_t len = r32(recipe + r); r += 4;
        if (op == OP_STRUCT) {
            if (r + len > rlen) die("recipe STRUCT overflow");
            if (fwrite(recipe + r, 1, len, out) != len) die("fwrite STRUCT");
            r += len;
            written += len;
        } else if (op == OP_REDEFLATE) {
            if (*solid_pos + len > solid_len) die("solid REDEFLATE overflow");
            size_t df_len = 0;
            uint8_t *df = raw_deflate_l9(solid + *solid_pos, len, &df_len);
            if (!df) die("raw_deflate_l9");
            if (fwrite(df, 1, df_len, out) != df_len) die("fwrite REDEFLATE");
            free(df);
            *solid_pos += len;
            written += df_len;
        } else if (op == OP_STORE) {
            if (*solid_pos + len > solid_len) die("solid STORE overflow");
            if (fwrite(solid + *solid_pos, 1, len, out) != len) die("fwrite STORE");
            *solid_pos += len;
            written += len;
        } else if (op == OP_JPEG_STORE) {
            if (r + 4 > rlen) die("recipe JPEG_STORE brn_len truncated");
            uint32_t brn_len = r32(recipe + r); r += 4;
            if (r + brn_len > rlen) die("recipe JPEG_STORE brn overflow");
            char tmp_brn[2048], tmp_jpg[2048], cmd[4096];
            snprintf(tmp_brn, sizeof(tmp_brn), "%s.uj.brn.tmp", tmp_prefix);
            snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.uj.jpg.tmp", tmp_prefix);
            FILE *bf = fopen(tmp_brn, "wb");
            if (!bf) die("fopen tmp brn");
            if (brn_len > 0 && fwrite(recipe + r, 1, brn_len, bf) != brn_len) die("fwrite tmp brn");
            fclose(bf);
            snprintf(cmd, sizeof(cmd), "dbrunsli \"%s\" \"%s\" >%s 2>&1", tmp_brn, tmp_jpg, ZXLE_DEVNULL);
            run(cmd);
            unlink(tmp_brn);
            size_t got_n = 0;
            uint8_t *got = read_whole_file(tmp_jpg, &got_n);
            unlink(tmp_jpg);
            if (got_n != len) die("JPEG_STORE size mismatch");
            if (fwrite(got, 1, got_n, out) != got_n) die("fwrite JPEG_STORE");
            free(got);
            r += brn_len;
            written += len;
        } else if (op == OP_PNG_STORE) {
            if (r + 4 > rlen) die("recipe PNG_STORE recipe_len truncated");
            uint32_t prl = r32(recipe + r); r += 4;
            if (r + prl > rlen) die("recipe PNG_STORE recipe overflow");
            unpack_png(recipe + r, prl, solid, solid_len, solid_pos, out, len);
            r += prl;
            written += len;
        } else if (op == OP_GZIP_STORE) {
            if (r + 4 > rlen) die("recipe GZIP_STORE recipe_len truncated");
            uint32_t grl = r32(recipe + r); r += 4;
            if (r + grl > rlen) die("recipe GZIP_STORE recipe overflow");
            unpack_gz(recipe + r, grl, solid, solid_len, solid_pos, out, len, tmp_prefix);
            r += grl;
            written += len;
        } else if (op == OP_BZ2_STORE) {
            if (r + 4 > rlen) die("recipe BZ2_STORE recipe_len truncated");
            uint32_t brl = r32(recipe + r); r += 4;
            if (r + brl > rlen) die("recipe BZ2_STORE recipe overflow");
            unpack_bz2(recipe + r, brl, solid, solid_len, solid_pos, out, len, tmp_prefix);
            r += brl;
            written += len;
        } else if (op == OP_PREFLATE) {
            if (r + 4 > rlen) die("recipe PREFLATE diff_len truncated");
            uint32_t diff_len = r32(recipe + r); r += 4;
            if (r + diff_len > rlen) die("recipe PREFLATE diff overflow");
            if (*solid_pos + len > solid_len) die("solid PREFLATE overflow");
            uint8_t *def = NULL; size_t def_n = 0;
            if (!zxle_preflate_join(solid + *solid_pos, len,
                                    recipe + r, diff_len,
                                    &def, &def_n)) die("preflate_reencode");
            if (fwrite(def, 1, def_n, out) != def_n) die("fwrite PREFLATE");
            zxle_preflate_free(def);
            r += diff_len;
            *solid_pos += len;
            written += def_n;
        } else {
            die("unknown recipe op");
        }
    }
    if (written != expected_size) die("recipe size mismatch");
}

/* ---------- PNG helpers ---------- */

static const uint8_t PNG_SIG[8] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};

static uint32_t r32be(const uint8_t *p) {
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | (uint32_t)p[3];
}
static void w32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v>>24); p[1] = (uint8_t)(v>>16); p[2] = (uint8_t)(v>>8); p[3] = (uint8_t)v;
}

/* zlib-wrapped inflate with unknown output size. Returns malloc'd buffer or NULL. */
static uint8_t *zlib_inflate_dyn(const uint8_t *src, size_t src_n, size_t *out_n) {
    z_stream z = {0};
    if (inflateInit(&z) != Z_OK) return NULL;
    size_t cap = src_n * 4 + 4096;
    uint8_t *out = malloc(cap);
    if (!out) { inflateEnd(&z); die("malloc inflate"); }
    z.next_in = (Bytef *)src;
    z.avail_in = (uInt)src_n;
    z.next_out = out;
    z.avail_out = (uInt)cap;
    for (;;) {
        int rc = inflate(&z, Z_FINISH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR || rc == Z_OK) {
            size_t newcap = cap * 2;
            uint8_t *no = realloc(out, newcap);
            if (!no) { inflateEnd(&z); free(out); die("realloc inflate"); }
            out = no;
            z.next_out = out + z.total_out;
            z.avail_out = (uInt)(newcap - z.total_out);
            cap = newcap;
            continue;
        }
        inflateEnd(&z); free(out); return NULL;
    }
    *out_n = z.total_out;
    inflateEnd(&z);
    return out;
}

/* zlib-wrapped deflate, level 9, default strategy. */
static uint8_t *zlib_deflate_l9(const uint8_t *raw, size_t raw_n, size_t *out_n) {
    z_stream z = {0};
    if (deflateInit2(&z, 9, Z_DEFLATED, MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;
    size_t bound = deflateBound(&z, (uLong)raw_n);
    uint8_t *out = malloc(bound ? bound : 1);
    if (!out) { deflateEnd(&z); die("malloc zlib_deflate"); }
    z.next_in = (Bytef *)raw;
    z.avail_in = (uInt)raw_n;
    z.next_out = out;
    z.avail_out = (uInt)bound;
    int rc = deflate(&z, Z_FINISH);
    if (rc != Z_STREAM_END) { deflateEnd(&z); free(out); return NULL; }
    *out_n = z.total_out;
    deflateEnd(&z);
    return out;
}

/* Pack a top-level PNG. Builds *recipe and appends inflated IDAT bytes to
 * *solid. Returns 0 on success, -1 to fall back to KIND_OPAQUE. */
static int pack_png(const uint8_t *p, size_t n, Buf *recipe, Buf *solid) {
    if (n < 8 + 12 || memcmp(p, PNG_SIG, 8) != 0) return -1;

    /* Walk chunks. Locate IDAT range and concatenate IDAT data. */
    size_t pre_end = 0;          /* offset where first IDAT chunk header begins */
    size_t post_start = 0;       /* offset right after last IDAT's CRC */
    Buf zlib_concat; buf_init(&zlib_concat);
    Buf idat_sizes; buf_init(&idat_sizes);  /* u32 LE per IDAT data length */
    uint32_t idat_count = 0;
    int seen_idat = 0, seen_post_idat = 0;

    size_t cur = 8;
    while (cur + 12 <= n) {
        uint32_t clen = r32be(p + cur);
        if ((size_t)clen + 12 > n - cur) { buf_free(&zlib_concat); buf_free(&idat_sizes); return -1; }
        const uint8_t *type = p + cur + 4;
        const uint8_t *data = p + cur + 8;
        int is_idat = (type[0]=='I' && type[1]=='D' && type[2]=='A' && type[3]=='T');

        if (is_idat) {
            if (seen_post_idat) { buf_free(&zlib_concat); buf_free(&idat_sizes); return -1; } /* non-contiguous */
            if (!seen_idat) { pre_end = cur; seen_idat = 1; }
            buf_append(&zlib_concat, data, clen);
            buf_u32(&idat_sizes, clen);
            idat_count++;
        } else if (seen_idat) {
            if (!seen_post_idat) { post_start = cur; seen_post_idat = 1; }
        }

        cur += 12 + clen;
        if (type[0]=='I' && type[1]=='E' && type[2]=='N' && type[3]=='D') break;
    }
    if (!seen_idat || idat_count == 0) { buf_free(&zlib_concat); buf_free(&idat_sizes); return -1; }
    if (!seen_post_idat) post_start = cur; /* no chunks after IDAT block */

    /* Inflate concatenated IDAT zlib stream. */
    size_t raw_n = 0;
    uint8_t *raw = zlib_inflate_dyn(zlib_concat.p, zlib_concat.n, &raw_n);
    if (!raw) { buf_free(&zlib_concat); buf_free(&idat_sizes); return -1; }
    if (raw_n > 0xFFFFFFFFu) { free(raw); buf_free(&zlib_concat); buf_free(&idat_sizes); return -1; }

    /* Try mode 0: zlib L9 default-strategy redeflate must match byte-for-byte. */
    int mode = -1;
    Buf preflate_diff; buf_init(&preflate_diff);
    uint8_t zhdr[2] = {0,0}, adler[4] = {0,0,0,0};

    size_t redef_n = 0;
    uint8_t *redef = zlib_deflate_l9(raw, raw_n, &redef_n);
    if (redef && redef_n == zlib_concat.n &&
        memcmp(redef, zlib_concat.p, zlib_concat.n) == 0) {
        mode = 0;
    }
    free(redef);

    /* Mode 1: preflate over the raw deflate body (zlib stream minus 2-byte
     * header and 4-byte adler trailer). */
    if (mode < 0 && zlib_concat.n >= 6) {
        zhdr[0] = zlib_concat.p[0];
        zhdr[1] = zlib_concat.p[1];
        memcpy(adler, zlib_concat.p + zlib_concat.n - 4, 4);
        const uint8_t *def = zlib_concat.p + 2;
        size_t def_n = zlib_concat.n - 6;
        uint8_t *unp = NULL, *diff = NULL, *rejoin = NULL;
        size_t unp_n = 0, diff_n = 0, rejoin_n = 0;
        if (zxle_preflate_split(def, def_n, &unp, &unp_n, &diff, &diff_n)) {
            if (unp_n == raw_n && memcmp(unp, raw, raw_n) == 0 &&
                zxle_preflate_join(unp, unp_n, diff, diff_n, &rejoin, &rejoin_n) &&
                rejoin_n == def_n &&
                memcmp(rejoin, def, def_n) == 0) {
                buf_append(&preflate_diff, diff, diff_n);
                mode = 1;
            }
        }
        zxle_preflate_free(unp);
        zxle_preflate_free(diff);
        zxle_preflate_free(rejoin);
    }

    if (mode < 0) {
        free(raw);
        buf_free(&zlib_concat);
        buf_free(&idat_sizes);
        buf_free(&preflate_diff);
        return -1;
    }

    /* Build recipe. */
    uint32_t pre_len  = (uint32_t)pre_end;
    uint32_t post_len = (uint32_t)(n - post_start);
    buf_u32(recipe, pre_len);
    buf_append(recipe, p, pre_len);
    buf_u32(recipe, idat_count);
    buf_append(recipe, idat_sizes.p, idat_sizes.n);
    buf_u8(recipe, (uint8_t)mode);
    buf_u32(recipe, (uint32_t)raw_n);
    buf_u32(recipe, (uint32_t)zlib_concat.n);
    if (mode == 1) {
        buf_append(recipe, zhdr, 2);
        buf_append(recipe, adler, 4);
        buf_u32(recipe, (uint32_t)preflate_diff.n);
        buf_append(recipe, preflate_diff.p, preflate_diff.n);
    }
    buf_u32(recipe, post_len);
    buf_append(recipe, p + post_start, post_len);

    /* Inflated IDAT bytes go to the solid stream. */
    buf_append(solid, raw, raw_n);

    fprintf(stderr, "    png: %u IDAT chunk(s), zlib=%zu raw=%zu mode=%d%s\n",
            idat_count, zlib_concat.n, raw_n, mode,
            mode == 1 ? " (preflate)" : " (l9)");

    free(raw);
    buf_free(&zlib_concat);
    buf_free(&idat_sizes);
    buf_free(&preflate_diff);
    return 0;
}

/* Reconstruct a PNG from recipe + solid bytes. */
static void unpack_png(const uint8_t *recipe, size_t rlen,
                       const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                       FILE *out, uint64_t expected_size) {
    size_t r = 0;
    if (r + 4 > rlen) die("png recipe truncated");
    uint32_t pre_len = r32(recipe + r); r += 4;
    if (r + pre_len > rlen) die("png pre overflow");
    const uint8_t *pre = recipe + r; r += pre_len;
    if (r + 4 > rlen) die("png idat_count truncated");
    uint32_t idat_count = r32(recipe + r); r += 4;
    if (idat_count == 0 || r + (size_t)idat_count * 4 > rlen) die("png idat sizes overflow");
    const uint8_t *idat_sizes = recipe + r; r += (size_t)idat_count * 4;
    if (r + 1 + 4 + 4 > rlen) die("png header truncated");
    uint8_t mode = recipe[r]; r += 1;
    uint32_t raw_len = r32(recipe + r); r += 4;
    uint32_t zlib_total = r32(recipe + r); r += 4;
    uint8_t zhdr[2] = {0,0}, adler[4] = {0,0,0,0};
    const uint8_t *diff = NULL; uint32_t diff_len = 0;
    if (mode == 1) {
        if (r + 2 + 4 + 4 > rlen) die("png mode1 header truncated");
        memcpy(zhdr, recipe + r, 2); r += 2;
        memcpy(adler, recipe + r, 4); r += 4;
        diff_len = r32(recipe + r); r += 4;
        if (r + diff_len > rlen) die("png diff overflow");
        diff = recipe + r; r += diff_len;
    }
    if (r + 4 > rlen) die("png post_len truncated");
    uint32_t post_len = r32(recipe + r); r += 4;
    if (r + post_len > rlen) die("png post overflow");
    const uint8_t *post = recipe + r; r += post_len;
    if (r != rlen) die("png recipe trailing bytes");

    if (*solid_pos + raw_len > solid_len) die("png solid overflow");
    const uint8_t *raw = solid + *solid_pos;

    /* Reconstruct zlib stream. */
    uint8_t *zlib_buf = NULL;
    size_t zlib_n = 0;
    if (mode == 0) {
        zlib_buf = zlib_deflate_l9(raw, raw_len, &zlib_n);
        if (!zlib_buf) die("png zlib_deflate_l9");
    } else {
        uint8_t *def = NULL; size_t def_n = 0;
        if (!zxle_preflate_join(raw, raw_len, diff, diff_len, &def, &def_n))
            die("png preflate_join");
        zlib_n = 2 + def_n + 4;
        zlib_buf = malloc(zlib_n);
        if (!zlib_buf) die("png malloc zlib");
        memcpy(zlib_buf, zhdr, 2);
        memcpy(zlib_buf + 2, def, def_n);
        memcpy(zlib_buf + 2 + def_n, adler, 4);
        zxle_preflate_free(def);
    }
    if (zlib_n != zlib_total) die("png zlib size mismatch");

    /* Verify split sizes sum to zlib_n. */
    size_t sum = 0;
    for (uint32_t i = 0; i < idat_count; i++) sum += r32(idat_sizes + i*4);
    if (sum != zlib_n) die("png idat sizes mismatch");

    /* Emit pre, IDAT chunks, post. */
    if (pre_len > 0 && fwrite(pre, 1, pre_len, out) != pre_len) die("fwrite png pre");
    size_t zoff = 0;
    for (uint32_t i = 0; i < idat_count; i++) {
        uint32_t clen = r32(idat_sizes + i*4);
        uint8_t hdr[8];
        w32be(hdr, clen);
        hdr[4] = 'I'; hdr[5] = 'D'; hdr[6] = 'A'; hdr[7] = 'T';
        if (fwrite(hdr, 1, 8, out) != 8) die("fwrite png idat hdr");
        if (clen > 0 && fwrite(zlib_buf + zoff, 1, clen, out) != clen) die("fwrite png idat data");
        uLong c = crc32(0L, Z_NULL, 0);
        c = crc32(c, hdr + 4, 4);
        if (clen > 0) c = crc32(c, zlib_buf + zoff, clen);
        uint8_t crcb[4]; w32be(crcb, (uint32_t)c);
        if (fwrite(crcb, 1, 4, out) != 4) die("fwrite png idat crc");
        zoff += clen;
    }
    if (post_len > 0 && fwrite(post, 1, post_len, out) != post_len) die("fwrite png post");

    free(zlib_buf);
    *solid_pos += raw_len;
    uint64_t written = (uint64_t)pre_len + (uint64_t)idat_count * 12 + zlib_n + post_len;
    if (written != expected_size) die("png size mismatch");
}

/* ---------- gzip helpers ---------- */

/* Raw-inflate src into a freshly allocated buffer of unknown size. */
static uint8_t *raw_inflate_dyn(const uint8_t *src, size_t src_n, size_t *out_n) {
    z_stream z = {0};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return NULL;
    size_t cap = src_n * 4 + 4096;
    uint8_t *out = malloc(cap);
    if (!out) { inflateEnd(&z); die("malloc raw_inflate_dyn"); }
    z.next_in = (Bytef *)src;
    z.avail_in = (uInt)src_n;
    z.next_out = out;
    z.avail_out = (uInt)cap;
    for (;;) {
        int rc = inflate(&z, Z_FINISH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR || rc == Z_OK) {
            size_t newcap = cap * 2;
            uint8_t *no = realloc(out, newcap);
            if (!no) { inflateEnd(&z); free(out); die("realloc raw_inflate_dyn"); }
            out = no;
            z.next_out = out + z.total_out;
            z.avail_out = (uInt)(newcap - z.total_out);
            cap = newcap;
            continue;
        }
        inflateEnd(&z); free(out); return NULL;
    }
    *out_n = z.total_out;
    inflateEnd(&z);
    return out;
}

/* Pack a top-level single-member gzip. Builds *recipe and appends inflated body
 * bytes to *solid. Returns 0 on success, -1 to fall back to KIND_OPAQUE. */
static int pack_gz(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid) {
    if (n < 18) return -1;
    if (p[0] != 0x1F || p[1] != 0x8B || p[2] != 0x08) return -1;
    uint8_t flg = p[3];
    if (flg & 0xE0) return -1; /* reserved bits set */
    size_t hdr = 10;
    if (flg & 0x04) {          /* FEXTRA */
        if (hdr + 2 > n) return -1;
        uint16_t xlen = (uint16_t)(p[hdr] | (p[hdr+1] << 8));
        hdr += 2 + xlen;
        if (hdr > n) return -1;
    }
    if (flg & 0x08) {          /* FNAME */
        while (hdr < n && p[hdr] != 0) hdr++;
        if (hdr >= n) return -1;
        hdr++;
    }
    if (flg & 0x10) {          /* FCOMMENT */
        while (hdr < n && p[hdr] != 0) hdr++;
        if (hdr >= n) return -1;
        hdr++;
    }
    if (flg & 0x02) {          /* FHCRC */
        if (hdr + 2 > n) return -1;
        hdr += 2;
    }
    if (hdr + 8 > n) return -1;
    size_t def_n = n - hdr - 8;
    const uint8_t *def = p + hdr;
    const uint8_t *trailer = p + n - 8;

    /* Inflate body. */
    size_t raw_n = 0;
    uint8_t *raw = raw_inflate_dyn(def, def_n, &raw_n);
    if (!raw) return -1;
    if (raw_n > 0xFFFFFFFFu) { free(raw); return -1; }

    /* Verify CRC32 + ISIZE against trailer. */
    uint32_t want_crc = r32(trailer);
    uint32_t want_isize = r32(trailer + 4);
    uLong c = crc32(0L, Z_NULL, 0);
    if (raw_n > 0) c = crc32(c, raw, (uInt)raw_n);
    if ((uint32_t)c != want_crc || (uint32_t)(raw_n & 0xFFFFFFFFu) != want_isize) {
        free(raw); return -1;
    }

    /* Try mode 0: raw-deflate L9 default-strategy must match. */
    int mode = -1;
    Buf preflate_diff; buf_init(&preflate_diff);

    size_t redef_n = 0;
    uint8_t *redef = raw_deflate_l9(raw, (uint32_t)raw_n, &redef_n);
    if (redef && redef_n == def_n && memcmp(redef, def, def_n) == 0) {
        mode = 0;
    }
    free(redef);

    /* Mode 1: preflate over the raw deflate body. */
    if (mode < 0) {
        uint8_t *unp = NULL, *diff = NULL, *rejoin = NULL;
        size_t unp_n = 0, diff_n = 0, rejoin_n = 0;
        if (zxle_preflate_split(def, def_n, &unp, &unp_n, &diff, &diff_n)) {
            if (unp_n == raw_n && memcmp(unp, raw, raw_n) == 0 &&
                zxle_preflate_join(unp, unp_n, diff, diff_n, &rejoin, &rejoin_n) &&
                rejoin_n == def_n &&
                memcmp(rejoin, def, def_n) == 0) {
                buf_append(&preflate_diff, diff, diff_n);
                mode = 1;
            }
        }
        zxle_preflate_free(unp);
        zxle_preflate_free(diff);
        zxle_preflate_free(rejoin);
    }

    if (mode < 0) {
        free(raw); buf_free(&preflate_diff); return -1;
    }

    /* M3e-targz: if the inflated body is a ustar tar, route per-entry payloads
     * through pack_tar instead of dumping the whole thing opaque to solid. Use
     * scratch buffers so a mid-stream pack_tar failure can't contaminate solid. */
    int inner_kind = 0;
    Buf tar_recipe; buf_init(&tar_recipe);
    Buf tar_solid;  buf_init(&tar_solid);
    if (raw_n >= 1024 && raw_n % 512 == 0 &&
        memcmp(raw + 257, "ustar", 5) == 0) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.gztar", tmp_prefix);
        if (pack_tar(raw, raw_n, tp, &tar_recipe, &tar_solid) == 0) {
            inner_kind = 1;
        } else {
            buf_free(&tar_recipe); buf_init(&tar_recipe);
            buf_free(&tar_solid);  buf_init(&tar_solid);
        }
    }

    /* Build recipe. */
    buf_u32(recipe, (uint32_t)hdr);
    buf_append(recipe, p, hdr);
    buf_u8(recipe, (uint8_t)mode);
    buf_u32(recipe, (uint32_t)raw_n);
    buf_u32(recipe, (uint32_t)def_n);
    if (mode == 1) {
        buf_u32(recipe, (uint32_t)preflate_diff.n);
        buf_append(recipe, preflate_diff.p, preflate_diff.n);
    }
    buf_append(recipe, trailer, 8);
    buf_u8(recipe, (uint8_t)inner_kind);
    if (inner_kind == 1) {
        buf_u32(recipe, (uint32_t)tar_recipe.n);
        buf_append(recipe, tar_recipe.p, tar_recipe.n);
        buf_append(solid, tar_solid.p, tar_solid.n);
    } else {
        buf_append(solid, raw, raw_n);
    }

    fprintf(stderr, "    gz: hdr=%zu def=%zu raw=%zu mode=%d%s%s\n",
            hdr, def_n, raw_n, mode,
            mode == 1 ? " (preflate)" : " (l9)",
            inner_kind == 1 ? " inner=tar" : "");

    free(raw);
    buf_free(&preflate_diff);
    buf_free(&tar_recipe);
    buf_free(&tar_solid);
    return 0;
}

/* Reconstruct a gzip from recipe + solid bytes. */
static void unpack_gz(const uint8_t *recipe, size_t rlen,
                      const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                      FILE *out, uint64_t expected_size, const char *tmp_prefix) {
    size_t r = 0;
    if (r + 4 > rlen) die("gz recipe truncated");
    uint32_t hdr_len = r32(recipe + r); r += 4;
    if (r + hdr_len > rlen) die("gz hdr overflow");
    const uint8_t *hdr = recipe + r; r += hdr_len;
    if (r + 1 + 4 + 4 > rlen) die("gz header truncated");
    uint8_t mode = recipe[r]; r += 1;
    uint32_t raw_len = r32(recipe + r); r += 4;
    uint32_t def_len = r32(recipe + r); r += 4;
    const uint8_t *diff = NULL; uint32_t diff_len = 0;
    if (mode == 1) {
        if (r + 4 > rlen) die("gz diff len truncated");
        diff_len = r32(recipe + r); r += 4;
        if (r + diff_len > rlen) die("gz diff overflow");
        diff = recipe + r; r += diff_len;
    }
    if (r + 8 > rlen) die("gz trailer truncated");
    const uint8_t *trailer = recipe + r; r += 8;
    if (r + 1 > rlen) die("gz inner_kind truncated");
    uint8_t inner_kind = recipe[r]; r += 1;
    const uint8_t *tar_recipe = NULL; uint32_t tar_recipe_len = 0;
    if (inner_kind == 1) {
        if (r + 4 > rlen) die("gz tar recipe len truncated");
        tar_recipe_len = r32(recipe + r); r += 4;
        if (r + tar_recipe_len > rlen) die("gz tar recipe overflow");
        tar_recipe = recipe + r; r += tar_recipe_len;
    }
    if (r != rlen) die("gz recipe trailing bytes");

    /* Materialize the inflated body. inner_kind=0: raw bytes are in solid.
     * inner_kind=1: reconstruct via the nested tar recipe (which itself
     * consumes from solid). */
    uint8_t *raw_buf = NULL;
    const uint8_t *raw = NULL;
    if (inner_kind == 0) {
        if (*solid_pos + raw_len > solid_len) die("gz solid overflow");
        raw = solid + *solid_pos;
        *solid_pos += raw_len;
    } else {
        char tp[2048];
        snprintf(tp, sizeof(tp), "%s.gztar.tmp", tmp_prefix);
        FILE *tf = fopen(tp, "wb");
        if (!tf) die("fopen gz tar tmp");
        unpack_recipe(tar_recipe, tar_recipe_len,
                      solid, solid_len, solid_pos,
                      tf, raw_len, tp);
        fclose(tf);
        size_t got_n = 0;
        raw_buf = read_whole_file(tp, &got_n);
        unlink(tp);
        if (got_n != raw_len) die("gz tar size mismatch");
        raw = raw_buf;
    }

    uint8_t *def_buf = NULL; size_t def_n = 0;
    if (mode == 0) {
        def_buf = raw_deflate_l9(raw, raw_len, &def_n);
        if (!def_buf) die("gz raw_deflate_l9");
    } else {
        if (!zxle_preflate_join(raw, raw_len, diff, diff_len, &def_buf, &def_n))
            die("gz preflate_join");
    }
    if (def_n != def_len) die("gz def size mismatch");

    if (hdr_len > 0 && fwrite(hdr, 1, hdr_len, out) != hdr_len) die("fwrite gz hdr");
    if (def_n > 0 && fwrite(def_buf, 1, def_n, out) != def_n) die("fwrite gz body");
    if (fwrite(trailer, 1, 8, out) != 8) die("fwrite gz trailer");

    if (mode == 0) free(def_buf); else zxle_preflate_free(def_buf);
    free(raw_buf);
    uint64_t written = (uint64_t)hdr_len + def_n + 8;
    if (written != expected_size) die("gz size mismatch");
}

/* ---------- bzip2 (kind=8) ----------
 *
 * Pack: shell out to `bzip2 -dc` to inflate, verify round-trip by re-running
 * `bzip2 -<n>` on the inflated bytes and cmp'ing byte-for-byte against the
 * original. Same M3e-targz inner-tar trick: if the inflated body is a ustar
 * tar, route through pack_tar instead of dumping opaque to solid.
 *
 * No preflate equivalent for bzip2 — reproducibility relies on the system
 * bzip2 binary being deterministic for the chosen block size. Any mismatch
 * fails out and the caller falls back to KIND_OPAQUE.
 */
static int pack_bz2(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid) {
    if (n < 14) return -1;
    if (p[0] != 'B' || p[1] != 'Z' || p[2] != 'h') return -1;
    if (p[3] < '1' || p[3] > '9') return -1;
    if (n > 0xFFFFFFFFu) return -1;
    uint8_t block_size = p[3];

    char in_bz2[1024], raw_path[1024], rt_bz2[1024], cmd[4096];
    snprintf(in_bz2,   sizeof(in_bz2),   "%s.in.bz2",  tmp_prefix);
    snprintf(raw_path, sizeof(raw_path), "%s.raw.bin", tmp_prefix);
    snprintf(rt_bz2,   sizeof(rt_bz2),   "%s.rt.bz2",  tmp_prefix);

    FILE *bf = fopen(in_bz2, "wb");
    if (!bf) return -1;
    if (n > 0 && fwrite(p, 1, n, bf) != n) { fclose(bf); unlink(in_bz2); return -1; }
    fclose(bf);

    snprintf(cmd, sizeof(cmd), "bzip2 -dc \"%s\" > \"%s\" 2>%s",
             in_bz2, raw_path, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(in_bz2); unlink(raw_path); return -1; }

    size_t raw_n = 0;
    uint8_t *raw = read_whole_file(raw_path, &raw_n);
    if (!raw || raw_n > 0xFFFFFFFFu) {
        free(raw); unlink(in_bz2); unlink(raw_path); return -1;
    }

    /* Re-bzip2 with the same block size; cmp byte-for-byte. */
    snprintf(cmd, sizeof(cmd), "bzip2 -%c -c \"%s\" > \"%s\" 2>%s",
             (char)block_size, raw_path, rt_bz2, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) {
        free(raw); unlink(in_bz2); unlink(raw_path); unlink(rt_bz2); return -1;
    }
    size_t rt_n = 0;
    uint8_t *rt = read_whole_file(rt_bz2, &rt_n);
    int ok = (rt && rt_n == n && memcmp(rt, p, n) == 0);
    free(rt);
    unlink(in_bz2); unlink(rt_bz2);
    if (!ok) { free(raw); unlink(raw_path); return -1; }

    /* Inner-kind dispatch: ustar tar -> pack_tar; otherwise dump raw to solid. */
    int inner_kind = 0;
    Buf tar_recipe; buf_init(&tar_recipe);
    Buf tar_solid;  buf_init(&tar_solid);
    if (raw_n >= 1024 && raw_n % 512 == 0 &&
        memcmp(raw + 257, "ustar", 5) == 0) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.bz2tar", tmp_prefix);
        if (pack_tar(raw, raw_n, tp, &tar_recipe, &tar_solid) == 0) {
            inner_kind = 1;
        } else {
            buf_free(&tar_recipe); buf_init(&tar_recipe);
            buf_free(&tar_solid);  buf_init(&tar_solid);
        }
    }

    /* Build recipe. */
    buf_u8(recipe, block_size);
    buf_u32(recipe, (uint32_t)raw_n);
    buf_u32(recipe, (uint32_t)n);
    buf_u8(recipe, (uint8_t)inner_kind);
    if (inner_kind == 1) {
        buf_u32(recipe, (uint32_t)tar_recipe.n);
        buf_append(recipe, tar_recipe.p, tar_recipe.n);
        buf_append(solid, tar_solid.p, tar_solid.n);
    } else {
        buf_append(solid, raw, raw_n);
    }

    fprintf(stderr, "    bz2: block=%c orig=%zu raw=%zu%s\n",
            (char)block_size, n, raw_n,
            inner_kind == 1 ? " inner=tar" : "");

    free(raw);
    unlink(raw_path);
    buf_free(&tar_recipe);
    buf_free(&tar_solid);
    return 0;
}

static void unpack_bz2(const uint8_t *recipe, size_t rlen,
                       const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                       FILE *out, uint64_t expected_size, const char *tmp_prefix) {
    size_t r = 0;
    if (r + 1 + 4 + 4 + 1 > rlen) die("bz2 recipe truncated");
    uint8_t block_size = recipe[r]; r += 1;
    uint32_t raw_len   = r32(recipe + r); r += 4;
    uint32_t orig_len  = r32(recipe + r); r += 4;
    uint8_t inner_kind = recipe[r]; r += 1;
    const uint8_t *tar_recipe = NULL; uint32_t tar_recipe_len = 0;
    if (inner_kind == 1) {
        if (r + 4 > rlen) die("bz2 tar recipe len truncated");
        tar_recipe_len = r32(recipe + r); r += 4;
        if (r + tar_recipe_len > rlen) die("bz2 tar recipe overflow");
        tar_recipe = recipe + r; r += tar_recipe_len;
    }
    if (r != rlen) die("bz2 recipe trailing bytes");
    if (block_size < '1' || block_size > '9') die("bz2 block size");

    char raw_path[2048], rt_bz2[2048], cmd[4096];
    snprintf(raw_path, sizeof(raw_path), "%s.bz2.raw.tmp", tmp_prefix);
    snprintf(rt_bz2,   sizeof(rt_bz2),   "%s.bz2.rt.tmp",  tmp_prefix);

    /* Materialize raw bytes to a temp file. */
    FILE *rf = fopen(raw_path, "wb");
    if (!rf) die("fopen bz2 raw tmp");
    if (inner_kind == 0) {
        if (*solid_pos + raw_len > solid_len) die("bz2 solid overflow");
        if (raw_len > 0 && fwrite(solid + *solid_pos, 1, raw_len, rf) != raw_len)
            die("fwrite bz2 raw");
        *solid_pos += raw_len;
        fclose(rf);
    } else {
        unpack_recipe(tar_recipe, tar_recipe_len,
                      solid, solid_len, solid_pos,
                      rf, raw_len, raw_path);
        fclose(rf);
    }

    snprintf(cmd, sizeof(cmd), "bzip2 -%c -c \"%s\" > \"%s\" 2>%s",
             (char)block_size, raw_path, rt_bz2, ZXLE_DEVNULL);
    run(cmd);
    unlink(raw_path);

    size_t got_n = 0;
    uint8_t *got = read_whole_file(rt_bz2, &got_n);
    unlink(rt_bz2);
    if (!got || got_n != orig_len) die("bz2 size mismatch");
    if (got_n != expected_size) die("bz2 expected size mismatch");
    if (got_n > 0 && fwrite(got, 1, got_n, out) != got_n) die("fwrite bz2 out");
    free(got);
}

/* ---------- zstd (kind=9) ----------
 *
 * Pack: shell out to `zstd -d` to inflate, then probe a `(level, --long, io)`
 * ladder to find a re-encode that byte-matches the input. The probe ladder is
 * driven by the input's frame header (RFC 8478 §3.1.1.1):
 *   - byte 4 = Frame_Header_Descriptor:
 *       FCS_flag (bits 6-7), Single_Segment (bit 5),
 *       Content_Checksum_flag (bit 2), Dictionary_ID_flag (bits 0-1).
 *   - byte 5 (when !Single_Segment) = Window_Descriptor: window_log = 10 + exp.
 *
 * Observed FCS presence pins io mode: file-mode writes FCS, stdin-mode does
 * not. Observed checksum bit pins --check / --no-check. Dictionary frames are
 * not supported (return -1 → KIND_OPAQUE).
 *
 * For each level in the ladder we try --long values in this order: 27 (matches
 * our internal output), the observed window_log (from the header), then no
 * --long. --long=N is *not* idempotent across N even when the encoded window
 * matches (verified empirically against makepkg output), so the observed value
 * has to be one of the probes.
 */
static int pack_zst(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid) {
    if (n < 8) return -1;
    if (p[0] != 0x28 || p[1] != 0xB5 || p[2] != 0x2F || p[3] != 0xFD) return -1;
    if (n > 0xFFFFFFFFu) return -1;

    /* Parse Frame_Header_Descriptor + Window_Descriptor. */
    uint8_t fhd            = p[4];
    int fcs_flag           = (fhd >> 6) & 0x3;
    int single_segment     = (fhd >> 5) & 0x1;
    int has_checksum       = (fhd >> 2) & 0x1;
    int dict_id_flag       = fhd & 0x3;
    if (dict_id_flag != 0) return -1;        /* dictionary frames unsupported */
    int window_log = 0;                       /* 0 means "no window descriptor present" */
    if (!single_segment) {
        if (n < 6) return -1;
        window_log = 10 + ((p[5] >> 3) & 0x1F);
        if (window_log < 10 || window_log > 31) return -1;
    }
    /* Single_Segment frames always carry FCS (1 byte minimum). */
    int has_fcs = (fcs_flag != 0) || single_segment;

    char in_zst[1024], raw_path[1024], rt_zst[1024], cmd[4096];
    snprintf(in_zst,   sizeof(in_zst),   "%s.in.zst",  tmp_prefix);
    snprintf(raw_path, sizeof(raw_path), "%s.raw.bin", tmp_prefix);
    snprintf(rt_zst,   sizeof(rt_zst),   "%s.rt.zst",  tmp_prefix);

    FILE *zf = fopen(in_zst, "wb");
    if (!zf) return -1;
    if (n > 0 && fwrite(p, 1, n, zf) != n) { fclose(zf); unlink(in_zst); return -1; }
    fclose(zf);

    snprintf(cmd, sizeof(cmd), "zstd -d -q -f -o \"%s\" \"%s\" >%s 2>&1",
             raw_path, in_zst, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(in_zst); unlink(raw_path); return -1; }

    size_t raw_n = 0;
    uint8_t *raw = read_whole_file(raw_path, &raw_n);
    if (!raw || raw_n > 0xFFFFFFFFu) {
        free(raw); unlink(in_zst); unlink(raw_path); return -1;
    }

    /* Levels ordered: 19 (our internal), then 22/20/21 (common high-effort /
     * Arch makepkg), then descending to 1. */
    static const uint8_t levels[] = {19, 22, 20, 21, 18, 17, 9, 6, 3, 1};
    /* For each level, try long args in order: 27 (internal), observed
     * window_log, none. Skip duplicates. */
    uint8_t long_tries[3];
    int n_long = 0;
    long_tries[n_long++] = 27;
    if (window_log != 0 && window_log != 27) long_tries[n_long++] = (uint8_t)window_log;
    long_tries[n_long++] = 0;  /* sentinel: no --long */

    const char *check_arg = has_checksum ? "--check" : "--no-check";

    int matched_level = -1;
    uint8_t matched_long = 0;
    for (size_t li = 0; li < sizeof(levels); li++) {
        uint8_t level = levels[li];
        for (int lj = 0; lj < n_long; lj++) {
            uint8_t lw = long_tries[lj];
            char long_part[32];
            if (lw == 0) long_part[0] = 0;
            else snprintf(long_part, sizeof(long_part), " --long=%u", lw);

            if (has_fcs) {
                snprintf(cmd, sizeof(cmd),
                         "zstd -%u%s %s -q -f -o \"%s\" \"%s\" >%s 2>&1",
                         level, long_part, check_arg, rt_zst, raw_path,
                         ZXLE_DEVNULL);
            } else {
                snprintf(cmd, sizeof(cmd),
                         "zstd -%u%s %s -q < \"%s\" > \"%s\" 2>%s",
                         level, long_part, check_arg, raw_path, rt_zst,
                         ZXLE_DEVNULL);
            }
            if (try_run(cmd) != 0) { unlink(rt_zst); continue; }
            size_t rt_n = 0;
            uint8_t *rt = read_whole_file(rt_zst, &rt_n);
            int ok = (rt && rt_n == n && memcmp(rt, p, n) == 0);
            free(rt);
            unlink(rt_zst);
            if (ok) { matched_level = level; matched_long = lw; break; }
        }
        if (matched_level >= 0) break;
    }
    unlink(in_zst);
    if (matched_level < 0) { free(raw); unlink(raw_path); return -1; }
    uint8_t level  = (uint8_t)matched_level;
    uint8_t window = matched_long;  /* 0 means "no --long"; else value used */
    uint8_t flags  = 0;
    if (!has_fcs)      flags |= 0x01;  /* use stdin (suppress FCS) */
    if (!has_checksum) flags |= 0x02;  /* --no-check */

    int inner_kind = 0;
    Buf tar_recipe; buf_init(&tar_recipe);
    Buf tar_solid;  buf_init(&tar_solid);
    if (raw_n >= 1024 && raw_n % 512 == 0 &&
        memcmp(raw + 257, "ustar", 5) == 0) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.zsttar", tmp_prefix);
        if (pack_tar(raw, raw_n, tp, &tar_recipe, &tar_solid) == 0) {
            inner_kind = 1;
        } else {
            buf_free(&tar_recipe); buf_init(&tar_recipe);
            buf_free(&tar_solid);  buf_init(&tar_solid);
        }
    }

    buf_u8(recipe, level);
    buf_u8(recipe, window);
    buf_u8(recipe, flags);
    buf_u32(recipe, (uint32_t)raw_n);
    buf_u32(recipe, (uint32_t)n);
    buf_u8(recipe, (uint8_t)inner_kind);
    if (inner_kind == 1) {
        buf_u32(recipe, (uint32_t)tar_recipe.n);
        buf_append(recipe, tar_recipe.p, tar_recipe.n);
        buf_append(solid, tar_solid.p, tar_solid.n);
    } else {
        buf_append(solid, raw, raw_n);
    }

    fprintf(stderr, "    zst: orig=%zu raw=%zu level=%u long=%u io=%s check=%s%s\n",
            n, raw_n, level, window,
            (flags & 0x01) ? "stdin" : "file",
            (flags & 0x02) ? "off" : "on",
            inner_kind == 1 ? " inner=tar" : "");

    free(raw);
    unlink(raw_path);
    buf_free(&tar_recipe);
    buf_free(&tar_solid);
    return 0;
}

static void unpack_zst(const uint8_t *recipe, size_t rlen,
                       const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                       FILE *out, uint64_t expected_size, const char *tmp_prefix) {
    size_t r = 0;
    if (r + 1 + 1 + 1 + 4 + 4 + 1 > rlen) die("zst recipe truncated");
    uint8_t level      = recipe[r]; r += 1;
    uint8_t window     = recipe[r]; r += 1;
    uint8_t flags      = recipe[r]; r += 1;
    uint32_t raw_len   = r32(recipe + r); r += 4;
    uint32_t orig_len  = r32(recipe + r); r += 4;
    uint8_t inner_kind = recipe[r]; r += 1;
    if (level < 1 || level > 22) die("zst level out of range");
    const uint8_t *tar_recipe = NULL; uint32_t tar_recipe_len = 0;
    if (inner_kind == 1) {
        if (r + 4 > rlen) die("zst tar recipe len truncated");
        tar_recipe_len = r32(recipe + r); r += 4;
        if (r + tar_recipe_len > rlen) die("zst tar recipe overflow");
        tar_recipe = recipe + r; r += tar_recipe_len;
    }
    if (r != rlen) die("zst recipe trailing bytes");

    char raw_path[2048], rt_zst[2048], cmd[4096];
    snprintf(raw_path, sizeof(raw_path), "%s.zst.raw.tmp", tmp_prefix);
    snprintf(rt_zst,   sizeof(rt_zst),   "%s.zst.rt.tmp",  tmp_prefix);

    FILE *rf = fopen(raw_path, "wb");
    if (!rf) die("fopen zst raw tmp");
    if (inner_kind == 0) {
        if (*solid_pos + raw_len > solid_len) die("zst solid overflow");
        if (raw_len > 0 && fwrite(solid + *solid_pos, 1, raw_len, rf) != raw_len)
            die("fwrite zst raw");
        *solid_pos += raw_len;
        fclose(rf);
    } else {
        unpack_recipe(tar_recipe, tar_recipe_len,
                      solid, solid_len, solid_pos,
                      rf, raw_len, raw_path);
        fclose(rf);
    }

    char long_part[32];
    if (window == 0) long_part[0] = 0;
    else snprintf(long_part, sizeof(long_part), " --long=%u", window);
    const char *check_arg = (flags & 0x02) ? "--no-check" : "--check";
    if (flags & 0x01) {
        snprintf(cmd, sizeof(cmd),
                 "zstd -%u%s %s -q < \"%s\" > \"%s\" 2>%s",
                 level, long_part, check_arg, raw_path, rt_zst, ZXLE_DEVNULL);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "zstd -%u%s %s -q -f -o \"%s\" \"%s\" >%s 2>&1",
                 level, long_part, check_arg, rt_zst, raw_path, ZXLE_DEVNULL);
    }
    run(cmd);
    unlink(raw_path);

    size_t got_n = 0;
    uint8_t *got = read_whole_file(rt_zst, &got_n);
    unlink(rt_zst);
    if (!got || got_n != orig_len) die("zst size mismatch");
    if (got_n != expected_size) die("zst expected size mismatch");
    if (got_n > 0 && fwrite(got, 1, got_n, out) != got_n) die("fwrite zst out");
    free(got);
}

/* ---------- tar (POSIX/ustar) ---------- */

/* Pack a top-level ustar tar. Builds *recipe (using the OP_* vocabulary shared
 * with KIND_ZIP) and appends raw payload bytes of regular file entries to
 * *solid. Returns 0 on success, -1 to fall back to KIND_OPAQUE.
 *
 * Scope: ustar / GNU "ustar " magic. Regular files (typeflag '0' or '\\0').
 * Other types (dir/link/longname/etc.) keep their header in OP_STRUCT and
 * have no payload; if such an entry has a non-zero size, we conservatively
 * STORE it. GNU base-256 size encoding (high bit of size[0] set) is rejected. */
static int pack_tar(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid) {
    if (n < 1024) return -1;
    if (n % 512 != 0) return -1;
    if (memcmp(p + 257, "ustar", 5) != 0) return -1;

    size_t cur = 0;
    int regulars = 0, jpeg_stored = 0, png_stored = 0, gzip_stored = 0, bz2_stored = 0, stored_plain = 0;

    while (cur + 512 <= n) {
        const uint8_t *hdr = p + cur;

        /* Detect end-of-archive zero block(s). Two consecutive zero blocks
         * mark the end; emit everything from cur to n verbatim. */
        int is_zero = 1;
        for (int i = 0; i < 512; i++) if (hdr[i]) { is_zero = 0; break; }
        if (is_zero) {
            buf_u8(recipe, OP_STRUCT);
            buf_u32(recipe, (uint32_t)(n - cur));
            buf_append(recipe, p + cur, n - cur);
            cur = n;
            break;
        }

        if (memcmp(hdr + 257, "ustar", 5) != 0) return -1;
        if (hdr[124] & 0x80) return -1; /* GNU base-256 size */

        uint64_t size = 0;
        for (int i = 124; i < 124 + 11; i++) {
            uint8_t c = hdr[i];
            if (c == 0 || c == ' ') break;
            if (c < '0' || c > '7') return -1;
            size = size * 8 + (c - '0');
        }
        if (size > 0xFFFFFFFFu) return -1;

        char tflag = (char)hdr[156];
        uint64_t padded = (size + 511) & ~(uint64_t)511;
        if (cur + 512 + padded > n) return -1;

        /* Header verbatim. */
        buf_u8(recipe, OP_STRUCT);
        buf_u32(recipe, 512);
        buf_append(recipe, hdr, 512);
        cur += 512;

        if (size > 0) {
            int is_regular = (tflag == '0' || tflag == 0);
            int handled = 0;

            if (is_regular && size >= 8 && memcmp(p + cur, PNG_SIG, 8) == 0) {
                Buf png_recipe; buf_init(&png_recipe);
                if (pack_png(p + cur, (size_t)size, &png_recipe, solid) == 0) {
                    buf_u8(recipe, OP_PNG_STORE);
                    buf_u32(recipe, (uint32_t)size);
                    buf_u32(recipe, (uint32_t)png_recipe.n);
                    buf_append(recipe, png_recipe.p, png_recipe.n);
                    png_stored++;
                    handled = 1;
                }
                buf_free(&png_recipe);
            }
            if (!handled && is_regular && size >= 4 &&
                p[cur] == 0xFF && p[cur+1] == 0xD8 && p[cur+2] == 0xFF) {
                char tp[2048];
                snprintf(tp, sizeof(tp), "%s.tj.%zu", tmp_prefix, cur);
                Buf brn; buf_init(&brn);
                if (try_brunsli_buf(p + cur, (size_t)size, tp, &brn) == 0) {
                    buf_u8(recipe, OP_JPEG_STORE);
                    buf_u32(recipe, (uint32_t)size);
                    buf_u32(recipe, (uint32_t)brn.n);
                    buf_append(recipe, brn.p, brn.n);
                    jpeg_stored++;
                    handled = 1;
                }
                buf_free(&brn);
            }
            if (!handled && is_regular && size >= 18 &&
                p[cur] == 0x1F && p[cur+1] == 0x8B && p[cur+2] == 0x08) {
                char tp[1024];
                snprintf(tp, sizeof(tp), "%s.tgz.%zu", tmp_prefix, cur);
                Buf gz_recipe; buf_init(&gz_recipe);
                Buf gz_solid;  buf_init(&gz_solid);
                if (pack_gz(p + cur, (size_t)size, tp, &gz_recipe, &gz_solid) == 0) {
                    buf_u8(recipe, OP_GZIP_STORE);
                    buf_u32(recipe, (uint32_t)size);
                    buf_u32(recipe, (uint32_t)gz_recipe.n);
                    buf_append(recipe, gz_recipe.p, gz_recipe.n);
                    buf_append(solid, gz_solid.p, gz_solid.n);
                    gzip_stored++;
                    handled = 1;
                }
                buf_free(&gz_recipe);
                buf_free(&gz_solid);
            }
            if (!handled && is_regular && size >= 14 &&
                p[cur] == 'B' && p[cur+1] == 'Z' && p[cur+2] == 'h' &&
                p[cur+3] >= '1' && p[cur+3] <= '9') {
                char tp[1024];
                snprintf(tp, sizeof(tp), "%s.tbz.%zu", tmp_prefix, cur);
                Buf bz_recipe; buf_init(&bz_recipe);
                Buf bz_solid;  buf_init(&bz_solid);
                if (pack_bz2(p + cur, (size_t)size, tp, &bz_recipe, &bz_solid) == 0) {
                    buf_u8(recipe, OP_BZ2_STORE);
                    buf_u32(recipe, (uint32_t)size);
                    buf_u32(recipe, (uint32_t)bz_recipe.n);
                    buf_append(recipe, bz_recipe.p, bz_recipe.n);
                    buf_append(solid, bz_solid.p, bz_solid.n);
                    bz2_stored++;
                    handled = 1;
                }
                buf_free(&bz_recipe);
                buf_free(&bz_solid);
            }
            if (!handled) {
                buf_u8(recipe, OP_STORE);
                buf_u32(recipe, (uint32_t)size);
                buf_append(solid, p + cur, (size_t)size);
                stored_plain++;
            }
            cur += size;

            uint64_t pad = padded - size;
            if (pad > 0) {
                buf_u8(recipe, OP_STRUCT);
                buf_u32(recipe, (uint32_t)pad);
                buf_append(recipe, p + cur, (size_t)pad);
                cur += pad;
            }
            if (is_regular) regulars++;
        }
    }

    if (cur < n) {
        buf_u8(recipe, OP_STRUCT);
        buf_u32(recipe, (uint32_t)(n - cur));
        buf_append(recipe, p + cur, n - cur);
    }

    fprintf(stderr, "    tar: %d regular (%d store, %d jpeg-store, %d png-store, %d gzip-store, %d bz2-store)\n",
            regulars, stored_plain, jpeg_stored, png_stored, gzip_stored, bz2_stored);
    return 0;
}

/* ---------- ar (Unix archive: .a, .deb) ---------- */

/* Pack a top-level AR archive. 8-byte magic "!<arch>\n" then per-entry 60-byte
 * headers (16-byte name + 12-byte mtime + 6-byte uid + 6-byte gid + 8-byte mode
 * + 10-byte ASCII-decimal size + 2-byte 0x60 0x0A magic). Entries are 2-byte
 * aligned: a single 0x0A pad byte follows odd-sized payloads.
 *
 * Same OP_* vocabulary as KIND_TAR. Per-entry payloads route via OP_GZIP_STORE
 * (gzip files inside the archive — typical for .deb's data.tar.gz / control.tar.gz),
 * OP_PNG_STORE, OP_JPEG_STORE, else OP_STORE. Headers and pad bytes go in
 * OP_STRUCT verbatim.
 *
 * BSD vs GNU long-name variants don't need to be interpreted: we treat the
 * special "//" / "#1/N" entries as opaque payloads (just route them through
 * the same payload-dispatch chain as anything else; they typically don't
 * match gzip/png/jpeg magic so they fall through to OP_STORE). */
static int pack_ar(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *recipe, Buf *solid) {
    if (n < 8) return -1;
    if (memcmp(p, "!<arch>\n", 8) != 0) return -1;

    buf_u8(recipe, OP_STRUCT);
    buf_u32(recipe, 8);
    buf_append(recipe, p, 8);

    size_t cur = 8;
    int entries = 0, gzip_stored = 0, bz2_stored = 0, png_stored = 0, jpeg_stored = 0, stored_plain = 0;

    while (cur < n) {
        if (cur + 60 > n) return -1;
        const uint8_t *hdr = p + cur;
        if (hdr[58] != 0x60 || hdr[59] != 0x0A) return -1;

        uint64_t size = 0;
        for (int i = 0; i < 10; i++) {
            uint8_t c = hdr[48 + i];
            if (c == ' ' || c == 0) break;
            if (c < '0' || c > '9') return -1;
            size = size * 10 + (c - '0');
        }
        if (size > 0xFFFFFFFFu) return -1;
        if (cur + 60 + size > n) return -1;

        buf_u8(recipe, OP_STRUCT);
        buf_u32(recipe, 60);
        buf_append(recipe, hdr, 60);
        cur += 60;

        if (size > 0) {
            const uint8_t *body = p + cur;
            int handled = 0;

            if (size >= 18 && body[0] == 0x1F && body[1] == 0x8B && body[2] == 0x08) {
                char tp[1024];
                snprintf(tp, sizeof(tp), "%s.argz.%zu", tmp_prefix, cur);
                Buf gz_recipe; buf_init(&gz_recipe);
                Buf gz_solid;  buf_init(&gz_solid);
                if (pack_gz(body, (size_t)size, tp, &gz_recipe, &gz_solid) == 0) {
                    buf_u8(recipe, OP_GZIP_STORE);
                    buf_u32(recipe, (uint32_t)size);
                    buf_u32(recipe, (uint32_t)gz_recipe.n);
                    buf_append(recipe, gz_recipe.p, gz_recipe.n);
                    buf_append(solid, gz_solid.p, gz_solid.n);
                    gzip_stored++;
                    handled = 1;
                }
                buf_free(&gz_recipe);
                buf_free(&gz_solid);
            }
            if (!handled && size >= 8 && memcmp(body, PNG_SIG, 8) == 0) {
                Buf png_recipe; buf_init(&png_recipe);
                if (pack_png(body, (size_t)size, &png_recipe, solid) == 0) {
                    buf_u8(recipe, OP_PNG_STORE);
                    buf_u32(recipe, (uint32_t)size);
                    buf_u32(recipe, (uint32_t)png_recipe.n);
                    buf_append(recipe, png_recipe.p, png_recipe.n);
                    png_stored++;
                    handled = 1;
                }
                buf_free(&png_recipe);
            }
            if (!handled && size >= 4 &&
                body[0] == 0xFF && body[1] == 0xD8 && body[2] == 0xFF) {
                char tp[2048];
                snprintf(tp, sizeof(tp), "%s.arj.%zu", tmp_prefix, cur);
                Buf brn; buf_init(&brn);
                if (try_brunsli_buf(body, (size_t)size, tp, &brn) == 0) {
                    buf_u8(recipe, OP_JPEG_STORE);
                    buf_u32(recipe, (uint32_t)size);
                    buf_u32(recipe, (uint32_t)brn.n);
                    buf_append(recipe, brn.p, brn.n);
                    jpeg_stored++;
                    handled = 1;
                }
                buf_free(&brn);
            }
            if (!handled && size >= 14 &&
                body[0] == 'B' && body[1] == 'Z' && body[2] == 'h' &&
                body[3] >= '1' && body[3] <= '9') {
                char tp[1024];
                snprintf(tp, sizeof(tp), "%s.arbz.%zu", tmp_prefix, cur);
                Buf bz_recipe; buf_init(&bz_recipe);
                Buf bz_solid;  buf_init(&bz_solid);
                if (pack_bz2(body, (size_t)size, tp, &bz_recipe, &bz_solid) == 0) {
                    buf_u8(recipe, OP_BZ2_STORE);
                    buf_u32(recipe, (uint32_t)size);
                    buf_u32(recipe, (uint32_t)bz_recipe.n);
                    buf_append(recipe, bz_recipe.p, bz_recipe.n);
                    buf_append(solid, bz_solid.p, bz_solid.n);
                    bz2_stored++;
                    handled = 1;
                }
                buf_free(&bz_recipe);
                buf_free(&bz_solid);
            }
            if (!handled) {
                buf_u8(recipe, OP_STORE);
                buf_u32(recipe, (uint32_t)size);
                buf_append(solid, body, (size_t)size);
                stored_plain++;
            }
            cur += size;
        }

        /* 2-byte alignment: a single 0x0A pad byte after an odd-sized entry. */
        if (cur < n && (cur & 1) == 1) {
            if (p[cur] != 0x0A) return -1;
            buf_u8(recipe, OP_STRUCT);
            buf_u32(recipe, 1);
            buf_append(recipe, p + cur, 1);
            cur += 1;
        }
        entries++;
    }

    fprintf(stderr, "    ar: %d entries (%d store, %d gzip-store, %d bz2-store, %d png-store, %d jpeg-store)\n",
            entries, stored_plain, gzip_stored, bz2_stored, png_stored, jpeg_stored);
    return 0;
}

/* ---------- pack/unpack drivers ---------- */

typedef struct {
    const char *path;       /* on-disk path passed on command line (pack) */
    const char *name;       /* basename used in archive */
    uint64_t    orig_size;  /* size of original file on disk */
    uint32_t    mode;
    uint8_t     kind;
    Buf         recipe;     /* kind=1 only */
    Buf         brn;        /* kind=2 only */
    Buf         pmp;        /* kind=3 only */
} PackEntry;

/* Run cbrunsli on a JPEG buffer; verify by dbrunsli + cmp; append the brunsli
 * blob to *out. Returns 0 on success, -1 on detection miss / tooling failure /
 * round-trip mismatch / blob >= original. tmp_prefix derives scratch paths. */
static int try_brunsli_buf(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *out) {
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8 || p[2] != 0xFF) return -1;

    char tmp_in[1024], tmp_brn[1024], tmp_jpg[1024], cmd[4096];
    snprintf(tmp_in,  sizeof(tmp_in),  "%s.in.jpg.tmp", tmp_prefix);
    snprintf(tmp_brn, sizeof(tmp_brn), "%s.brn.tmp",    tmp_prefix);
    snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.brnrt.tmp",  tmp_prefix);

    FILE *jf = fopen(tmp_in, "wb");
    if (!jf) return -1;
    if (n > 0 && fwrite(p, 1, n, jf) != n) { fclose(jf); unlink(tmp_in); return -1; }
    fclose(jf);

    snprintf(cmd, sizeof(cmd), "cbrunsli \"%s\" \"%s\" >%s 2>&1", tmp_in, tmp_brn, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(tmp_in); return -1; }
    unlink(tmp_in);

    snprintf(cmd, sizeof(cmd), "dbrunsli \"%s\" \"%s\" >%s 2>&1", tmp_brn, tmp_jpg, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(tmp_brn); return -1; }
    size_t rt_n = 0;
    uint8_t *rt = read_whole_file(tmp_jpg, &rt_n);
    int ok = (rt_n == n && memcmp(rt, p, n) == 0);
    free(rt);
    unlink(tmp_jpg);
    if (!ok) { unlink(tmp_brn); return -1; }

    size_t brn_n = 0;
    uint8_t *brn = read_whole_file(tmp_brn, &brn_n);
    unlink(tmp_brn);
    if (brn_n >= n) { free(brn); return -1; }
    buf_append(out, brn, brn_n);
    free(brn);
    return 0;
}

/* MPEG-1 Layer III sync detection. ID3v2 tag (bytes "ID3") or a frame header
 * (0xFF then top 3 bits set; full sync). Permissive — packMP3 will reject
 * MPEG-2/2.5 internally and we'll fall back to KIND_OPAQUE. */
static int looks_like_mp3(const uint8_t *p, size_t n) {
    if (n < 3) return 0;
    if (p[0]=='I' && p[1]=='D' && p[2]=='3') return 1;
    if (p[0]==0xFF && (p[1]&0xE0)==0xE0) return 1;
    return 0;
}

/* Run packMP3 on an MP3 buffer; verify by packMP3 (decode side) + cmp; append
 * the .pmp blob to *out. Returns 0 on success, -1 on detection miss / tooling
 * failure / round-trip mismatch / blob >= original. */
static int try_packmp3_buf(const uint8_t *p, size_t n, const char *tmp_prefix, Buf *out) {
    if (!looks_like_mp3(p, n)) return -1;

    char tmp_in[1024], tmp_pmp[1024], cmd[4096];
    snprintf(tmp_in,  sizeof(tmp_in),  "%s.in.mp3", tmp_prefix);
    snprintf(tmp_pmp, sizeof(tmp_pmp), "%s.in.pmp", tmp_prefix);

    FILE *jf = fopen(tmp_in, "wb");
    if (!jf) return -1;
    if (n > 0 && fwrite(p, 1, n, jf) != n) { fclose(jf); unlink(tmp_in); return -1; }
    fclose(jf);

    /* Compress: produces tmp_pmp from tmp_in. */
    snprintf(cmd, sizeof(cmd), "packMP3 -o -np \"%s\" >%s 2>&1", tmp_in, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(tmp_in); unlink(tmp_pmp); return -1; }
    unlink(tmp_in);

    /* Decompress for verify. packMP3 of foo.pmp writes foo.mp3 next to it.
     * Rename the compressed output to a path whose .mp3 sibling is free. */
    char tmp_rt_pmp[1024], tmp_rt_mp3[1024];
    snprintf(tmp_rt_pmp, sizeof(tmp_rt_pmp), "%s.rt.pmp", tmp_prefix);
    snprintf(tmp_rt_mp3, sizeof(tmp_rt_mp3), "%s.rt.mp3", tmp_prefix);
    if (rename(tmp_pmp, tmp_rt_pmp) != 0) { unlink(tmp_pmp); return -1; }
    snprintf(cmd, sizeof(cmd), "packMP3 -o -np \"%s\" >%s 2>&1", tmp_rt_pmp, ZXLE_DEVNULL);
    if (try_run(cmd) != 0) { unlink(tmp_rt_pmp); unlink(tmp_rt_mp3); return -1; }
    size_t rt_n = 0;
    uint8_t *rt = read_whole_file(tmp_rt_mp3, &rt_n);
    int ok = (rt && rt_n == n && memcmp(rt, p, n) == 0);
    free(rt);
    unlink(tmp_rt_mp3);
    if (!ok) { unlink(tmp_rt_pmp); return -1; }

    size_t pmp_n = 0;
    uint8_t *pmp = read_whole_file(tmp_rt_pmp, &pmp_n);
    unlink(tmp_rt_pmp);
    if (!pmp || pmp_n >= n) { free(pmp); return -1; }
    buf_append(out, pmp, pmp_n);
    free(pmp);
    return 0;
}

/* pack_run — main pack body. force_opaque=1 skips all container-unwrap routing
 * and stores every input as KIND_OPAQUE. Used by do_pack() to compute a
 * fall-through baseline; see "min-pack" comment in do_pack. Returns the number
 * of files unwrapped (i.e. anything other than KIND_OPAQUE) on success, or -1
 * on failure. *out_size receives the produced file size. */
static int pack_run(const char *out, int n, char **files, int force_opaque,
                    long long *out_size, uint64_t *out_total) {
    PackEntry *ents = calloc((size_t)n, sizeof(PackEntry));
    if (!ents) die("calloc ents");
    Buf solid; buf_init(&solid);

    uint64_t total = 0;
    int unwrapped_count = 0;
    for (int i = 0; i < n; i++) {
        struct stat st;
        if (stat(files[i], &st) != 0) { fprintf(stderr, "stat %s\n", files[i]); die("stat input"); }
        size_t fsz = 0;
        uint8_t *fb = read_whole_file(files[i], &fsz);
        ents[i].path      = files[i];
        ents[i].name      = basename_of(files[i]);
        ents[i].orig_size = fsz;
        ents[i].mode      = (uint32_t)st.st_mode;
        buf_init(&ents[i].recipe);
        buf_init(&ents[i].brn);
        buf_init(&ents[i].pmp);

        int unwrapped = 0;
        if (!force_opaque && fsz >= 22 && fb[0]==0x50 && fb[1]==0x4B) {
            char tp[1024];
            snprintf(tp, sizeof(tp), "%s.%d", out, i);
            if (pack_zip(fb, fsz, tp, &ents[i].recipe, &solid) == 0) {
                ents[i].kind = KIND_ZIP;
                unwrapped = 1;
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 4 && fb[0]==0xFF && fb[1]==0xD8 && fb[2]==0xFF) {
            char tmp_prefix[1024];
            snprintf(tmp_prefix, sizeof(tmp_prefix), "%s.%d", out, i);
            if (try_brunsli_buf(fb, fsz, tmp_prefix, &ents[i].brn) == 0) {
                ents[i].kind = KIND_JPEG;
                unwrapped = 1;
            }
        }
        if (!force_opaque && !unwrapped && looks_like_mp3(fb, fsz)) {
            char tmp_prefix[1024];
            snprintf(tmp_prefix, sizeof(tmp_prefix), "%s.%d", out, i);
            if (try_packmp3_buf(fb, fsz, tmp_prefix, &ents[i].pmp) == 0) {
                ents[i].kind = KIND_MP3;
                unwrapped = 1;
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 8 && memcmp(fb, PNG_SIG, 8) == 0) {
            if (pack_png(fb, fsz, &ents[i].recipe, &solid) == 0) {
                ents[i].kind = KIND_PNG;
                unwrapped = 1;
            } else {
                buf_free(&ents[i].recipe);
                buf_init(&ents[i].recipe);
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 18 && fb[0]==0x1F && fb[1]==0x8B && fb[2]==0x08) {
            char tp[1024];
            snprintf(tp, sizeof(tp), "%s.%d", out, i);
            if (pack_gz(fb, fsz, tp, &ents[i].recipe, &solid) == 0) {
                ents[i].kind = KIND_GZIP;
                unwrapped = 1;
            } else {
                buf_free(&ents[i].recipe);
                buf_init(&ents[i].recipe);
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 8 && fb[0]==0x28 && fb[1]==0xB5 && fb[2]==0x2F && fb[3]==0xFD) {
            char tp[1024];
            snprintf(tp, sizeof(tp), "%s.%d", out, i);
            if (pack_zst(fb, fsz, tp, &ents[i].recipe, &solid) == 0) {
                ents[i].kind = KIND_ZSTD;
                unwrapped = 1;
            } else {
                buf_free(&ents[i].recipe);
                buf_init(&ents[i].recipe);
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 14 && fb[0]=='B' && fb[1]=='Z' && fb[2]=='h' &&
            fb[3] >= '1' && fb[3] <= '9') {
            char tp[1024];
            snprintf(tp, sizeof(tp), "%s.%d", out, i);
            if (pack_bz2(fb, fsz, tp, &ents[i].recipe, &solid) == 0) {
                ents[i].kind = KIND_BZIP2;
                unwrapped = 1;
            } else {
                buf_free(&ents[i].recipe);
                buf_init(&ents[i].recipe);
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 1024 && memcmp(fb + 257, "ustar", 5) == 0) {
            char tp[1024];
            snprintf(tp, sizeof(tp), "%s.%d", out, i);
            if (pack_tar(fb, fsz, tp, &ents[i].recipe, &solid) == 0) {
                ents[i].kind = KIND_TAR;
                unwrapped = 1;
            } else {
                buf_free(&ents[i].recipe);
                buf_init(&ents[i].recipe);
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 8 && memcmp(fb, "!<arch>\n", 8) == 0) {
            char tp[1024];
            snprintf(tp, sizeof(tp), "%s.%d", out, i);
            if (pack_ar(fb, fsz, tp, &ents[i].recipe, &solid) == 0) {
                ents[i].kind = KIND_AR;
                unwrapped = 1;
            } else {
                buf_free(&ents[i].recipe);
                buf_init(&ents[i].recipe);
            }
        }
        if (!unwrapped) {
            ents[i].kind = KIND_OPAQUE;
            buf_append(&solid, fb, fsz);
        } else {
            unwrapped_count++;
        }
        total += fsz;
        free(fb);
    }

    /* Write solid temp, zstd it. */
    char tmp_concat[1024], tmp_zst[1024];
    snprintf(tmp_concat, sizeof(tmp_concat), "%s.concat.tmp", out);
    snprintf(tmp_zst,    sizeof(tmp_zst),    "%s.zst.tmp",    out);

    FILE *cf = fopen(tmp_concat, "wb");
    if (!cf) die("fopen concat");
    if (solid.n > 0 && fwrite(solid.p, 1, solid.n, cf) != solid.n) die("fwrite solid");
    fclose(cf);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "zstd -19 --long=27 -q -f -o \"%s\" \"%s\"", tmp_zst, tmp_concat);
    run(cmd);

    /* Compute manifest size. */
    size_t mlen = 0;
    for (int i = 0; i < n; i++) {
        mlen += 2 + strlen(ents[i].name) + 8 + 4 + 1;
        if (ents[i].kind == KIND_ZIP)  mlen += 4 + ents[i].recipe.n;
        if (ents[i].kind == KIND_JPEG) mlen += 4 + ents[i].brn.n;
        if (ents[i].kind == KIND_MP3)  mlen += 4 + ents[i].pmp.n;
        if (ents[i].kind == KIND_PNG)  mlen += 4 + ents[i].recipe.n;
        if (ents[i].kind == KIND_GZIP) mlen += 4 + ents[i].recipe.n;
        if (ents[i].kind == KIND_TAR)  mlen += 4 + ents[i].recipe.n;
        if (ents[i].kind == KIND_AR)   mlen += 4 + ents[i].recipe.n;
        if (ents[i].kind == KIND_BZIP2) mlen += 4 + ents[i].recipe.n;
        if (ents[i].kind == KIND_ZSTD)  mlen += 4 + ents[i].recipe.n;
    }

    FILE *o = fopen(out, "wb");
    if (!o) die("fopen out");
    fwrite(ZXLE_MAGIC, 1, 4, o);
    fputc(ZXLE_VER, o);
    fputc(0, o);
    wu32(o, (uint32_t)mlen);
    for (int i = 0; i < n; i++) {
        size_t plen = strlen(ents[i].name);
        wu16(o, (uint16_t)plen);
        fwrite(ents[i].name, 1, plen, o);
        wu64(o, ents[i].orig_size);
        wu32(o, ents[i].mode);
        fputc(ents[i].kind, o);
        if (ents[i].kind == KIND_ZIP) {
            wu32(o, (uint32_t)ents[i].recipe.n);
            if (ents[i].recipe.n > 0)
                fwrite(ents[i].recipe.p, 1, ents[i].recipe.n, o);
        } else if (ents[i].kind == KIND_JPEG) {
            wu32(o, (uint32_t)ents[i].brn.n);
            if (ents[i].brn.n > 0)
                fwrite(ents[i].brn.p, 1, ents[i].brn.n, o);
        } else if (ents[i].kind == KIND_MP3) {
            wu32(o, (uint32_t)ents[i].pmp.n);
            if (ents[i].pmp.n > 0)
                fwrite(ents[i].pmp.p, 1, ents[i].pmp.n, o);
        } else if (ents[i].kind == KIND_PNG) {
            wu32(o, (uint32_t)ents[i].recipe.n);
            if (ents[i].recipe.n > 0)
                fwrite(ents[i].recipe.p, 1, ents[i].recipe.n, o);
        } else if (ents[i].kind == KIND_GZIP) {
            wu32(o, (uint32_t)ents[i].recipe.n);
            if (ents[i].recipe.n > 0)
                fwrite(ents[i].recipe.p, 1, ents[i].recipe.n, o);
        } else if (ents[i].kind == KIND_TAR) {
            wu32(o, (uint32_t)ents[i].recipe.n);
            if (ents[i].recipe.n > 0)
                fwrite(ents[i].recipe.p, 1, ents[i].recipe.n, o);
        } else if (ents[i].kind == KIND_AR) {
            wu32(o, (uint32_t)ents[i].recipe.n);
            if (ents[i].recipe.n > 0)
                fwrite(ents[i].recipe.p, 1, ents[i].recipe.n, o);
        } else if (ents[i].kind == KIND_BZIP2) {
            wu32(o, (uint32_t)ents[i].recipe.n);
            if (ents[i].recipe.n > 0)
                fwrite(ents[i].recipe.p, 1, ents[i].recipe.n, o);
        } else if (ents[i].kind == KIND_ZSTD) {
            wu32(o, (uint32_t)ents[i].recipe.n);
            if (ents[i].recipe.n > 0)
                fwrite(ents[i].recipe.p, 1, ents[i].recipe.n, o);
        }
    }
    FILE *zf = fopen(tmp_zst, "rb");
    if (!zf) die("fopen zst");
    char buf[65536]; size_t got;
    while ((got = fread(buf, 1, sizeof(buf), zf)) > 0)
        if (fwrite(buf, 1, got, o) != got) die("fwrite payload");
    fclose(zf);
    fclose(o);

    unlink(tmp_concat);
    unlink(tmp_zst);
    for (int i = 0; i < n; i++) { buf_free(&ents[i].recipe); buf_free(&ents[i].brn); buf_free(&ents[i].pmp); }
    buf_free(&solid);
    free(ents);

    long long osz = fsize(out);
    if (out_size)  *out_size  = osz;
    if (out_total) *out_total = total;
    return unwrapped_count;
}

static int do_pack(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: zxle pack <out.zxle> <files...>\n"); return 1; }
    const char *out = argv[0];
    int n = argc - 1;
    char **files = argv + 1;

    /* min-pack: pack with container unwrap engaged, then if any entry was
     * unwrapped, also pack as all-opaque and keep the smaller. Cures the
     * "small tightly-deflated input inflates to bigger solid" regression
     * (see tests/real_world.md, npm tarballs +144%/+247% before this fix). */
    long long osz = -1; uint64_t total = 0;
    int unwrapped = pack_run(out, n, files, 0, &osz, &total);

    if (unwrapped > 0) {
        char opq[1024];
        snprintf(opq, sizeof(opq), "%s.opq.tmp", out);
        long long opq_osz = -1; uint64_t opq_total = 0;
        pack_run(opq, n, files, 1, &opq_osz, &opq_total);
        if (opq_osz > 0 && opq_osz < osz) {
            fprintf(stderr, "min-pack: opaque %lld < unwrap %lld -> using opaque\n",
                    opq_osz, osz);
            unlink(out);
            if (rename(opq, out) != 0) die("rename opq->out");
            osz = opq_osz;
        } else {
            unlink(opq);
        }
    }

    fprintf(stderr, "packed %d file(s), orig=%llu zxle=%lld ratio=%.4f\n",
            n, (unsigned long long)total, osz,
            total ? (double)osz / (double)total : 0.0);
    return 0;
}

typedef struct {
    char     name[1024];
    uint64_t orig_size;
    uint32_t mode;
    uint8_t  kind;
    uint8_t *recipe;        /* points into manifest buffer */
    uint32_t recipe_len;
    uint8_t *brn;           /* points into manifest buffer */
    uint32_t brn_len;
    uint8_t *pmp;           /* points into manifest buffer */
    uint32_t pmp_len;
} UnpackEntry;

static int do_unpack(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: zxle unpack <in.zxle> <outdir>\n"); return 1; }
    const char *in = argv[0];
    const char *outdir = argv[1];

    FILE *f = fopen(in, "rb");
    if (!f) die("fopen in");
    char magic[4];
    if (fread(magic,1,4,f) != 4 || memcmp(magic, ZXLE_MAGIC, 4) != 0) die("bad magic");
    int ver = fgetc(f), flags = fgetc(f);
    (void)flags;
    if (ver != ZXLE_VER) die("bad version");

    uint8_t mlen_b[4];
    if (fread(mlen_b, 1, 4, f) != 4) die("read mlen");
    uint32_t mlen = r32(mlen_b);
    uint8_t *manifest = malloc(mlen ? mlen : 1);
    if (!manifest) die("malloc manifest");
    if (mlen > 0 && fread(manifest, 1, mlen, f) != mlen) die("read manifest");

    /* Parse manifest. */
    int count = 0, cap = 0;
    UnpackEntry *ents = NULL;
    size_t mp = 0;
    while (mp < mlen) {
        if (count == cap) { cap = cap ? cap*2 : 16; ents = realloc(ents, (size_t)cap * sizeof(UnpackEntry)); if (!ents) die("realloc"); }
        if (mp + 2 > mlen) die("manifest truncated");
        uint16_t pl = r16(manifest + mp); mp += 2;
        if (pl >= sizeof(ents[0].name) || mp + pl + 8 + 4 + 1 > mlen) die("manifest overflow");
        memcpy(ents[count].name, manifest + mp, pl); ents[count].name[pl] = 0; mp += pl;
        ents[count].orig_size = (uint64_t)r32(manifest + mp) | ((uint64_t)r32(manifest + mp + 4) << 32); mp += 8;
        ents[count].mode = r32(manifest + mp); mp += 4;
        ents[count].kind = manifest[mp]; mp += 1;
        ents[count].recipe = NULL; ents[count].recipe_len = 0;
        ents[count].brn    = NULL; ents[count].brn_len    = 0;
        ents[count].pmp    = NULL; ents[count].pmp_len    = 0;
        if (ents[count].kind == KIND_ZIP) {
            if (mp + 4 > mlen) die("recipe len truncated");
            ents[count].recipe_len = r32(manifest + mp); mp += 4;
            if (mp + ents[count].recipe_len > mlen) die("recipe overflow");
            ents[count].recipe = manifest + mp;
            mp += ents[count].recipe_len;
        } else if (ents[count].kind == KIND_JPEG) {
            if (mp + 4 > mlen) die("brn len truncated");
            ents[count].brn_len = r32(manifest + mp); mp += 4;
            if (mp + ents[count].brn_len > mlen) die("brn overflow");
            ents[count].brn = manifest + mp;
            mp += ents[count].brn_len;
        } else if (ents[count].kind == KIND_MP3) {
            if (mp + 4 > mlen) die("pmp len truncated");
            ents[count].pmp_len = r32(manifest + mp); mp += 4;
            if (mp + ents[count].pmp_len > mlen) die("pmp overflow");
            ents[count].pmp = manifest + mp;
            mp += ents[count].pmp_len;
        } else if (ents[count].kind == KIND_PNG) {
            if (mp + 4 > mlen) die("png recipe len truncated");
            ents[count].recipe_len = r32(manifest + mp); mp += 4;
            if (mp + ents[count].recipe_len > mlen) die("png recipe overflow");
            ents[count].recipe = manifest + mp;
            mp += ents[count].recipe_len;
        } else if (ents[count].kind == KIND_GZIP) {
            if (mp + 4 > mlen) die("gz recipe len truncated");
            ents[count].recipe_len = r32(manifest + mp); mp += 4;
            if (mp + ents[count].recipe_len > mlen) die("gz recipe overflow");
            ents[count].recipe = manifest + mp;
            mp += ents[count].recipe_len;
        } else if (ents[count].kind == KIND_TAR) {
            if (mp + 4 > mlen) die("tar recipe len truncated");
            ents[count].recipe_len = r32(manifest + mp); mp += 4;
            if (mp + ents[count].recipe_len > mlen) die("tar recipe overflow");
            ents[count].recipe = manifest + mp;
            mp += ents[count].recipe_len;
        } else if (ents[count].kind == KIND_AR) {
            if (mp + 4 > mlen) die("ar recipe len truncated");
            ents[count].recipe_len = r32(manifest + mp); mp += 4;
            if (mp + ents[count].recipe_len > mlen) die("ar recipe overflow");
            ents[count].recipe = manifest + mp;
            mp += ents[count].recipe_len;
        } else if (ents[count].kind == KIND_BZIP2) {
            if (mp + 4 > mlen) die("bz2 recipe len truncated");
            ents[count].recipe_len = r32(manifest + mp); mp += 4;
            if (mp + ents[count].recipe_len > mlen) die("bz2 recipe overflow");
            ents[count].recipe = manifest + mp;
            mp += ents[count].recipe_len;
        } else if (ents[count].kind == KIND_ZSTD) {
            if (mp + 4 > mlen) die("zst recipe len truncated");
            ents[count].recipe_len = r32(manifest + mp); mp += 4;
            if (mp + ents[count].recipe_len > mlen) die("zst recipe overflow");
            ents[count].recipe = manifest + mp;
            mp += ents[count].recipe_len;
        }
        count++;
    }

    /* Stream payload to temp, then zstd -d to a solid buffer file. */
    char tmp_zst[1024], tmp_concat[1024];
    snprintf(tmp_zst,    sizeof(tmp_zst),    "%s.unpack.zst.tmp",    in);
    snprintf(tmp_concat, sizeof(tmp_concat), "%s.unpack.concat.tmp", in);

    FILE *zf = fopen(tmp_zst, "wb");
    if (!zf) die("fopen tmp zst");
    char buf[65536]; size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
        if (fwrite(buf, 1, got, zf) != got) die("fwrite tmp zst");
    fclose(zf);
    fclose(f);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "zstd -d -q -f -o \"%s\" \"%s\"", tmp_concat, tmp_zst);
    run(cmd);

    size_t solid_len = 0;
    uint8_t *solid = read_whole_file(tmp_concat, &solid_len);

    if (ZXLE_MKDIR(outdir) != 0 && errno != EEXIST) die("mkdir outdir");

    size_t solid_pos = 0;
    for (int i = 0; i < count; i++) {
        char p[2048];
        snprintf(p, sizeof(p), "%s/%s", outdir, ents[i].name);
        FILE *of = fopen(p, "wb");
        if (!of) { fprintf(stderr, "fopen %s\n", p); die("fopen out"); }
        if (ents[i].kind == KIND_OPAQUE) {
            if (solid_pos + ents[i].orig_size > solid_len) die("opaque overflow");
            if (ents[i].orig_size > 0 && fwrite(solid + solid_pos, 1, ents[i].orig_size, of) != ents[i].orig_size) die("fwrite opaque");
            solid_pos += ents[i].orig_size;
            fclose(of);
        } else if (ents[i].kind == KIND_ZIP) {
            unpack_recipe(ents[i].recipe, ents[i].recipe_len,
                          solid, solid_len, &solid_pos,
                          of, ents[i].orig_size, p);
            fclose(of);
        } else if (ents[i].kind == KIND_JPEG) {
            /* Write brn blob to a temp, dbrunsli to the output path. */
            fclose(of);
            char tmp_brn[2048], cmd[4096];
            snprintf(tmp_brn, sizeof(tmp_brn), "%s.brn.tmp", p);
            FILE *bf = fopen(tmp_brn, "wb");
            if (!bf) die("fopen tmp brn");
            if (ents[i].brn_len > 0 && fwrite(ents[i].brn, 1, ents[i].brn_len, bf) != ents[i].brn_len) die("fwrite tmp brn");
            fclose(bf);
            snprintf(cmd, sizeof(cmd), "dbrunsli \"%s\" \"%s\" >%s 2>&1", tmp_brn, p, ZXLE_DEVNULL);
            run(cmd);
            unlink(tmp_brn);
        } else if (ents[i].kind == KIND_PNG) {
            unpack_png(ents[i].recipe, ents[i].recipe_len,
                       solid, solid_len, &solid_pos,
                       of, ents[i].orig_size);
            fclose(of);
        } else if (ents[i].kind == KIND_GZIP) {
            unpack_gz(ents[i].recipe, ents[i].recipe_len,
                      solid, solid_len, &solid_pos,
                      of, ents[i].orig_size, p);
            fclose(of);
        } else if (ents[i].kind == KIND_TAR) {
            unpack_recipe(ents[i].recipe, ents[i].recipe_len,
                          solid, solid_len, &solid_pos,
                          of, ents[i].orig_size, p);
            fclose(of);
        } else if (ents[i].kind == KIND_AR) {
            unpack_recipe(ents[i].recipe, ents[i].recipe_len,
                          solid, solid_len, &solid_pos,
                          of, ents[i].orig_size, p);
            fclose(of);
        } else if (ents[i].kind == KIND_BZIP2) {
            unpack_bz2(ents[i].recipe, ents[i].recipe_len,
                       solid, solid_len, &solid_pos,
                       of, ents[i].orig_size, p);
            fclose(of);
        } else if (ents[i].kind == KIND_ZSTD) {
            unpack_zst(ents[i].recipe, ents[i].recipe_len,
                       solid, solid_len, &solid_pos,
                       of, ents[i].orig_size, p);
            fclose(of);
        } else if (ents[i].kind == KIND_MP3) {
            /* Write pmp blob to a sibling .pmp temp; packMP3 will produce a
             * sibling .mp3 next to it; then move that to the output path. */
            fclose(of);
            char tmp_pmp[2048], tmp_mp3[2048], cmd[4096];
            snprintf(tmp_pmp, sizeof(tmp_pmp), "%s.pmp.tmp.pmp", p);
            snprintf(tmp_mp3, sizeof(tmp_mp3), "%s.pmp.tmp.mp3", p);
            FILE *bf = fopen(tmp_pmp, "wb");
            if (!bf) die("fopen tmp pmp");
            if (ents[i].pmp_len > 0 && fwrite(ents[i].pmp, 1, ents[i].pmp_len, bf) != ents[i].pmp_len) die("fwrite tmp pmp");
            fclose(bf);
            snprintf(cmd, sizeof(cmd), "packMP3 -o -np \"%s\" >%s 2>&1", tmp_pmp, ZXLE_DEVNULL);
            run(cmd);
            unlink(tmp_pmp);
            /* Move recovered .mp3 to the output path. */
            unlink(p);
            if (rename(tmp_mp3, p) != 0) die("rename mp3 out");
        } else {
            die("unknown kind");
        }
    }
    if (solid_pos != solid_len) die("solid stream not fully consumed");

    free(solid);
    free(manifest);
    free(ents);
    unlink(tmp_zst);
    unlink(tmp_concat);

    fprintf(stderr, "unpacked %d file(s) to %s\n", count, outdir);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "ZXL-E (M2)\n"
        "  zxle pack   <out.zxle> <files...>\n"
        "  zxle unpack <in.zxle>  <outdir>\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    if (strcmp(argv[1], "pack")   == 0) return do_pack  (argc - 2, argv + 2);
    if (strcmp(argv[1], "unpack") == 0) return do_unpack(argc - 2, argv + 2);
    usage();
    return 1;
}
