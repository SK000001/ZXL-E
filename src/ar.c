#include "ar.h"
#include "kinds.h"
#include "png.h"
#include "jpeg.h"
#include "gz.h"
#include "bz2.h"
#include "xz.h"
#include "zst.h"
#include "zip.h"

int pack_ar(const uint8_t *p, size_t n, const char *tmp_prefix,
            Buf *recipe, Buf *b0, Buf *b1) {
    if (n < 8) return -1;
    if (memcmp(p, "!<arch>\n", 8) != 0) return -1;

    buf_u8(recipe, OP_STRUCT);
    buf_u32(recipe, 8);
    buf_append(recipe, p, 8);

    size_t cur = 8;
    int entries = 0, gzip_stored = 0, bz2_stored = 0, xz_stored = 0, zstd_stored = 0, png_stored = 0, jpeg_stored = 0, zip_stored = 0, stored_plain = 0;

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
                if (pack_gz(body, (size_t)size, tp, &gz_recipe, b0, b1, 0) == 0) {
                    buf_u8(recipe, OP_GZIP_STORE);
                    buf_u32(recipe, (uint32_t)size);
                    buf_u32(recipe, (uint32_t)gz_recipe.n);
                    buf_append(recipe, gz_recipe.p, gz_recipe.n);
                    gzip_stored++;
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
                if (try_jpeg_buf(body, (size_t)size, tp, &brn) == 0) {
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
                if (pack_bz2(body, (size_t)size, tp, &bz_recipe, b0, b1, 0) == 0) {
                    buf_u8(recipe, OP_BZ2_STORE);
                    buf_u32(recipe, (uint32_t)size);
                    buf_u32(recipe, (uint32_t)bz_recipe.n);
                    buf_append(recipe, bz_recipe.p, bz_recipe.n);
                    bz2_stored++;
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
                    xz_stored++;
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
                    zstd_stored++;
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
                    zip_stored++;
                    handled = 1;
                }
                buf_free(&zip_recipe);
            }
            if (!handled) {
                uint8_t bk = bucket_for_bytes(body, (size_t)size);
                buf_u8(recipe, OP_STORE);
                buf_u32(recipe, (uint32_t)size);
                buf_u8(recipe, bk);
                buf_append(bk == 1 ? b1 : b0, body, (size_t)size);
                stored_plain++;
            }
            cur += size;
        }

        if (cur < n && (cur & 1) == 1) {
            if (p[cur] != 0x0A) return -1;
            buf_u8(recipe, OP_STRUCT);
            buf_u32(recipe, 1);
            buf_append(recipe, p + cur, 1);
            cur += 1;
        }
        entries++;
    }

    fprintf(stderr, "    ar: %d entries (%d store, %d gzip-store, %d bz2-store, %d xz-store, %d zstd-store, %d png-store, %d jpeg-store, %d zip-store)\n",
            entries, stored_plain, gzip_stored, bz2_stored, xz_stored, zstd_stored, png_stored, jpeg_stored, zip_stored);
    return 0;
}
