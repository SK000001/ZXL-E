#ifndef ZXLE_UTIL_H
#define ZXLE_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#define ZXLE_MKDIR(p) _mkdir(p)
#define ZXLE_DEVNULL "NUL"
#else
#define ZXLE_MKDIR(p) mkdir((p), 0755)
#define ZXLE_DEVNULL "/dev/null"
#endif

void die(const char *msg);

void wu16(FILE *f, uint16_t v);
void wu32(FILE *f, uint32_t v);
void wu64(FILE *f, uint64_t v);

uint16_t r16(const uint8_t *p);
uint32_t r32(const uint8_t *p);

void run(const char *cmd);
int  try_run(const char *cmd);

long long fsize(const char *path);
const char *basename_of(const char *p);

uint8_t *read_whole_file(const char *path, size_t *out_len);

typedef struct {
    uint8_t *p;
    size_t   n;
    size_t   cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_reserve(Buf *b, size_t want);
void buf_append(Buf *b, const void *d, size_t n);
void buf_u8(Buf *b, uint8_t v);
void buf_u32(Buf *b, uint32_t v);

#endif
