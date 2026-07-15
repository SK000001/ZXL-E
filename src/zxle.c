/* ZXL-E driver — main, do_pack/do_unpack/pack_run, manifest read/write.
 *
 * Format-aware container handlers, recipe walker, and shared helpers live in
 * sibling translation units (see graph.md). zxle.c only stitches them together
 * via the manifest and dispatches per-entry by KIND. */

#include "util.h"
#include "kinds.h"
#include "zip.h"
#include "png.h"
#include "gz.h"
#include "bz2.h"
#include "zst.h"
#include "xz.h"
#include "tar.h"
#include "ar.h"
#include "jpeg.h"
#include "mp3.h"
#include "pdf.h"
#include "recipe.h"
#include <pthread.h>
#include <zlib.h>

typedef struct {
    const char *path;
    const char *name;
    uint64_t    orig_size;
    uint32_t    mode;
    uint32_t    crc;        /* v7: crc32 of the original file bytes */
    uint8_t     kind;
    uint8_t     opaque_bucket;  /* M6 v1: which solid bucket KIND_OPAQUE bytes go to */
    Buf         recipe;     /* most kinds */
    Buf         brn;        /* KIND_JPEG */
    Buf         pmp;        /* KIND_MP3  */
} PackEntry;

/* M6 codec ids stored in the v4+ trailing payload's per-bucket header. */
#define CODEC_XZ_9E       0
#define CODEC_XZ_9E_X86   1
#define CODEC_ZPAQ_M5     2

/* Compress one solid bucket's bytes with the given codec into *payload.
 * `tag` keys the temp filenames (callers may finalize competing candidates
 * for the same logical bucket). fast: xz --threads=0 with an input-scaled
 * block size -- bucket/ncpus clamped [8 MiB, 64 MiB] (the preset-9e dict);
 * fixed 8 MiB overpaid ratio at scale (128 blocks at 1 GB for <=16 workers),
 * and larger blocks measured *slower* (16 concurrent 9e encoders hit memory
 * pressure before the wave-quantization win shows up). */
static void finalize_bucket(const Buf *in_buf, int codec, int fast,
                            const char *out, int tag, Buf *payload) {
    char cmd[4096], tmp_concat[1024], tmp_payload[1024];
    snprintf(tmp_concat,  sizeof(tmp_concat),  "%s.b%d.concat.tmp", out, tag);
    snprintf(tmp_payload, sizeof(tmp_payload), "%s.b%d.pack.tmp",   out, tag);

    char fastopt[64] = "--threads=1";
    if (fast) {
        uint64_t bs = (uint64_t)in_buf->n / (uint64_t)zxle_ncpus();
        if (bs < (uint64_t)8  << 20) bs = (uint64_t)8  << 20;
        if (bs > (uint64_t)64 << 20) bs = (uint64_t)64 << 20;
        snprintf(fastopt, sizeof(fastopt), "--threads=0 --block-size=%llu",
                 (unsigned long long)bs);
    }

    FILE *cf = fopen(tmp_concat, "wb");
    if (!cf) die("fopen concat");
    if (fwrite(in_buf->p, 1, in_buf->n, cf) != in_buf->n) die("fwrite solid");
    fclose(cf);

    switch (codec) {
    case CODEC_ZPAQ_M5:
        unlink(tmp_payload);
        snprintf(cmd, sizeof(cmd),
                 "zpaq a \"%s\" \"%s\" -m5 >%s 2>&1",
                 tmp_payload, tmp_concat, ZXLE_DEVNULL);
        break;
    case CODEC_XZ_9E_X86:
        /* BCJ filter is recorded in the xz block header, so plain `xz -d`
         * reverses it; xz -d also consumes --fast's multi-block streams
         * transparently, so the manifest format is unchanged either way. */
        snprintf(cmd, sizeof(cmd),
                 "xz -9e --x86 --lzma2=preset=9e -c %s \"%s\" > \"%s\" 2>%s",
                 fastopt, tmp_concat, tmp_payload, ZXLE_DEVNULL);
        break;
    case CODEC_XZ_9E:
    default:
        snprintf(cmd, sizeof(cmd),
                 "xz -9e -c %s \"%s\" > \"%s\" 2>%s",
                 fastopt, tmp_concat, tmp_payload, ZXLE_DEVNULL);
        break;
    }
    run(cmd);

    size_t plen = 0;
    uint8_t *pbytes = read_whole_file(tmp_payload, &plen);
    if (!pbytes) die("read packed bucket");
    buf_append(payload, pbytes, plen);
    free(pbytes);

    unlink(tmp_concat);
    unlink(tmp_payload);
}

/* crc32 over an arbitrary-size buffer (zlib's crc32 takes a 32-bit uInt len). */
static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    uLong c = crc32(0L, Z_NULL, 0);
    while (n > 0) {
        uInt chunk = n > (1u << 30) ? (1u << 30) : (uInt)n;
        c = crc32(c, p, chunk);
        p += chunk; n -= chunk;
    }
    return (uint32_t)c;
}

/* pack_run — main pack body. force_opaque=1 skips all container-unwrap routing
 * and stores every input as KIND_OPAQUE. slow=1 finalizes the solid stream
 * with zpaq -m5 instead of xz -9e (cmix-class context-mixing on raw streams;
 * 5-10x slower). Used by do_pack() to compute a fall-through baseline; see
 * "min-pack" comment in do_pack. Returns the number of files unwrapped (i.e.
 * anything other than KIND_OPAQUE) on success. *out_size receives the
 * produced file size. */
