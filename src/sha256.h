#ifndef UCRAFT_SHA256_H
#define UCRAFT_SHA256_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t fill;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t n);
void sha256_final(sha256_ctx *c, char hex[65]);
int sha256_file(const char *path, char hex[65]);   /* 0 on success */

#endif
