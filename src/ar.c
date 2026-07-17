#include "ar.h"
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

/* M7 step 5 (ar): same shape as pack_tar -- headers validated up front,
 * entry payloads on a worker pool into per-entry fragments, ordered splice.
 * All validation precedes any append, so malformed input never touches the
 * shared buckets. */

typedef struct {
    size_t hdr_off;   /* offset of the 60-byte header */
    uint64_t size;    /* payload bytes */
    int has_pad;      /* 0x0A alignment byte follows the payload */
} ArEnt;

typedef struct {
    Buf frag;
    Buf f0, f1;
    int gzip_stored, bz2_stored, xz_stored, zstd_stored;
    int png_stored, jpeg_stored, zip_stored, pdf_stored, stored_plain;
} ArEntOut;

static void ar_entry_payload(const uint8_t *p, const char *tmp_prefix,
                             const ArEnt *e, ArEntOut *o) {
    if (e->size == 0) return;
    size_t cur = e->hdr_off + 60;
    uint64_t size = e->size;
    const uint8_t *body = p + cur;
    Buf *recipe = &o->frag, *b0 = &o->f0, *b1 = &o->f1;
    int handled = 0;

    if (size >= 18 && body[0] == 0x1F && body[1] == 0x8B && body[2] == 0x08) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.argz.%zu", tmp_prefix, cur);
        Buf gz_recipe; buf_init(&gz_recipe);
        if (pack_gz(body, (size_t)size, tp, &gz_recipe, b0, b1, 0) == 0) {
            buf_u8(recipe, OP_GZIP_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)gz_recipe.n);
            buf_append(recipe, gz_recipe.p, gz_recipe.n);
            o->gzip_stored++;
            handled = 1;
        }
        buf_free(&gz_recipe);
    }
    if (!handled && size >= 8 && memcmp(body, PNG_SIG, 8) == 0) {
        Buf png_recipe; buf_init(&png_recipe);
        if (pack_png(body, (size_t)size, &png_recipe, b0) == 0) {
            buf_u8(recipe, OP_PNG_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)png_recipe.n);
            buf_append(recipe, png_recipe.p, png_recipe.n);
            o->png_stored++;
            handled = 1;
        }
        buf_free(&png_recipe);
    }
    if (!handled && size >= 4 &&
        body[0] == 0xFF && body[1] == 0xD8 && body[2] == 0xFF) {
        char tp[2048];
        snprintf(tp, sizeof(tp), "%s.arj.%zu", tmp_prefix, cur);
        Buf brn; buf_init(&brn);
        if (try_jpeg_buf(body, (size_t)size, tp, &brn) == 0) {
            buf_u8(recipe, OP_JPEG_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)brn.n);
            buf_append(recipe, brn.p, brn.n);
            o->jpeg_stored++;
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
        if (pack_bz2(body, (size_t)size, tp, &bz_recipe, b0, b1, 0) == 0) {
            buf_u8(recipe, OP_BZ2_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)bz_recipe.n);
            buf_append(recipe, bz_recipe.p, bz_recipe.n);
            o->bz2_stored++;
            handled = 1;
        }
        buf_free(&bz_recipe);
    }
    if (!handled && size >= 12 &&
        body[0] == 0xFD && body[1] == 0x37 && body[2] == 0x7A &&
        body[3] == 0x58 && body[4] == 0x5A && body[5] == 0x00) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.arxz.%zu", tmp_prefix, cur);
        Buf xz_recipe; buf_init(&xz_recipe);
        if (pack_xz(body, (size_t)size, tp, &xz_recipe, b0, b1, 0) == 0) {
            buf_u8(recipe, OP_XZ_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)xz_recipe.n);
            buf_append(recipe, xz_recipe.p, xz_recipe.n);
            o->xz_stored++;
            handled = 1;
        }
        buf_free(&xz_recipe);
    }
    if (!handled && size >= 8 &&
        body[0] == 0x28 && body[1] == 0xB5 &&
        body[2] == 0x2F && body[3] == 0xFD) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.arzs.%zu", tmp_prefix, cur);
        Buf zs_recipe; buf_init(&zs_recipe);
        if (pack_zst(body, (size_t)size, tp, &zs_recipe, b0, b1, 0) == 0) {
            buf_u8(recipe, OP_ZSTD_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)zs_recipe.n);
            buf_append(recipe, zs_recipe.p, zs_recipe.n);
            o->zstd_stored++;
            handled = 1;
        }
        buf_free(&zs_recipe);
    }
    if (!handled && size >= 22 && body[0] == 0x50 && body[1] == 0x4B) {
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.arzip.%zu", tmp_prefix, cur);
        Buf zip_recipe; buf_init(&zip_recipe);
        if (pack_zip(body, (size_t)size, tp, &zip_recipe, b0, b1) == 0) {
            buf_u8(recipe, OP_ZIP_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)zip_recipe.n);
            buf_append(recipe, zip_recipe.p, zip_recipe.n);
            o->zip_stored++;
            handled = 1;
        }
        buf_free(&zip_recipe);
    }
    if (!handled && size >= 32 && memcmp(body, "%PDF-", 5) == 0) {
        /* Nested PDF recipe shares the generic OP vocabulary, so it
         * rides OP_ZIP_STORE (recurse-unpack_recipe semantics). */
        char tp[1024];
        snprintf(tp, sizeof(tp), "%s.arpdf.%zu", tmp_prefix, cur);
        Buf pdf_recipe; buf_init(&pdf_recipe);
        if (pack_pdf(body, (size_t)size, tp, &pdf_recipe, b0, b1) == 0) {
            buf_u8(recipe, OP_ZIP_STORE);
            buf_u32(recipe, (uint32_t)size);
            buf_u32(recipe, (uint32_t)pdf_recipe.n);
            buf_append(recipe, pdf_recipe.p, pdf_recipe.n);
            o->pdf_stored++;
            handled = 1;
        }
        buf_free(&pdf_recipe);
    }
    if (!handled) {
        uint8_t bk = bucket_for_bytes(body, (size_t)size);
        buf_u8(recipe, OP_STORE);
        buf_u32(recipe, (uint32_t)size);
        buf_u8(recipe, bk);
        buf_append(bk == 1 ? b1 : b0, body, (size_t)size);
        o->stored_plain++;
    }
}

