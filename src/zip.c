#include "zip.h"
#include "kinds.h"
#include "deflate.h"
#include "preflate_shim.h"
#include "png.h"
#include "jpeg.h"

size_t find_eocd(const uint8_t *p, size_t n) {
    if (n < 22) return (size_t)-1;
    size_t lo = n > 65557 ? n - 65557 : 0;
    for (size_t i = n - 22; i + 1 > lo; i--) {
        if (p[i]==0x50 && p[i+1]==0x4B && p[i+2]==0x05 && p[i+3]==0x06) return i;
        if (i == 0) break;
    }
    return (size_t)-1;
}

int zip_parse(const uint8_t *p, size_t n,
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
    if (cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu || entries_tot == 0xFFFFu) return -1;
    if ((size_t)eocd + 22 + comment_len != n) return -1;
    if ((size_t)cd_off + cd_size != eocd) return -1;

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
        if (comp == 0xFFFFFFFFu || raw == 0xFFFFFFFFu || lfh_off == 0xFFFFFFFFu) { free(ents); return -1; }
        if (gpflag & 0x0001) { free(ents); return -1; }
        if (method != 0 && method != 8) { free(ents); return -1; }
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

int pack_zip(const uint8_t *p, size_t n, const char *tmp_prefix,
             Buf *recipe, Buf *b0, Buf *b1) {
    ZipEntry *ents = NULL;
    uint32_t count = 0;
    size_t cd_off = 0, cd_len = 0, eocd_off = 0, eocd_len = 0;
    if (zip_parse(p, n, &ents, &count, &cd_off, &cd_len, &eocd_off, &eocd_len) != 0) return -1;

    size_t cursor = 0;
    int redeflated = 0, preflated = 0, store_orig = 0, stored_method = 0, jpeg_stored = 0, png_stored = 0;

    for (uint32_t i = 0; i < count; i++) {
        ZipEntry *e = &ents[i];
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
            if (e->raw_size != e->comp_size) { free(ents); return -1; }
            int handled = 0;
            if (!handled && e->raw_size >= 8 &&
                memcmp(p + e->payload_off, PNG_SIG, 8) == 0) {
                Buf png_recipe; buf_init(&png_recipe);
                if (pack_png(p + e->payload_off, e->raw_size, &png_recipe, b0) == 0) {
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
                if (try_jpeg_buf(p + e->payload_off, e->raw_size, tp, &brn) == 0) {
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
                uint8_t bk = bucket_for_bytes(p + e->payload_off, e->raw_size);
                buf_u8(recipe, OP_STORE);
                buf_u32(recipe, e->raw_size);
                buf_u8(recipe, bk);
                buf_append(bk == 1 ? b1 : b0, p + e->payload_off, e->raw_size);
                stored_method++;
            }
        } else {
            uint8_t *raw = raw_inflate(p + e->payload_off, e->comp_size, e->raw_size);
            if (!raw) {
                buf_u8(recipe, OP_STRUCT);
                buf_u32(recipe, e->comp_size);
                buf_append(recipe, p + e->payload_off, e->comp_size);
                store_orig++;
            } else {
                uint8_t bk = bucket_for_bytes(raw, e->raw_size);
                size_t redef_len = 0;
                uint8_t *redef = raw_deflate_l9(raw, e->raw_size, &redef_len);
                if (redef && redef_len == e->comp_size && memcmp(redef, p + e->payload_off, e->comp_size) == 0) {
                    buf_u8(recipe, OP_REDEFLATE);
                    buf_u32(recipe, e->raw_size);
                    buf_u8(recipe, bk);
                    buf_append(bk == 1 ? b1 : b0, raw, e->raw_size);
                    redeflated++;
                    free(redef);
                    free(raw);
                } else {
                    free(redef);
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
                            buf_u8(recipe, bk);
                            buf_u32(recipe, (uint32_t)diff_n);
                            buf_append(recipe, diff, diff_n);
                            buf_append(bk == 1 ? b1 : b0, unp, e->raw_size);
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
