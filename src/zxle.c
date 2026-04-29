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
 *     u8  kind                    -- 0=opaque, 1=zip-unwrap
 *     [if kind==1] u32 recipe_len, recipe_bytes
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
#else
#define ZXLE_MKDIR(p) mkdir((p), 0755)
#endif

#define ZXLE_MAGIC "ZXLE"
#define ZXLE_VER 2

#define KIND_OPAQUE 0
#define KIND_ZIP    1

#define OP_STRUCT    0x00
#define OP_REDEFLATE 0x01
#define OP_STORE     0x02
#define OP_PREFLATE  0x03

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
static int pack_zip(const uint8_t *p, size_t n, Buf *recipe, Buf *solid) {
    ZipEntry *ents = NULL;
    uint32_t count = 0;
    size_t cd_off = 0, cd_len = 0, eocd_off = 0, eocd_len = 0;
    if (zip_parse(p, n, &ents, &count, &cd_off, &cd_len, &eocd_off, &eocd_len) != 0) return -1;

    size_t cursor = 0;
    int redeflated = 0, preflated = 0, store_orig = 0, stored_method = 0;

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
            /* Stored: raw bytes are the payload bytes. */
            if (e->raw_size != e->comp_size) { free(ents); return -1; }
            buf_u8(recipe, OP_STORE);
            buf_u32(recipe, e->raw_size);
            buf_append(solid, p + e->payload_off, e->raw_size);
            stored_method++;
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

    fprintf(stderr, "    zip: %u entries (%d redeflate, %d preflate, %d store-orig, %d stored)\n",
            count, redeflated, preflated, store_orig, stored_method);

    free(ents);
    return 0;
}

/* ---------- unpack: walk recipe ---------- */

static void unpack_recipe(const uint8_t *recipe, size_t rlen,
                          const uint8_t *solid, size_t solid_len, size_t *solid_pos,
                          FILE *out, uint64_t expected_size) {
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
} PackEntry;

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

        int unwrapped = 0;
        if (fsz >= 22 && fb[0]==0x50 && fb[1]==0x4B) {
            if (pack_zip(fb, fsz, &ents[i].recipe, &solid) == 0) {
                ents[i].kind = KIND_ZIP;
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
        if (ents[i].kind == KIND_ZIP) mlen += 4 + ents[i].recipe.n;
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
    for (int i = 0; i < n; i++) buf_free(&ents[i].recipe);
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
        if (ents[count].kind == KIND_ZIP) {
            if (mp + 4 > mlen) die("recipe len truncated");
            ents[count].recipe_len = r32(manifest + mp); mp += 4;
            if (mp + ents[count].recipe_len > mlen) die("recipe overflow");
            ents[count].recipe = manifest + mp;
            mp += ents[count].recipe_len;
        } else {
            ents[count].recipe = NULL;
            ents[count].recipe_len = 0;
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
        } else if (ents[i].kind == KIND_ZIP) {
            unpack_recipe(ents[i].recipe, ents[i].recipe_len,
                          solid, solid_len, &solid_pos,
                          of, ents[i].orig_size);
        } else {
            die("unknown kind");
        }
        fclose(of);
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