typedef struct {
    const uint8_t *p;
    const char *tmp_prefix;
    const ArEnt *ents;
    size_t count;
    ArEntOut *outs;
    long next;
} ArPool;

static void *ar_pool_worker(void *arg) {
    ArPool *pl = (ArPool *)arg;
    for (;;) {
        long i = __atomic_fetch_add(&pl->next, 1, __ATOMIC_SEQ_CST);
        if (i >= (long)pl->count) return NULL;
        ar_entry_payload(pl->p, pl->tmp_prefix, &pl->ents[i], &pl->outs[i]);
    }
}

int pack_ar(const uint8_t *p, size_t n, const char *tmp_prefix,
            Buf *recipe, Buf *b0, Buf *b1) {
    if (n < 8) return -1;
    if (memcmp(p, "!<arch>\n", 8) != 0) return -1;

    /* Parse phase: validate every header before any output is produced. */
    ArEnt *ents = NULL;
    size_t count = 0, cap = 0;
    size_t cur = 8;

    while (cur < n) {
        if (cur + 60 > n) { free(ents); return -1; }
        const uint8_t *hdr = p + cur;
        if (hdr[58] != 0x60 || hdr[59] != 0x0A) { free(ents); return -1; }

        uint64_t size = 0;
        for (int i = 0; i < 10; i++) {
            uint8_t c = hdr[48 + i];
            if (c == ' ' || c == 0) break;
            if (c < '0' || c > '9') { free(ents); return -1; }
            size = size * 10 + (c - '0');
        }
        if (size > 0xFFFFFFFFu) { free(ents); return -1; }
        if (cur + 60 + size > n) { free(ents); return -1; }

        int has_pad = 0;
        size_t after = cur + 60 + (size_t)size;
        if (after < n && (after & 1) == 1) {
            if (p[after] != 0x0A) { free(ents); return -1; }
            has_pad = 1;
        }

        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            ArEnt *ne = realloc(ents, cap * sizeof(ArEnt));
            if (!ne) { free(ents); return -1; }
            ents = ne;
        }
        ents[count].hdr_off = cur;
        ents[count].size = size;
        ents[count].has_pad = has_pad;
        count++;
        cur = after + has_pad;
    }

    ArEntOut *outs = calloc(count ? count : 1, sizeof(ArEntOut));
    if (!outs) { free(ents); return -1; }
    for (size_t i = 0; i < count; i++) {
        buf_init(&outs[i].frag); buf_init(&outs[i].f0); buf_init(&outs[i].f1);
    }
    ArPool pl = { p, tmp_prefix, ents, count, outs, 0 };
    int nth = zxle_ncpus();
    if (nth > (int)count) nth = (int)count;
    if (nth > 64) nth = 64;
    if (nth > 1) {
        pthread_t th[64];
        for (int t = 0; t < nth; t++) pthread_create(&th[t], NULL, ar_pool_worker, &pl);
        for (int t = 0; t < nth; t++) pthread_join(th[t], NULL);
    } else {
        ar_pool_worker(&pl);
    }

    buf_u8(recipe, OP_STRUCT);
    buf_u32(recipe, 8);
    buf_append(recipe, p, 8);

    int entries = 0, gzip_stored = 0, bz2_stored = 0, xz_stored = 0, zstd_stored = 0, png_stored = 0, jpeg_stored = 0, zip_stored = 0, pdf_stored = 0, stored_plain = 0;

    for (size_t i = 0; i < count; i++) {
        const ArEnt *e = &ents[i];
        buf_u8(recipe, OP_STRUCT);
        buf_u32(recipe, 60);
        buf_append(recipe, p + e->hdr_off, 60);

        if (e->size > 0) {
            buf_append(recipe, outs[i].frag.p, outs[i].frag.n);
            buf_append(b0, outs[i].f0.p, outs[i].f0.n);
            buf_append(b1, outs[i].f1.p, outs[i].f1.n);

            gzip_stored += outs[i].gzip_stored;
            bz2_stored  += outs[i].bz2_stored;
            xz_stored   += outs[i].xz_stored;
            zstd_stored += outs[i].zstd_stored;
            png_stored  += outs[i].png_stored;
            jpeg_stored += outs[i].jpeg_stored;
            zip_stored  += outs[i].zip_stored;
            pdf_stored  += outs[i].pdf_stored;
            stored_plain += outs[i].stored_plain;
        }
        if (e->has_pad) {
            buf_u8(recipe, OP_STRUCT);
            buf_u32(recipe, 1);
            buf_append(recipe, p + e->hdr_off + 60 + e->size, 1);
        }
        entries++;
        buf_free(&outs[i].frag); buf_free(&outs[i].f0); buf_free(&outs[i].f1);
    }
    free(outs);
    free(ents);

    fprintf(stderr, "    ar: %d entries (%d store, %d gzip-store, %d bz2-store, %d xz-store, %d zstd-store, %d png-store, %d jpeg-store, %d zip-store, %d pdf-store)\n",
            entries, stored_plain, gzip_stored, bz2_stored, xz_stored, zstd_stored, png_stored, jpeg_stored, zip_stored, pdf_stored);
    return 0;
}
