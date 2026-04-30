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
 *                                    3=mp3-packmp3
 *     [if kind==1] u32 recipe_len, recipe_bytes
 *     [if kind==2] u32 brn_len,    brn_bytes      -- brunsli blob; entry
 *                                                    contributes nothing to
 *                                                    the solid stream
 *     [if kind==3] u32 pmp_len,    pmp_bytes      -- packmp3 blob; entry
 *                                                    contributes nothing to
 *                                                    the solid stream
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

#define OP_STRUCT     0x00
#define OP_REDEFLATE  0x01
#define OP_STORE      0x02
#define OP_PREFLATE   0x03
#define OP_JPEG_STORE 0x04

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
    int redeflated = 0, preflated = 0, store_orig = 0, stored_method = 0, jpeg_stored = 0;

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
            if (e->raw_size >= 4 &&
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

    fprintf(stderr, "    zip: %u entries (%d redeflate, %d preflate, %d store-orig, %d stored, %d jpeg-store)\n",
            count, redeflated, preflated, store_orig, stored_method, jpeg_stored);

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

static int do_pack(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: zxle pack <out.zxle> <files...>\n"); return 1; }
    const char *out = argv[0];
    int n = argc - 1;
    char **files = argv + 1;

    PackEntry *ents = calloc((size_t)n, sizeof(PackEntry));
    if (!ents) die("calloc ents");
    Buf solid; buf_init(&solid);

    uint64_t total = 0;
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
        if (fsz >= 22 && fb[0]==0x50 && fb[1]==0x4B) {
            char tp[1024];
            snprintf(tp, sizeof(tp), "%s.%d", out, i);
            if (pack_zip(fb, fsz, tp, &ents[i].recipe, &solid) == 0) {
                ents[i].kind = KIND_ZIP;
                unwrapped = 1;
            }
        }
        if (!unwrapped && fsz >= 4 && fb[0]==0xFF && fb[1]==0xD8 && fb[2]==0xFF) {
            char tmp_prefix[1024];
            snprintf(tmp_prefix, sizeof(tmp_prefix), "%s.%d", out, i);
            if (try_brunsli_buf(fb, fsz, tmp_prefix, &ents[i].brn) == 0) {
                ents[i].kind = KIND_JPEG;
                unwrapped = 1;
            }
        }
        if (!unwrapped && looks_like_mp3(fb, fsz)) {
            char tmp_prefix[1024];
            snprintf(tmp_prefix, sizeof(tmp_prefix), "%s.%d", out, i);
            if (try_packmp3_buf(fb, fsz, tmp_prefix, &ents[i].pmp) == 0) {
                ents[i].kind = KIND_MP3;
                unwrapped = 1;
            }
        }
        if (!unwrapped) {
            ents[i].kind = KIND_OPAQUE;
            buf_append(&solid, fb, fsz);
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
