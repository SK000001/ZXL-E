#include "tar.h"
#include "kinds.h"
#include "png.h"
#include "jpeg.h"
#include "gz.h"
#include "bz2.h"
#include "xz.h"
#include "zst.h"
#include "zip.h"
#include "pdf.h"
#include <pthread.h>

/* M7 step 5 (tar): headers are parsed and validated up front, entry payloads
 * run on a worker pool into per-entry fragments, then everything is spliced
 * in entry order -- output byte-identical to the serial walk. Because all
 * validation happens before any append, a malformed tar can no longer touch
 * the shared buckets at all. */

typedef struct {
    size_t hdr_off;   /* offset of the 512-byte header */
    uint64_t size;    /* payload bytes */
    uint64_t padded;  /* payload rounded up to 512 */
    int is_regular;
} TarEnt;

typedef struct {
    Buf frag;   /* recipe ops for this entry's payload */
    Buf f0, f1; /* solid-bucket fragments */
    int jpeg_stored, png_stored, gzip_stored, bz2_stored;
    int xz_stored, zstd_stored, zip_stored, pdf_stored, stored_plain;
} TarEntOut;

static void tar_entry_payload(const uint8_t *p, const char *tmp_prefix,
                              const TarEnt *e, TarEntOut *o) {
    if (e->size == 0) return;
    size_t cur = e->hdr_off + 512;
    uint64_t size = e->size;
    int is_regular = e->is_regular;
    Buf *recipe = &o->frag, *b0 = &o->f0, *b1 = &o->f1;
    int handled = 0;

    if (is_regular && size >= 8 && memcmp(p + cur, PNG_SIG, 8) == 0) {
        Buf png_recipe; buf_init(&png_recipe);
        if (pack_png(p + cur, (size_t)size, &png_recipe, b0) == 0) {
            buf_u8(recipe, OP_PNG_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)png_recipe.n);
            buf_append(recipe, png_recipe.p, png_recipe.n);
            o->png_stored++;
            handled = 1;
        }
        buf_free(&png_recipe);
    }
    if (!handled && is_regular && size >= 4 &&
        p[cur] == 0xFF && p[cur+1] == 0xD8 && p[cur+2] == 0xFF) {
        char tp[2048];
        snprintf(tp, sizeof(tp), "%s.tj.%zu", tmp_prefix, cur);
        Buf brn; buf_init(&brn);
        if (try_jpeg_buf(p + cur, (size_t)size, tp, &brn) == 0) {
            buf_u8(recipe, OP_JPEG_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)brn.n);
            buf_append(recipe, brn.p, brn.n);
            o->jpeg_stored++;
            handled = 1;
        }
        buf_free(&brn);
    }
    if (!handled && is_regular && size >= 22 &&
        p[cur] == 0x50 && p[cur+1] == 0x4B) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.tzip.%zu", tmp_prefix, cur);
        Buf zip_recipe; buf_init(&zip_recipe);
        if (pack_zip(p + cur, (size_t)size, tp, &zip_recipe, b0, b1) == 0) {
            buf_u8(recipe, OP_ZIP_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)zip_recipe.n);
            buf_append(recipe, zip_recipe.p, zip_recipe.n);
            o->zip_stored++;
            handled = 1;
        }
        buf_free(&zip_recipe);
    }
    if (!handled && is_regular && size >= 32 &&
        memcmp(p + cur, "%PDF-", 5) == 0) {
        /* Nested PDF recipe shares the generic OP vocabulary, so it
         * rides OP_ZIP_STORE (recurse-unpack_recipe semantics). */
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.tpdf.%zu", tmp_prefix, cur);
        Buf pdf_recipe; buf_init(&pdf_recipe);
        if (pack_pdf(p + cur, (size_t)size, tp, &pdf_recipe, b0, b1) == 0) {
            buf_u8(recipe, OP_ZIP_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)pdf_recipe.n);
            buf_append(recipe, pdf_recipe.p, pdf_recipe.n);
            o->pdf_stored++;
            handled = 1;
        }
        buf_free(&pdf_recipe);
    }
    if (!handled && is_regular && size >= 18 &&
        p[cur] == 0x1F && p[cur+1] == 0x8B && p[cur+2] == 0x08) {
        /* Per-entry sniff: assume gzip-of-PE if entry filename ends in
         * a PE extension (the cheap check); pack_gz routes inflated
         * body to b[bucket]. */
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.tgz.%zu", tmp_prefix, cur);
        Buf gz_recipe; buf_init(&gz_recipe);
        if (pack_gz(p + cur, (size_t)size, tp, &gz_recipe, b0, b1, 0) == 0) {
            buf_u8(recipe, OP_GZIP_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)gz_recipe.n);
            buf_append(recipe, gz_recipe.p, gz_recipe.n);
            o->gzip_stored++;
            handled = 1;
        }
        buf_free(&gz_recipe);
    }
    if (!handled && is_regular && size >= 14 &&
        p[cur] == 'B' && p[cur+1] == 'Z' && p[cur+2] == 'h' &&
        p[cur+3] >= '1' && p[cur+3] <= '9') {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.tbz.%zu", tmp_prefix, cur);
        Buf bz_recipe; buf_init(&bz_recipe);
        if (pack_bz2(p + cur, (size_t)size, tp, &bz_recipe, b0, b1, 0) == 0) {
            buf_u8(recipe, OP_BZ2_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)bz_recipe.n);
            buf_append(recipe, bz_recipe.p, bz_recipe.n);
            o->bz2_stored++;
            handled = 1;
        }
        buf_free(&bz_recipe);
    }
    if (!handled && is_regular && size >= 12 &&
        p[cur] == 0xFD && p[cur+1] == 0x37 && p[cur+2] == 0x7A &&
        p[cur+3] == 0x58 && p[cur+4] == 0x5A && p[cur+5] == 0x00) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.txz.%zu", tmp_prefix, cur);
        Buf xz_recipe; buf_init(&xz_recipe);
        if (pack_xz(p + cur, (size_t)size, tp, &xz_recipe, b0, b1, 0) == 0) {
            buf_u8(recipe, OP_XZ_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)xz_recipe.n);
            buf_append(recipe, xz_recipe.p, xz_recipe.n);
            o->xz_stored++;
            handled = 1;
        }
        buf_free(&xz_recipe);
    }
    if (!handled && is_regular && size >= 8 &&
        p[cur] == 0x28 && p[cur+1] == 0xB5 &&
        p[cur+2] == 0x2F && p[cur+3] == 0xFD) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.tzs.%zu", tmp_prefix, cur);
        Buf zs_recipe; buf_init(&zs_recipe);
        if (pack_zst(p + cur, (size_t)size, tp, &zs_recipe, b0, b1, 0) == 0) {
            buf_u8(recipe, OP_ZSTD_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)zs_recipe.n);
            buf_append(recipe, zs_recipe.p, zs_recipe.n);
            o->zstd_stored++;
            handled = 1;
        }
        buf_free(&zs_recipe);
    }
    if (!handled) {
        uint8_t bk = bucket_for_bytes(p + cur, (size_t)size);
        buf_u8(recipe, OP_STORE);
        buf_u32(recipe, (uint32_t)size);
        buf_u8(recipe, bk);
        buf_append(bk == 1 ? b1 : b0, p + cur, (size_t)size);
        o->stored_plain++;
    }
}

