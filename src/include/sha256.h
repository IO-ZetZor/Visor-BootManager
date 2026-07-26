#ifndef SHA256_H
#define SHA256_H

#include <efi.h>

typedef struct {
    UINT32 state[8];
    UINT64 bitlen;
    UINT8  buf[64];
    UINTN  buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const UINT8 *data, UINTN len);
void sha256_final(sha256_ctx *ctx, UINT8 out[32]);

void sha256(const UINT8 *data, UINTN len, UINT8 out[32]);

#endif
