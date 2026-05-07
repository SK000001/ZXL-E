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
#include "recipe.h"

typedef struct {
    const char *path;
    const char *name;
    uint64_t    orig_size;
    uint32_t    mode;
    uint8_t     kind;
    Buf         recipe;     /* most kinds */
    Buf         brn;        /* KIND_JPEG */
    Buf         pmp;        /* KIND_MP3  */
} PackEntry;

/* pack_run — main pack body. force_opaque=1 skips all container-unwrap routing
 * and stores every input as KIND_OPAQUE. slow=1 finalizes the solid stream
 * with zpaq -m5 instead of xz -9e (cmix-class context-mixing on raw streams;
 * 5-10x slower). Used by do_pack() to compute a fall-through baseline; see
 * "min-pack" comment in do_pack. Returns the number of files unwrapped (i.e.
 * anything other than KIND_OPAQUE) on success. *out_size receives the
 * produced file size. */
static int pack_run(const char *out, int n, char **files, int force_opaque,
                    int slow, long long *out_size, uint64_t *out_total) {
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
        if (!force_opaque && !unwrapped && fsz >= 12 &&
            fb[0]==0xFD && fb[1]==0x37 && fb[2]==0x7A && fb[3]==0x58 && fb[4]==0x5A && fb[5]==0x00) {
            char tp[1024];
            snprintf(tp, sizeof(tp), "%s.%d", out, i);
            if (pack_xz(fb, fsz, tp, &ents[i].recipe, &solid) == 0) {
                ents[i].kind = KIND_XZ;
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

    char tmp_concat[1024], tmp_zst[1024];
    snprintf(tmp_concat, sizeof(tmp_concat), "%s.concat.tmp", out);
    snprintf(tmp_zst,    sizeof(tmp_zst),    "%s.zst.tmp",    out);

    FILE *cf = fopen(tmp_concat, "wb");
    if (!cf) die("fopen concat");
    if (solid.n > 0 && fwrite(solid.p, 1, solid.n, cf) != solid.n) die("fwrite solid");
    fclose(cf);

    char cmd[4096];
    if (slow) {
        /* zpaq -m5: cmix-class context-mixing. Archive is journaling-format
         * (not a stream); zpaq creates tmp_zst itself and we read it back
         * verbatim into the trailing payload. */
        unlink(tmp_zst);
        snprintf(cmd, sizeof(cmd),
                 "zpaq a \"%s\" \"%s\" -m5 >%s 2>&1",
                 tmp_zst, tmp_concat, ZXLE_DEVNULL);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "xz -9e -c --threads=1 \"%s\" > \"%s\" 2>%s",
                 tmp_concat, tmp_zst, ZXLE_DEVNULL);
    }
    run(cmd);

    /* Compute manifest size. KIND_OPAQUE has no recipe blob; every other kind
     * carries `(u32 len)(len bytes)` after the kind byte. */
    size_t mlen = 0;
    for (int i = 0; i < n; i++) {
        mlen += 2 + strlen(ents[i].name) + 8 + 4 + 1;
        if (ents[i].kind == KIND_ZIP)   mlen += 4 + ents[i].recipe.n;
        if (ents[i].kind == KIND_JPEG)  mlen += 4 + ents[i].brn.n;
        if (ents[i].kind == KIND_MP3)   mlen += 4 + ents[i].pmp.n;
        if (ents[i].kind == KIND_PNG)   mlen += 4 + ents[i].recipe.n;
        if (ents[i].kind == KIND_GZIP)  mlen += 4 + ents[i].recipe.n;
        if (ents[i].kind == KIND_TAR)   mlen += 4 + ents[i].recipe.n;
        if (ents[i].kind == KIND_AR)    mlen += 4 + ents[i].recipe.n;
        if (ents[i].kind == KIND_BZIP2) mlen += 4 + ents[i].recipe.n;
        if (ents[i].kind == KIND_ZSTD)  mlen += 4 + ents[i].recipe.n;
        if (ents[i].kind == KIND_XZ)    mlen += 4 + ents[i].recipe.n;
    }

    FILE *o = fopen(out, "wb");
    if (!o) die("fopen out");
    fwrite(ZXLE_MAGIC, 1, 4, o);
    fputc(ZXLE_VER, o);
    /* Flags byte: bit 0 = trailing payload codec (0 = xz-9e, 1 = zpaq -m5). */
    fputc(slow ? 0x01 : 0x00, o);
    wu32(o, (uint32_t)mlen);
    for (int i = 0; i < n; i++) {
        size_t plen = strlen(ents[i].name);
        wu16(o, (uint16_t)plen);
        fwrite(ents[i].name, 1, plen, o);
        wu64(o, ents[i].orig_size);
        wu32(o, ents[i].mode);
        fputc(ents[i].kind, o);

        const Buf *blob = NULL;
        switch (ents[i].kind) {
        case KIND_ZIP:
        case KIND_PNG:
        case KIND_GZIP:
        case KIND_TAR:
        case KIND_AR:
        case KIND_BZIP2:
        case KIND_ZSTD:
        case KIND_XZ:    blob = &ents[i].recipe; break;
        case KIND_JPEG:  blob = &ents[i].brn;    break;
        case KIND_MP3:   blob = &ents[i].pmp;    break;
        default: break;
        }
        if (blob) {
            wu32(o, (uint32_t)blob->n);
            if (blob->n > 0) fwrite(blob->p, 1, blob->n, o);
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

/* Run pack_run for a given (slow) tier with the inner min-pack opaque check.
 * Output lands at `out`; returns the number of unwrapped entries from the
 * primary unwrap pass (>= 0 on success). *out_osz / *out_total receive the
 * final size and input total. The caller can then compare two tiers (slow
 * vs default) and keep whichever produced the smaller blob. */
static int min_pack_for_tier(const char *out, int n, char **files, int slow,
                             long long *out_osz, uint64_t *out_total) {
    long long osz = -1; uint64_t total = 0;
    int unwrapped = pack_run(out, n, files, 0, slow, &osz, &total);

    int run_opaque_pass =
        (unwrapped > 0 &&
         !(unwrapped == n && osz > 0 && (uint64_t)osz * 100 < total * 95));
    if (run_opaque_pass) {
        char opq[1024];
        snprintf(opq, sizeof(opq), "%s.opq.tmp", out);
        long long opq_osz = -1; uint64_t opq_total = 0;
        pack_run(opq, n, files, 1, slow, &opq_osz, &opq_total);
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

static int do_pack(int argc, char **argv) {
    /* Parse leading flags: --slow finalizes the solid stream with zpaq -m5
     * (cmix-class context-mixing) instead of xz -9e. 5-10x slower on raw
     * streams; closes the Silesia gap surfaced by the 2026-05-08 measurement. */
    int slow = 0;
    while (argc > 0 && argv[0][0] == '-') {
        if (strcmp(argv[0], "--slow") == 0) { slow = 1; argc--; argv++; continue; }
        break;
    }
    if (argc < 2) { fprintf(stderr, "usage: zxle pack [--slow] <out.zxle> <files...>\n"); return 1; }
    const char *out = argv[0];
    int n = argc - 1;
    char **files = argv + 1;

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
    min_pack_for_tier(out, n, files, slow, &osz, &total);

    if (slow && total < (uint64_t)1024 * 1024) {
        char def_path[1024];
        snprintf(def_path, sizeof(def_path), "%s.def.tmp", out);
        long long def_osz; uint64_t def_total;
        min_pack_for_tier(def_path, n, files, 0, &def_osz, &def_total);
        if (def_osz > 0 && def_osz < osz) {
            fprintf(stderr, "min-pack: default %lld < slow %lld -> using default\n",
                    def_osz, osz);
            unlink(out);
            if (rename(def_path, out) != 0) die("rename def->out");
            osz = def_osz;
        } else {
            unlink(def_path);
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
    uint8_t *brn;
    uint32_t brn_len;
    uint8_t *pmp;
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
    if (ver != ZXLE_VER) die("bad version");
    int slow = (flags & 0x01);

    uint8_t mlen_b[4];
    if (fread(mlen_b, 1, 4, f) != 4) die("read mlen");
    uint32_t mlen = r32(mlen_b);
    uint8_t *manifest = malloc(mlen ? mlen : 1);
    if (!manifest) die("malloc manifest");
    if (mlen > 0 && fread(manifest, 1, mlen, f) != mlen) die("read manifest");

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

        uint8_t k = ents[count].kind;
        int has_recipe = (k == KIND_ZIP || k == KIND_PNG || k == KIND_GZIP ||
                          k == KIND_TAR || k == KIND_AR  || k == KIND_BZIP2 ||
                          k == KIND_ZSTD || k == KIND_XZ);
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
    if (slow) {
        /* zpaq archive: extract via `zpaq x` to a scratch dir, then read the
         * single file out. zpaq stores the path as given (we passed
         * tmp_concat which is e.g. "<in>.unpack.concat.tmp"); on extract it
         * lands at <recdir>/<resolved-tmp_concat-path>, which we resolve by
         * scanning for the first regular file under recdir. */
        char recdir[1024];
        snprintf(recdir, sizeof(recdir), "%s.unpack.zpaq.d", in);
        ZXLE_MKDIR(recdir);
        snprintf(cmd, sizeof(cmd),
                 "zpaq x \"%s\" -to \"%s/\" -f >%s 2>&1",
                 tmp_zst, recdir, ZXLE_DEVNULL);
        run(cmd);
        /* Move the extracted file (at some nested path) to tmp_concat. We
         * know it's the only regular file under recdir, since the archive
         * was created with one input. Use `find` shell-out for portability
         * across the path quirks we already hit in the bench. */
        char find_cmd[4096];
        snprintf(find_cmd, sizeof(find_cmd),
                 "find \"%s\" -type f -print -quit > \"%s.found\" 2>%s",
                 recdir, tmp_concat, ZXLE_DEVNULL);
        run(find_cmd);
        char found_path[2048] = {0};
        char found_meta[2048];
        snprintf(found_meta, sizeof(found_meta), "%s.found", tmp_concat);
        FILE *ff = fopen(found_meta, "rb");
        if (!ff) die("zpaq extract: no file found");
        if (fgets(found_path, sizeof(found_path), ff) == NULL) die("zpaq extract: empty find");
        fclose(ff);
        unlink(found_meta);
        size_t fpl = strlen(found_path);
        while (fpl > 0 && (found_path[fpl-1]=='\n' || found_path[fpl-1]=='\r'))
            found_path[--fpl] = 0;
        unlink(tmp_concat);
        if (rename(found_path, tmp_concat) != 0) die("rename zpaq extract -> tmp_concat");
        /* Best-effort cleanup of the empty dir tree zpaq left behind. */
        char rmcmd[2048];
        snprintf(rmcmd, sizeof(rmcmd), "rm -rf \"%s\" >%s 2>&1", recdir, ZXLE_DEVNULL);
        run(rmcmd);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "xz -d -c \"%s\" > \"%s\" 2>%s",
                 tmp_zst, tmp_concat, ZXLE_DEVNULL);
        run(cmd);
    }

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
        } else if (ents[i].kind == KIND_ZIP || ents[i].kind == KIND_TAR || ents[i].kind == KIND_AR) {
            unpack_recipe(ents[i].recipe, ents[i].recipe_len,
                          solid, solid_len, &solid_pos,
                          of, ents[i].orig_size, p);
            fclose(of);
        } else if (ents[i].kind == KIND_JPEG) {
            fclose(of);
            char tmp_brn[2048], cmd2[4096];
            snprintf(tmp_brn, sizeof(tmp_brn), "%s.brn.tmp", p);
            FILE *bf = fopen(tmp_brn, "wb");
            if (!bf) die("fopen tmp brn");
            if (ents[i].brn_len > 0 && fwrite(ents[i].brn, 1, ents[i].brn_len, bf) != ents[i].brn_len) die("fwrite tmp brn");
            fclose(bf);
            snprintf(cmd2, sizeof(cmd2), "dbrunsli \"%s\" \"%s\" >%s 2>&1", tmp_brn, p, ZXLE_DEVNULL);
            run(cmd2);
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
        } else if (ents[i].kind == KIND_XZ) {
            unpack_xz(ents[i].recipe, ents[i].recipe_len,
                      solid, solid_len, &solid_pos,
                      of, ents[i].orig_size, p);
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