typedef struct {
    const uint8_t *p;
    const char *tmp_prefix;
    const TarEnt *ents;
    size_t count;
    TarEntOut *outs;
    long next;
} TarPool;

static void *tar_pool_worker(void *arg) {
    TarPool *pl = (TarPool *)arg;
    for (;;) {
        long i = __atomic_fetch_add(&pl->next, 1, __ATOMIC_SEQ_CST);
        if (i >= (long)pl->count) return NULL;
        tar_entry_payload(pl->p, pl->tmp_prefix, &pl->ents[i], &pl->outs[i]);
    }
}

int pack_tar(const uint8_t *p, size_t n, const char *tmp_prefix,
             Buf *recipe, Buf *b0, Buf *b1) {
    if (n < 1024) return -1;
    if (n % 512 != 0) return -1;
    if (memcmp(p + 257, "ustar", 5) != 0) return -1;

    /* Parse phase: validate every header before any output is produced. */
    TarEnt *ents = NULL;
    size_t count = 0, cap = 0;
    size_t cur = 0, tail_off = n;

    while (cur + 512 <= n) {
        const uint8_t *hdr = p + cur;

        int is_zero = 1;
        for (int i = 0; i < 512; i++) if (hdr[i]) { is_zero = 0; break; }
        if (is_zero) { tail_off = cur; break; }

        if (memcmp(hdr + 257, "ustar", 5) != 0) { free(ents); return -1; }
        if (hdr[124] & 0x80) { free(ents); return -1; }

        uint64_t size = 0;
        for (int i = 124; i < 124 + 11; i++) {
            uint8_t c = hdr[i];
            if (c == 0 || c == ' ') break;
            if (c < '0' || c > '7') { free(ents); return -1; }
            size = size * 8 + (c - '0');
        }
        if (size > 0xFFFFFFFFu) { free(ents); return -1; }

        char tflag = (char)hdr[156];
        uint64_t padded = (size + 511) & ~(uint64_t)511;
        if (cur + 512 + padded > n) { free(ents); return -1; }

        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            TarEnt *ne = realloc(ents, cap * sizeof(TarEnt));
            if (!ne) { free(ents); return -1; }
            ents = ne;
        }
        ents[count].hdr_off = cur;
        ents[count].size = size;
        ents[count].padded = padded;
        ents[count].is_regular = (tflag == '0' || tflag == 0);
        count++;
        cur += 512 + padded;
    }

    TarEntOut *outs = calloc(count ? count : 1, sizeof(TarEntOut));
    if (!outs) { free(ents); return -1; }
    for (size_t i = 0; i < count; i++) {
        buf_init(&outs[i].frag); buf_init(&outs[i].f0); buf_init(&outs[i].f1);
    }
    TarPool pl = { p, tmp_prefix, ents, count, outs, 0 };
    int nth = zxle_ncpus();
    if (nth > (int)count) nth = (int)count;
    if (nth > 64) nth = 64;
    if (nth > 1) {
        pthread_t th[64];
        for (int t = 0; t < nth; t++) pthread_create(&th[t], NULL, tar_pool_worker, &pl);
        for (int t = 0; t < nth; t++) pthread_join(th[t], NULL);
    } else {
        tar_pool_worker(&pl);
    }

    int regulars = 0, jpeg_stored = 0, png_stored = 0, gzip_stored = 0, bz2_stored = 0, xz_stored = 0, zstd_stored = 0, zip_stored = 0, pdf_stored = 0, stored_plain = 0;

    for (size_t i = 0; i < count; i++) {
        const TarEnt *e = &ents[i];
        buf_u8(recipe, OP_STRUCT);
        buf_u32(recipe, 512);
        buf_append(recipe, p + e->hdr_off, 512);

        if (e->size > 0) {
            buf_append(recipe, outs[i].frag.p, outs[i].frag.n);
            buf_append(b0, outs[i].f0.p, outs[i].f0.n);
            buf_append(b1, outs[i].f1.p, outs[i].f1.n);

            uint64_t pad = e->padded - e->size;
            if (pad > 0) {
                buf_u8(recipe, OP_STRUCT);
                buf_u32(recipe, (uint32_t)pad);
                buf_append(recipe, p + e->hdr_off + 512 + e->size, (size_t)pad);
            }
            if (e->is_regular) regulars++;

            jpeg_stored += outs[i].jpeg_stored;
            png_stored  += outs[i].png_stored;
            gzip_stored += outs[i].gzip_stored;
            bz2_stored  += outs[i].bz2_stored;
            xz_stored   += outs[i].xz_stored;
            zstd_stored += outs[i].zstd_stored;
            zip_stored  += outs[i].zip_stored;
            pdf_stored  += outs[i].pdf_stored;
            stored_plain += outs[i].stored_plain;
        }
        buf_free(&outs[i].frag); buf_free(&outs[i].f0); buf_free(&outs[i].f1);
    }
    free(outs);
    free(ents);

    if (tail_off < n) {
        buf_u8(recipe, OP_STRUCT);
        buf_u32(recipe, (uint32_t)(n - tail_off));
        buf_append(recipe, p + tail_off, n - tail_off);
    }

    fprintf(stderr, "    tar: %d regular (%d store, %d jpeg-store, %d png-store, %d gzip-store, %d bz2-store, %d xz-store, %d zstd-store, %d zip-store, %d pdf-store)\n",
            regulars, stored_plain, jpeg_stored, png_stored, gzip_stored, bz2_stored, xz_stored, zstd_stored, zip_stored, pdf_stored);
    return 0;
}