static int pack_run(const char *out, int n, char **files, int force_opaque,
                    int slow, int fast, long long *out_size, uint64_t *out_total) {
    PackEntry *ents = calloc((size_t)n, sizeof(PackEntry));
    if (!ents) die("calloc ents");
    /* M6: two solid buckets. solid (bucket 0) holds main-codec bytes (xz or
     * zpaq depending on --slow); solid_x86 (bucket 1) holds PE/ELF bytes
     * which always go through xz with the BCJ x86 filter. All format-aware
     * unwrap kinds (KIND_ZIP/TAR/AR/etc.) still feed bucket 0 only -- M6 v1
     * routes only top-level KIND_OPAQUE entries. */
    Buf solid;     buf_init(&solid);
    Buf solid_x86; buf_init(&solid_x86);

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
        ents[i].crc       = crc32_buf(fb, fsz);
        buf_init(&ents[i].recipe);
        buf_init(&ents[i].brn);
        buf_init(&ents[i].pmp);

        int unwrapped = 0;
        /* M6 v3: each pack_<kind> takes both buckets (b0 main, b1 x86) and
         * routes per-entry/per-OP via internal sniffing. The container-level
         * pre-sniff helpers (zip_is_pe_heavy, wrapped_is_pe_heavy, etc.) are
         * gone; pack_zip/tar/ar walk entries and dispatch each one, while
         * pack_gz/bz2/xz/zst sniff their own inflated body. */
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.%d", out, i);
        if (!force_opaque && fsz >= 22 && fb[0]==0x50 && fb[1]==0x4B) {
            if (pack_zip(fb, fsz, tp, &ents[i].recipe, &solid, &solid_x86) == 0) {
                ents[i].kind = KIND_ZIP;
                unwrapped = 1;
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 4 && fb[0]==0xFF && fb[1]==0xD8 && fb[2]==0xFF) {
            if (try_jpeg_buf(fb, fsz, tp, &ents[i].brn) == 0) {
                ents[i].kind = KIND_JPEG;
                unwrapped = 1;
            }
        }
        if (!force_opaque && !unwrapped && looks_like_mp3(fb, fsz)) {
            if (try_packmp3_buf(fb, fsz, tp, &ents[i].pmp) == 0) {
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
            if (pack_gz(fb, fsz, tp, &ents[i].recipe, &solid, &solid_x86, 0) == 0) {
                ents[i].kind = KIND_GZIP;
                unwrapped = 1;
            } else {
                buf_free(&ents[i].recipe);
                buf_init(&ents[i].recipe);
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 8 && fb[0]==0x28 && fb[1]==0xB5 && fb[2]==0x2F && fb[3]==0xFD) {
            if (pack_zst(fb, fsz, tp, &ents[i].recipe, &solid, &solid_x86, 0) == 0) {
                ents[i].kind = KIND_ZSTD;
                unwrapped = 1;
            } else {
                buf_free(&ents[i].recipe);
                buf_init(&ents[i].recipe);
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 12 &&
            fb[0]==0xFD && fb[1]==0x37 && fb[2]==0x7A && fb[3]==0x58 && fb[4]==0x5A && fb[5]==0x00) {
            if (pack_xz(fb, fsz, tp, &ents[i].recipe, &solid, &solid_x86, 0) == 0) {
                ents[i].kind = KIND_XZ;
                unwrapped = 1;
            } else {
                buf_free(&ents[i].recipe);
                buf_init(&ents[i].recipe);
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 14 && fb[0]=='B' && fb[1]=='Z' && fb[2]=='h' &&
            fb[3] >= '1' && fb[3] <= '9') {
            if (pack_bz2(fb, fsz, tp, &ents[i].recipe, &solid, &solid_x86, 0) == 0) {
                ents[i].kind = KIND_BZIP2;
                unwrapped = 1;
            } else {
                buf_free(&ents[i].recipe);
                buf_init(&ents[i].recipe);
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 1024 && memcmp(fb + 257, "ustar", 5) == 0) {
            if (pack_tar(fb, fsz, tp, &ents[i].recipe, &solid, &solid_x86) == 0) {
                ents[i].kind = KIND_TAR;
                unwrapped = 1;
            } else {
                buf_free(&ents[i].recipe);
                buf_init(&ents[i].recipe);
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 8 && memcmp(fb, "!<arch>\n", 8) == 0) {
            if (pack_ar(fb, fsz, tp, &ents[i].recipe, &solid, &solid_x86) == 0) {
                ents[i].kind = KIND_AR;
                unwrapped = 1;
            } else {
                buf_free(&ents[i].recipe);
                buf_init(&ents[i].recipe);
            }
        }
        if (!force_opaque && !unwrapped && fsz >= 32 && memcmp(fb, "%PDF-", 5) == 0) {
            if (pack_pdf(fb, fsz, tp, &ents[i].recipe, &solid, &solid_x86) == 0) {
                ents[i].kind = KIND_PDF;
                unwrapped = 1;
            } else {
                buf_free(&ents[i].recipe);
                buf_init(&ents[i].recipe);
            }
        }
        if (!unwrapped) {
            ents[i].kind = KIND_OPAQUE;
            ents[i].opaque_bucket = bucket_for_bytes(fb, fsz);
            if (ents[i].opaque_bucket == 1) {
                buf_append(&solid_x86, fb, fsz);
            } else {
                buf_append(&solid, fb, fsz);
            }
        } else {
            unwrapped_count++;
        }
        total += fsz;
        free(fb);
    }

    /* Build the manifest in memory. v7: entries gain u32 crc32 after mode;
     * recipes carry per-OP buckets internally; KIND_OPAQUE has opaque_bucket. */
    Buf mbuf; buf_init(&mbuf);
    for (int i = 0; i < n; i++) {
        size_t plen = strlen(ents[i].name);
        buf_u16(&mbuf, (uint16_t)plen);
        buf_append(&mbuf, ents[i].name, plen);
        buf_u64(&mbuf, ents[i].orig_size);
        buf_u32(&mbuf, ents[i].mode);
        buf_u32(&mbuf, ents[i].crc);
        buf_u8(&mbuf, ents[i].kind);
        if (ents[i].kind == KIND_OPAQUE) buf_u8(&mbuf, ents[i].opaque_bucket);

        const Buf *blob = NULL;
        switch (ents[i].kind) {
        case KIND_ZIP:
        case KIND_PNG:
        case KIND_GZIP:
        case KIND_TAR:
        case KIND_AR:
        case KIND_BZIP2:
        case KIND_ZSTD:
        case KIND_XZ:
        case KIND_PDF:   blob = &ents[i].recipe; break;
        case KIND_JPEG:  blob = &ents[i].brn;    break;
        case KIND_MP3:   blob = &ents[i].pmp;    break;
        default: break;
        }
        if (blob) {
            buf_u32(&mbuf, (uint32_t)blob->n);
            if (blob->n > 0) buf_append(&mbuf, blob->p, blob->n);
        }
    }

    /* M6 finalize: pack each non-empty bucket separately. Bucket 0 ("main")
     * uses the requested codec (xz-9e or zpaq-m5 per slow); bucket 1 ("x86")
     * always uses xz-9e with the BCJ x86 filter regardless of slow because
     * BCJ's win is filter-not-codec, and the bucket only contains x86 code. */
    int  bucket_codec[2] = {
        slow ? CODEC_ZPAQ_M5 : CODEC_XZ_9E,
        CODEC_XZ_9E_X86,
    };
    Buf  bucket_payload[2];
    buf_init(&bucket_payload[0]);
    buf_init(&bucket_payload[1]);
    if (solid_x86.n > 0)
        finalize_bucket(&solid_x86, CODEC_XZ_9E_X86, fast, out, 1, &bucket_payload[1]);

    /* Split-path manifest compression, shared by the non-merged branches
     * below. Recipes hold the raw container-structure bytes (512 B tar
     * headers, ZIP LFH/CD/EOCD, padding) which are highly redundant; storing
     * them uncompressed cost up to 12% of archive size on tar-shaped fixtures
     * and 65% on entry-heavy ZIPs. comp_mlen==0 in the header means the raw
     * manifest follows (xz not smaller). Manifests below the xz container
     * overhead floor (200 B) skip the probe. */
    uint8_t *comp_m = NULL;
    size_t   comp_mlen = 0;
    int      m_split_done = 0;
#define SPLIT_MANIFEST_XZ() do {                                              \
        m_split_done = 1;                                                     \
        if (mbuf.n >= 200) {                                                  \
            char m_in[1024], m_xz[1024], mcmd[4096];                          \
            snprintf(m_in, sizeof(m_in), "%s.mf.tmp",    out);                \
            snprintf(m_xz, sizeof(m_xz), "%s.mf.xz.tmp", out);                \
            FILE *mf = fopen(m_in, "wb");                                     \
            if (!mf) die("fopen manifest tmp");                               \
            if (fwrite(mbuf.p, 1, mbuf.n, mf) != mbuf.n)                      \
                die("fwrite manifest tmp");                                   \
            fclose(mf);                                                       \
            /* Degenerate-shape guard: OP_STRUCT-heavy manifests (e.g.        \
             * concatenated tars) can rival the input size; -9e -T1 there     \
             * would dominate pack time. */                                   \
            snprintf(mcmd, sizeof(mcmd), "xz -9e %s -c \"%s\" > \"%s\" 2>%s", \
                     mbuf.n > ((size_t)64 << 20)                              \
                         ? "--threads=0 --block-size=8388608" : "--threads=1",\
                     m_in, m_xz, ZXLE_DEVNULL);                               \
            if (try_run(mcmd) == 0) {                                         \
                comp_m = read_whole_file(m_xz, &comp_mlen);                   \
                if (comp_mlen >= mbuf.n) {                                    \
                    free(comp_m); comp_m = NULL; comp_mlen = 0;               \
                }                                                             \
            }                                                                 \
            unlink(m_in);                                                     \
            unlink(m_xz);                                                     \
        }                                                                     \
    } while (0)

    /* v7 merged mode (flags bit 1): when bucket 0 is xz and non-empty, the
     * manifest rides at the head of bucket 0's stream instead of its own xz
     * block -- saves one xz container overhead and shares context between
     * the recipe's structural bytes and the content (sample.jar 3,044 ->
     * 2,804). Which layout wins is input-dependent (pptx measured +44 B
     * merged), so on small inputs both candidates are built and the smaller
     * kept, mirroring the min-pack philosophy; past the threshold merged
     * wins by construction (fixed ~60 B saving, context effects vanish).
     * Bucket-0-empty archives (blob-only) and --slow (zpaq bucket) always
     * take the split path with its raw-manifest fallback. */
    int merged = 0;
    if (!slow && solid.n > 0) {
        Buf merged0; buf_init(&merged0);
        buf_append(&merged0, mbuf.p, mbuf.n);
        buf_append(&merged0, solid.p, solid.n);
        finalize_bucket(&merged0, CODEC_XZ_9E, fast, out, 0, &bucket_payload[0]);
        buf_free(&merged0);
        merged = 1;
        if (mbuf.n + solid.n < ((size_t)8 << 20)) {
            Buf split_pay; buf_init(&split_pay);
            finalize_bucket(&solid, CODEC_XZ_9E, fast, out, 2, &split_pay);
            SPLIT_MANIFEST_XZ();
            size_t cost_merged = bucket_payload[0].n;
            size_t cost_split  = split_pay.n + (comp_m ? comp_mlen : mbuf.n);
            if (cost_split < cost_merged) {
                buf_free(&bucket_payload[0]);
                bucket_payload[0] = split_pay;
                merged = 0;
            } else {
                buf_free(&split_pay);
                free(comp_m); comp_m = NULL; comp_mlen = 0;
            }
        }
    } else if (solid.n > 0) {
        finalize_bucket(&solid, bucket_codec[0], fast, out, 0, &bucket_payload[0]);
    }
    if (!merged && !m_split_done) SPLIT_MANIFEST_XZ();
#undef SPLIT_MANIFEST_XZ

    FILE *o = fopen(out, "wb");
    if (!o) die("fopen out");
    fwrite(ZXLE_MAGIC, 1, 4, o);
    fputc(ZXLE_VER, o);
    /* Flags byte: bit 0 = bucket-0 codec (0 = xz-9e, 1 = zpaq -m5); bit 1 =
     * manifest merged into bucket 0 (no manifest block after the header --
     * the first raw_mlen bytes of decoded bucket 0 are the manifest). */
    fputc((slow ? 0x01 : 0x00) | (merged ? 0x02 : 0x00), o);
    wu32(o, (uint32_t)mbuf.n);
    wu32(o, (uint32_t)comp_mlen);
    if (!merged) {
        if (comp_m) {
            if (fwrite(comp_m, 1, comp_mlen, o) != comp_mlen) die("fwrite manifest xz");
        } else {
            if (mbuf.n > 0 && fwrite(mbuf.p, 1, mbuf.n, o) != mbuf.n) die("fwrite manifest");
        }
    }
    free(comp_m);
    buf_free(&mbuf);

    /* Trailing payload header: u8 num_buckets, then per-bucket
     * (u8 codec_id, u64 csize, csize bytes). num_buckets is 1 if no x86
     * entries, else 2. */
    int num_buckets = (solid_x86.n > 0) ? 2 : 1;
    fputc((uint8_t)num_buckets, o);
    for (int b = 0; b < num_buckets; b++) {
        fputc((uint8_t)bucket_codec[b], o);
        wu64(o, (uint64_t)bucket_payload[b].n);
        if (bucket_payload[b].n > 0)
            fwrite(bucket_payload[b].p, 1, bucket_payload[b].n, o);
    }
    fclose(o);

    for (int i = 0; i < n; i++) { buf_free(&ents[i].recipe); buf_free(&ents[i].brn); buf_free(&ents[i].pmp); }
    buf_free(&solid);
    buf_free(&solid_x86);
    buf_free(&bucket_payload[0]);
    buf_free(&bucket_payload[1]);
    free(ents);

    long long osz = fsize(out);
    if (out_size)  *out_size  = osz;
    if (out_total) *out_total = total;
    return unwrapped_count;
}

/* Run pack_run for a given (slow) tier with the inner min-pack opaque check.
 * Output lands at `out`; returns the number of unwrapped entries from the
 * primary unwrap pass (>= 0 on success). *out_osz / *out_total receive the
 * final size and input total. The caller can then compare two tiers (slow
 * vs default) and keep whichever produced the smaller blob. */
static int min_pack_for_tier(const char *out, int n, char **files, int slow,
                             int fast, long long *out_osz, uint64_t *out_total) {
    long long osz = -1; uint64_t total = 0;
    int unwrapped = pack_run(out, n, files, 0, slow, fast, &osz, &total);

    int run_opaque_pass =
        (unwrapped > 0 &&
         !(unwrapped == n && osz > 0 && (uint64_t)osz * 100 < total * 95));
    if (run_opaque_pass) {
        char opq[1024];
        snprintf(opq, sizeof(opq), "%s.opq.tmp", out);
        long long opq_osz = -1; uint64_t opq_total = 0;
        pack_run(opq, n, files, 1, slow, fast, &opq_osz, &opq_total);
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
    if (out_osz)   *out_osz   = osz;
    if (out_total) *out_total = total;
    return unwrapped;
}

/* M7 step 2: when --slow runs both the slow and default tiers (small-input
 * cross-codec min-pack), they don't depend on each other -- run them in
 * parallel and keep the smaller. Both tiers use disjoint out-path prefixes
 * so their internal temp files don't collide. */
typedef struct {
    const char *out;
    int          n;
    char       **files;
    int          slow;
    int          fast;
    long long    osz;
    uint64_t     total;
} TierJob;

static void *tier_worker(void *arg) {
    TierJob *j = (TierJob *)arg;
    min_pack_for_tier(j->out, j->n, j->files, j->slow, j->fast, &j->osz, &j->total);
    return NULL;
}

static int do_pack(int argc, char **argv) {
    /* Parse leading flags: --slow finalizes the solid stream with zpaq -m5
     * (cmix-class context-mixing) instead of xz -9e. 5-10x slower on raw
     * streams; closes the Silesia gap surfaced by the 2026-05-08 measurement. */
    int slow = 0, fast = 0;
    while (argc > 0 && argv[0][0] == '-') {
        if (strcmp(argv[0], "--slow") == 0) { slow = 1; argc--; argv++; continue; }
        if (strcmp(argv[0], "--fast") == 0) { fast = 1; argc--; argv++; continue; }
        break;
    }
    if (argc < 2) { fprintf(stderr, "usage: zxle pack [--slow] [--fast] <out.zxle> <files...>\n"); return 1; }
    const char *out = argv[0];
    int n = argc - 1;
    char **files = argv + 1;

    /* Entries are stored by basename; two inputs sharing one would silently
     * overwrite each other at unpack. Refuse up front. */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(basename_of(files[i]), basename_of(files[j])) == 0) {
                fprintf(stderr, "zxle: duplicate entry name '%s' (%s, %s)\n",
                        basename_of(files[i]), files[i], files[j]);
                return 1;
            }
        }
    }

    /* min-pack tiers:
     *   1. Primary tier: requested slow flag (xz-9e or zpaq -m5 final).
     *      Inside, runs unwrap + (optional) force_opaque, keeps smaller.
     *   2. Cross-codec tier (only when --slow on small inputs): also run
     *      default-mode and keep smaller. Closes the JAR/JPEG/MP3 +X%
     *      --slow regression on inputs where the per-stream blob is
     *      already at floor and zpaq's journaling-archive header overhead
     *      dominates the per-stream context-mixing gain. Threshold 1 MB
     *      catches all three documented cases (JAR 20 KB, JPEG 162 KB,
     *      MP3 481 KB) without bloating large-input pack time -- on big
     *      inputs --slow always wins so the second pack would be wasted. */
    long long osz; uint64_t total;

    /* M7 step 2: decide cross-codec eligibility upfront (sum input sizes via
     * stat) so we can spawn the slow + default tiers concurrently rather than
     * serially. Non-eligible runs (no --slow, or total >= 1 MB) fall through
     * to a single sequential tier as before. */
    uint64_t total_in = 0;
    if (slow) {
        for (int i = 0; i < n; i++) {
            struct stat st;
            if (stat(files[i], &st) == 0) total_in += (uint64_t)st.st_size;
        }
    }

    if (slow && total_in < (uint64_t)1024 * 1024) {
        char def_path[1024];
        snprintf(def_path, sizeof(def_path), "%s.def.tmp", out);
        TierJob slow_job = { out,      n, files, 1, fast, -1, 0 };
        TierJob def_job  = { def_path, n, files, 0, fast, -1, 0 };
        pthread_t th_slow, th_def;
        int spawn_slow = pthread_create(&th_slow, NULL, tier_worker, &slow_job);
        int spawn_def  = pthread_create(&th_def,  NULL, tier_worker, &def_job);
        if (spawn_slow != 0) tier_worker(&slow_job);
        if (spawn_def  != 0) tier_worker(&def_job);
        if (spawn_slow == 0) pthread_join(th_slow, NULL);
        if (spawn_def  == 0) pthread_join(th_def,  NULL);
        osz   = slow_job.osz;
        total = slow_job.total;
        if (def_job.osz > 0 && def_job.osz < slow_job.osz) {
            fprintf(stderr, "min-pack: default %lld < slow %lld -> using default\n",
                    def_job.osz, slow_job.osz);
            unlink(out);
            if (rename(def_path, out) != 0) die("rename def->out");
            osz = def_job.osz;
        } else {
            unlink(def_path);
        }
    } else {
        min_pack_for_tier(out, n, files, slow, fast, &osz, &total);
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
    uint32_t crc;           /* v7: crc32 of the original file bytes */
    uint8_t  kind;
    uint8_t  opaque_bucket;  /* M6 v1: which solid bucket KIND_OPAQUE bytes come from */
    uint8_t *recipe;        /* points into manifest buffer */
    uint32_t recipe_len;
    uint8_t *brn;
    uint32_t brn_len;
    uint8_t *pmp;
    uint32_t pmp_len;
} UnpackEntry;

/* Decompress one trailing-payload bucket: write the codec-input bytes from
 * `f` (current offset) of length `csize` to a temp file, run the inverse
 * codec, return the decompressed bytes. Caller frees. */
static uint8_t *decompress_bucket(FILE *f, uint64_t csize, uint8_t codec_id,
                                  const char *tmp_prefix, size_t *out_len) {
    /* Empty bucket: nothing to decompress, but allocate a 1-byte sentinel
     * so the caller's bucket_bytes[b] is non-NULL; consume zero file bytes. */
    if (csize == 0) {
        if (out_len) *out_len = 0;
        uint8_t *empty = (uint8_t *)malloc(1);
        if (!empty) die("malloc empty bucket");
        empty[0] = 0;
        return empty;
    }
    char tmp_in[1024], tmp_out[1024];
    snprintf(tmp_in,  sizeof(tmp_in),  "%s.b.in.tmp",  tmp_prefix);
    snprintf(tmp_out, sizeof(tmp_out), "%s.b.out.tmp", tmp_prefix);

    FILE *bf = fopen(tmp_in, "wb");
    if (!bf) die("fopen bucket in");
    {
        uint8_t buf[65536];
        uint64_t left = csize;
        while (left > 0) {
            size_t want = left < sizeof(buf) ? left : sizeof(buf);
            size_t got = fread(buf, 1, want, f);
            if (got == 0) die("read bucket short");
            if (fwrite(buf, 1, got, bf) != got) die("write bucket in");
            left -= got;
        }
    }
    fclose(bf);

    char cmd[4096];
    if (codec_id == CODEC_ZPAQ_M5) {
        char recdir[1024];
        snprintf(recdir, sizeof(recdir), "%s.b.zpaq.d", tmp_prefix);
        ZXLE_MKDIR(recdir);
        snprintf(cmd, sizeof(cmd),
                 "zpaq x \"%s\" -to \"%s/\" -f >%s 2>&1",
                 tmp_in, recdir, ZXLE_DEVNULL);
        run(cmd);
        char find_cmd[4096], found_meta[1024];
        snprintf(found_meta, sizeof(found_meta), "%s.b.found", tmp_prefix);
        snprintf(find_cmd, sizeof(find_cmd),
                 "find \"%s\" -type f -print -quit > \"%s\" 2>%s",
                 recdir, found_meta, ZXLE_DEVNULL);
        run(find_cmd);
        char found_path[2048] = {0};
        FILE *ff = fopen(found_meta, "rb");
        if (!ff) die("zpaq extract: no file found");
        if (fgets(found_path, sizeof(found_path), ff) == NULL) die("zpaq extract: empty find");
        fclose(ff);
        unlink(found_meta);
        size_t fpl = strlen(found_path);
        while (fpl > 0 && (found_path[fpl-1]=='\n' || found_path[fpl-1]=='\r'))
            found_path[--fpl] = 0;
        unlink(tmp_out);
        if (rename(found_path, tmp_out) != 0) die("rename zpaq extract");
        char rmcmd[2048];
        snprintf(rmcmd, sizeof(rmcmd), "rm -rf \"%s\" >%s 2>&1", recdir, ZXLE_DEVNULL);
        run(rmcmd);
    } else {
        /* CODEC_XZ_9E and CODEC_XZ_9E_X86 both decode via plain `xz -d` --
         * the BCJ filter is recorded in the xz block header so the decoder
         * applies it automatically. */
        snprintf(cmd, sizeof(cmd),
                 "xz -d -c \"%s\" > \"%s\" 2>%s",
                 tmp_in, tmp_out, ZXLE_DEVNULL);
        run(cmd);
    }

    size_t plen = 0;
    uint8_t *bytes = read_whole_file(tmp_out, &plen);
    unlink(tmp_in);
    unlink(tmp_out);
    if (out_len) *out_len = plen;
    return bytes;
}

/* v7: verify a reconstructed entry against the manifest crc32. Catches both
 * payload corruption in the unprotected manifest region and silent
 * reproduction drift (e.g. a decode-side xz/zstd/bzip2 version whose
 * re-encode differs from the pack machine's). */
static void verify_entry(const char *path, uint32_t want_crc, uint64_t want_size) {
    size_t got_n = 0;
    uint8_t *got = read_whole_file(path, &got_n);
    uint32_t c = crc32_buf(got, got_n);
    free(got);
    if ((uint64_t)got_n != want_size) {
        fprintf(stderr, "zxle: %s: size %llu != %llu\n", path,
                (unsigned long long)got_n, (unsigned long long)want_size);
        die("entry size mismatch");
    }
    if (c != want_crc) {
        fprintf(stderr, "zxle: %s\n", path);
        die("entry crc mismatch");
    }
}

static int do_unpack(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: zxle unpack <in.zxle> <outdir>\n"); return 1; }
    const char *in = argv[0];
    const char *outdir = argv[1];

    FILE *f = fopen(in, "rb");
    if (!f) die("fopen in");
    char magic[4];
    if (fread(magic,1,4,f) != 4 || memcmp(magic, ZXLE_MAGIC, 4) != 0) die("bad magic");
    int ver = fgetc(f), flags = fgetc(f);
    if (ver != ZXLE_VER) die("bad version");
    /* flags bit 0 is informational here (codec is per-bucket in the trailing
     * payload header); bit 1 = manifest merged into bucket 0. */
    int merged = (flags & 0x02) != 0;

    /* v7 header: u32 raw_mlen + u32 comp_mlen. Split mode: a manifest block
     * follows (comp_mlen==0 -> stored raw, else xz decompressing to
     * raw_mlen). Merged mode: no block here -- the first raw_mlen bytes of
     * decoded bucket 0 are the manifest. */
    uint8_t mlen_b[8];
    if (fread(mlen_b, 1, 8, f) != 8) die("read mlen");
    uint32_t mlen      = r32(mlen_b);
    uint32_t comp_mlen = r32(mlen_b + 4);
    uint8_t *manifest = NULL;
    if (!merged) {
        if (comp_mlen == 0) {
            manifest = malloc(mlen ? mlen : 1);
            if (!manifest) die("malloc manifest");
            if (mlen > 0 && fread(manifest, 1, mlen, f) != mlen) die("read manifest");
        } else {
            char m_xz[1024], m_raw[1024], mcmd[4096];
            snprintf(m_xz,  sizeof(m_xz),  "%s.mf.xz.tmp",  in);
            snprintf(m_raw, sizeof(m_raw), "%s.mf.raw.tmp", in);
            uint8_t *comp = malloc(comp_mlen);
            if (!comp) die("malloc manifest xz");
            if (fread(comp, 1, comp_mlen, f) != comp_mlen) die("read manifest xz");
            FILE *mf = fopen(m_xz, "wb");
            if (!mf) die("fopen manifest xz tmp");
            if (fwrite(comp, 1, comp_mlen, mf) != comp_mlen) die("fwrite manifest xz tmp");
            fclose(mf);
            free(comp);
            snprintf(mcmd, sizeof(mcmd), "xz -d -c \"%s\" > \"%s\" 2>%s",
                     m_xz, m_raw, ZXLE_DEVNULL);
            run(mcmd);
            size_t got = 0;
            manifest = read_whole_file(m_raw, &got);
            unlink(m_xz);
            unlink(m_raw);
            if (got != mlen) die("manifest size mismatch");
        }
    }

    /* Read the trailing-payload bucket header: u8 num_buckets, then
     * per-bucket (u8 codec_id, u64 csize, csize bytes). Decompress each
     * bucket into its own buffer; KIND_OPAQUE entries dispatch by their
     * opaque_bucket field, other kinds always read from bucket 0. */
    int num_buckets = fgetc(f);
    if (num_buckets <= 0 || num_buckets > 2) die("invalid num_buckets");
    uint8_t *bucket_bytes[2] = {0};
    size_t bucket_len[2] = {0};
    for (int b = 0; b < num_buckets; b++) {
        int codec_id = fgetc(f);
        if (codec_id < 0) die("codec_id eof");
        uint8_t cs[8];
        if (fread(cs, 1, 8, f) != 8) die("csize eof");
        uint64_t csize = (uint64_t)r32(cs) | ((uint64_t)r32(cs + 4) << 32);
        char tprefix[1024];
        snprintf(tprefix, sizeof(tprefix), "%s.bucket%d", in, b);
        bucket_bytes[b] = decompress_bucket(f, csize, (uint8_t)codec_id, tprefix, &bucket_len[b]);
        if (!bucket_bytes[b]) die("decompress bucket");
    }
    fclose(f);

    /* Merged mode: manifest is the head of decoded bucket 0; copy it out so
     * ownership matches the split path, and offset the solid view past it. */
    if (merged) {
        if ((uint64_t)mlen > bucket_len[0]) die("merged manifest overflow");
        manifest = malloc(mlen ? mlen : 1);
        if (!manifest) die("malloc manifest");
        memcpy(manifest, bucket_bytes[0], mlen);
    }

    int count = 0, cap = 0;
    UnpackEntry *ents = NULL;
    size_t mp = 0;
    while (mp < mlen) {
        if (count == cap) { cap = cap ? cap*2 : 16; ents = realloc(ents, (size_t)cap * sizeof(UnpackEntry)); if (!ents) die("realloc"); }
        if (mp + 2 > mlen) die("manifest truncated");
        uint16_t pl = r16(manifest + mp); mp += 2;
        if (pl >= sizeof(ents[0].name) || mp + pl + 8 + 4 + 4 + 1 > mlen) die("manifest overflow");
        memcpy(ents[count].name, manifest + mp, pl); ents[count].name[pl] = 0; mp += pl;
        /* Entries are basenames by construction; a separator, drive colon, or
         * ".." in a name means a crafted archive trying to escape outdir. */
        if (pl == 0 || strchr(ents[count].name, '/') || strchr(ents[count].name, '\\') ||
            strchr(ents[count].name, ':') || strcmp(ents[count].name, ".") == 0 ||
            strcmp(ents[count].name, "..") == 0) {
            fprintf(stderr, "zxle: bad entry name '%s'\n", ents[count].name);
            die("unsafe entry name");
        }
        ents[count].orig_size = (uint64_t)r32(manifest + mp) | ((uint64_t)r32(manifest + mp + 4) << 32); mp += 8;
        ents[count].mode = r32(manifest + mp); mp += 4;
        ents[count].crc  = r32(manifest + mp); mp += 4;
        ents[count].kind = manifest[mp]; mp += 1;
        ents[count].opaque_bucket = 0;
        if (ents[count].kind == KIND_OPAQUE) {
            if (mp + 1 > mlen) die("opaque_bucket truncated");
            ents[count].opaque_bucket = manifest[mp]; mp += 1;
        }
        ents[count].recipe = NULL; ents[count].recipe_len = 0;
        ents[count].brn    = NULL; ents[count].brn_len    = 0;
        ents[count].pmp    = NULL; ents[count].pmp_len    = 0;

        uint8_t k = ents[count].kind;
        int has_recipe = (k == KIND_ZIP || k == KIND_PNG || k == KIND_GZIP ||
                          k == KIND_TAR || k == KIND_AR  || k == KIND_BZIP2 ||
                          k == KIND_ZSTD || k == KIND_XZ || k == KIND_PDF);
        if (has_recipe) {
            if (mp + 4 > mlen) die("recipe len truncated");
            ents[count].recipe_len = r32(manifest + mp); mp += 4;
            if (mp + ents[count].recipe_len > mlen) die("recipe overflow");
            ents[count].recipe = manifest + mp;
            mp += ents[count].recipe_len;
        } else if (k == KIND_JPEG) {
            if (mp + 4 > mlen) die("brn len truncated");
            ents[count].brn_len = r32(manifest + mp); mp += 4;
            if (mp + ents[count].brn_len > mlen) die("brn overflow");
            ents[count].brn = manifest + mp;
            mp += ents[count].brn_len;
        } else if (k == KIND_MP3) {
            if (mp + 4 > mlen) die("pmp len truncated");
            ents[count].pmp_len = r32(manifest + mp); mp += 4;
            if (mp + ents[count].pmp_len > mlen) die("pmp overflow");
            ents[count].pmp = manifest + mp;
            mp += ents[count].pmp_len;
        }
        count++;
    }

    if (ZXLE_MKDIR(outdir) != 0 && errno != EEXIST) die("mkdir outdir");

    /* M6 v3: build a Solids snapshot pointing at each decoded bucket; recipes
     * carry per-OP bucket bytes that the walker dispatches on. KIND_OPAQUE
     * dispatches at the manifest level (single opaque_bucket per entry).
     * Merged mode: bucket 0's solid bytes start after the manifest prefix. */
    size_t off0 = merged ? mlen : 0;
    Solids sol;
    for (int b = 0; b < ZXLE_NUM_BUCKETS; b++) {
        size_t off = (b == 0) ? off0 : 0;
        sol.p[b]   = b < num_buckets ? bucket_bytes[b] + off : NULL;
        sol.len[b] = b < num_buckets ? bucket_len[b] - off   : 0;
        sol.pos[b] = 0;
    }
    for (int i = 0; i < count; i++) {
        char p[2048];
        snprintf(p, sizeof(p), "%s/%s", outdir, ents[i].name);
        FILE *of = fopen(p, "wb");
        if (!of) { fprintf(stderr, "fopen %s\n", p); die("fopen out"); }
        if (ents[i].kind == KIND_OPAQUE) {
            int b = ents[i].opaque_bucket;
            if (b >= num_buckets) die("opaque_bucket out of range");
            if (sol.pos[b] + ents[i].orig_size > sol.len[b]) die("opaque overflow");
            if (ents[i].orig_size > 0 &&
                fwrite(sol.p[b] + sol.pos[b], 1, ents[i].orig_size, of) != ents[i].orig_size)
                die("fwrite opaque");
            sol.pos[b] += ents[i].orig_size;
            fclose(of);
        } else if (ents[i].kind == KIND_ZIP || ents[i].kind == KIND_TAR ||
                   ents[i].kind == KIND_AR  || ents[i].kind == KIND_PDF) {
            unpack_recipe(ents[i].recipe, ents[i].recipe_len, &sol, of, ents[i].orig_size, p);
            fclose(of);
        } else if (ents[i].kind == KIND_JPEG) {
            size_t jn = 0;
            uint8_t *jpg = unpack_jpeg_blob(ents[i].brn, ents[i].brn_len, p, &jn);
            if (jn > 0 && fwrite(jpg, 1, jn, of) != jn) die("fwrite jpeg out");
            free(jpg);
            fclose(of);
        } else if (ents[i].kind == KIND_PNG) {
            unpack_png(ents[i].recipe, ents[i].recipe_len, &sol, of, ents[i].orig_size);
            fclose(of);
        } else if (ents[i].kind == KIND_GZIP) {
            unpack_gz(ents[i].recipe, ents[i].recipe_len, &sol, of, ents[i].orig_size, p);
            fclose(of);
        } else if (ents[i].kind == KIND_BZIP2) {
            unpack_bz2(ents[i].recipe, ents[i].recipe_len, &sol, of, ents[i].orig_size, p);
            fclose(of);
        } else if (ents[i].kind == KIND_ZSTD) {
            unpack_zst(ents[i].recipe, ents[i].recipe_len, &sol, of, ents[i].orig_size, p);
            fclose(of);
        } else if (ents[i].kind == KIND_XZ) {
            unpack_xz(ents[i].recipe, ents[i].recipe_len, &sol, of, ents[i].orig_size, p);
            fclose(of);
        } else if (ents[i].kind == KIND_MP3) {
            fclose(of);
            char tmp_pmp[2048], tmp_mp3[2048], cmd2[4096];
            snprintf(tmp_pmp, sizeof(tmp_pmp), "%s.pmp.tmp.pmp", p);
            snprintf(tmp_mp3, sizeof(tmp_mp3), "%s.pmp.tmp.mp3", p);
            FILE *bf = fopen(tmp_pmp, "wb");
            if (!bf) die("fopen tmp pmp");
            if (ents[i].pmp_len > 0 && fwrite(ents[i].pmp, 1, ents[i].pmp_len, bf) != ents[i].pmp_len) die("fwrite tmp pmp");
            fclose(bf);
            snprintf(cmd2, sizeof(cmd2), "packMP3 -o -np \"%s\" >%s 2>&1", tmp_pmp, ZXLE_DEVNULL);
            run(cmd2);
            unlink(tmp_pmp);
            unlink(p);
            if (rename(tmp_mp3, p) != 0) die("rename mp3 out");
        } else {
            die("unknown kind");
        }
        verify_entry(p, ents[i].crc, ents[i].orig_size);
    }
    for (int b = 0; b < num_buckets; b++) {
        if (sol.pos[b] != sol.len[b]) die("bucket stream not fully consumed");
        free(bucket_bytes[b]);
    }

    free(manifest);
    free(ents);

    fprintf(stderr, "unpacked %d file(s) to %s\n", count, outdir);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "ZXL-E\n"
        "  zxle pack   <out.zxle> <files...>\n"
        "  zxle unpack <in.zxle>  <outdir>\n");
}

/* Auto-discover third_party/ tool dirs that sit alongside the zxle binary,
 * and prepend them to PATH so direct invocation works after `make all-deps`
 * without the user needing to manage PATH manually. Mirrors the same logic
 * tests/bench.sh does. No-op if argv[0] doesn't resolve to a real dirname. */
static void prepend_third_party_to_path(const char *argv0) {
    char buf[2048];
    if (!argv0) return;
    size_t n = strnlen(argv0, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return;
    memcpy(buf, argv0, n + 1);
    /* Find last slash or backslash. */
    char *slash = NULL;
    for (size_t i = 0; i < n; i++) if (buf[i]=='/' || buf[i]=='\\') slash = &buf[i];
    if (!slash) return;
    *slash = 0; /* buf is now the bin's directory. */
    static const char *subdirs[] = {
        "third_party/brunsli/build/artifacts",
        "third_party/packmp3/source",
        "third_party/packjpg/source",
        "third_party/zpaq",
    };
    for (size_t i = 0; i < sizeof(subdirs)/sizeof(subdirs[0]); i++) {
        char candidate[2048];
        snprintf(candidate, sizeof(candidate), "%s/%s", buf, subdirs[i]);
        struct stat st;
        if (stat(candidate, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        const char *cur = getenv("PATH");
        char newpath[8192];
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        if (cur && cur[0]) snprintf(newpath, sizeof(newpath), "%s%c%s", candidate, sep, cur);
        else               snprintf(newpath, sizeof(newpath), "%s", candidate);
#ifdef _WIN32
        _putenv_s("PATH", newpath);
#else
        setenv("PATH", newpath, 1);
#endif
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    prepend_third_party_to_path(argv[0]);
    if (strcmp(argv[1], "pack")   == 0) return do_pack  (argc - 2, argv + 2);
    if (strcmp(argv[1], "unpack") == 0) return do_unpack(argc - 2, argv + 2);
    usage();
    return 1;
}
