#include "deflate.h"
#include "util.h"
#include <zlib.h>

uint32_t r32be(const uint8_t *p) {
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | (uint32_t)p[3];
}
void w32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v>>24); p[1] = (uint8_t)(v>>16); p[2] = (uint8_t)(v>>8); p[3] = (uint8_t)v;
}

uint8_t *raw_inflate(const uint8_t *src, uint32_t comp_size, uint32_t raw_size) {
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

uint8_t *raw_deflate_l9(const uint8_t *raw, uint32_t raw_size, size_t *out_len) {
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

uint8_t *raw_inflate_dyn(const uint8_t *src, size_t src_n, size_t *out_n) {
    z_stream z = {0};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return NULL;
    size_t cap = src_n * 4 + 4096;
    uint8_t *out = malloc(cap);
    if (!out) { inflateEnd(&z); die("malloc raw_inflate_dyn"); }
    z.next_in = (Bytef *)src;
    z.avail_in = (uInt)src_n;
    z.next_out = out;
    z.avail_out = (uInt)cap;
    for (;;) {
        int rc = inflate(&z, Z_FINISH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR || rc == Z_OK) {
            size_t newcap = cap * 2;
            uint8_t *no = realloc(out, newcap);
            if (!no) { inflateEnd(&z); free(out); die("realloc raw_inflate_dyn"); }
            out = no;
            z.next_out = out + z.total_out;
            z.avail_out = (uInt)(newcap - z.total_out);
            cap = newcap;
            continue;
        }
        inflateEnd(&z); free(out); return NULL;
    }
    *out_n = z.total_out;
    inflateEnd(&z);
    return out;
}

uint8_t *zlib_inflate_dyn(const uint8_t *src, size_t src_n, size_t *out_n) {
    z_stream z = {0};
    if (inflateInit(&z) != Z_OK) return NULL;
    size_t cap = src_n * 4 + 4096;
    uint8_t *out = malloc(cap);
    if (!out) { inflateEnd(&z); die("malloc inflate"); }
    z.next_in = (Bytef *)src;
    z.avail_in = (uInt)src_n;
    z.next_out = out;
    z.avail_out = (uInt)cap;
    for (;;) {
        int rc = inflate(&z, Z_FINISH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR || rc == Z_OK) {
            size_t newcap = cap * 2;
            uint8_t *no = realloc(out, newcap);
            if (!no) { inflateEnd(&z); free(out); die("realloc inflate"); }
            out = no;
            z.next_out = out + z.total_out;
            z.avail_out = (uInt)(newcap - z.total_out);
            cap = newcap;
            continue;
        }
        inflateEnd(&z); free(out); return NULL;
    }
    *out_n = z.total_out;
    inflateEnd(&z);
    return out;
}

uint8_t *zlib_deflate_l9(const uint8_t *raw, size_t raw_n, size_t *out_n) {
    z_stream z = {0};
    if (deflateInit2(&z, 9, Z_DEFLATED, MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;
    size_t bound = deflateBound(&z, (uLong)raw_n);
    uint8_t *out = malloc(bound ? bound : 1);
    if (!out) { deflateEnd(&z); die("malloc zlib_deflate"); }
    z.next_in = (Bytef *)raw;
    z.avail_in = (uInt)raw_n;
    z.next_out = out;
    z.avail_out = (uInt)bound;
    int rc = deflate(&z, Z_FINISH);
    if (rc != Z_STREAM_END) { deflateEnd(&z); free(out); return NULL; }
    *out_n = z.total_out;
    deflateEnd(&z);
    return out;
}
