#ifndef ZXLE_ZIP_H
#define ZXLE_ZIP_H

#include "util.h"

typedef struct {
    uint64_t lfh_off;
    uint64_t payload_off;
    uint32_t comp_size;
    uint32_t raw_size;
    uint16_t method;
    uint16_t gp_flag;
    uint32_t crc32;
} ZipEntry;

size_t find_eocd(const uint8_t *p, size_t n);

int zip_parse(const uint8_t *p, size_t n,
              ZipEntry **out_entries, uint32_t *out_count,
              size_t *out_cd_off, size_t *out_cd_len,
              size_t *out_eocd_off, size_t *out_eocd_len);

int pack_zip(const uint8_t *p, size_t n, const char *tmp_prefix,
             Buf *recipe, Buf *b0, Buf *b1);

#endif
