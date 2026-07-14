#include "tar.h"
#include "kinds.h"
#include "png.h"
#include "jpeg.h"
#include "gz.h"
#include "bz2.h"
#include "xz.h"
#include "zst.h"
#include "zip.h"

int pack_tar(const uint8_t *p, size_t n, const char *tmp_prefix,
             Buf *recipe, Buf *b0, Buf *b1) {
    if (n < 1024) return -1;
    if (n % 512 != 0) return -1;
    if (memcmp(p + 257, "ustar", 5) != 0) return -1;

    size_t cur = 0;
    int regulars = 0, jpeg_stored = 0, png_stored = 0, gzip_stored = 0, bz2_stored = 0, xz_stored = 0, zstd_stored = 0, zip_stored = 0, stored_plain = 0;

    while (cur + 512 <= n) {
        const uint8_t *hdr = p + cur;

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
        if (hdr[124] & 0x80) return -1;

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

        buf_u8(recipe, OP_STRUCT);
        buf_u32(recipe, 512);
        buf_append(recipe, hdr, 512);
        cur += 512;

        if (size > 0) {
            int is_regular = (tflag == '0' || tflag == 0);
            int handled = 0;

            if (is_regular && size >= 8 && memcmp(p + cur, PNG_SIG, 8) == 0) {
                Buf png_recipe; buf_init(&png_recipe);
                if (pack_png(p + cur, (size_t)size, &png_recipe, b0) == 0) {
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
                    zip_stored++;
                    handled = 1;
                }
                buf_free(&zip_recipe);
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
                    gzip_stored++;
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
                    bz2_stored++;
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
                    xz_stored++;
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
                    zstd_stored++;
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

    fprintf(stderr, "    tar: %d regular (%d store, %d jpeg-store, %d png-store, %d gzip-store, %d bz2-store, %d xz-store, %d zstd-store, %d zip-store)\n",
            regulars, stored_plain, jpeg_stored, png_stored, gzip_stored, bz2_stored, xz_stored, zstd_stored, zip_stored);
    return 0;
}
