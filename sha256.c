
#include "sha256.h"

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const UINT32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_transform(sha256_ctx *ctx, const UINT8 *p) {
    UINT32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((UINT32)p[i * 4] << 24) | ((UINT32)p[i * 4 + 1] << 16) |
               ((UINT32)p[i * 4 + 2] << 8) | (UINT32)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        UINT32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        UINT32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    UINT32 a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    UINT32 e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        UINT32 S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        UINT32 ch = (e & f) ^ ((~e) & g);
        UINT32 t1 = h + S1 + ch + K[i] + w[i];
        UINT32 S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        UINT32 maj = (a & b) ^ (a & c) ^ (b & c);
        UINT32 t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void sha256_update(sha256_ctx *ctx, const UINT8 *data, UINTN len) {
    for (UINTN i = 0; i < len; i++) {
        ctx->buf[ctx->buflen++] = data[i];
        if (ctx->buflen == 64) {
            sha256_transform(ctx, ctx->buf);
            ctx->bitlen += 512;
            ctx->buflen = 0;
        }
    }
}

void sha256_final(sha256_ctx *ctx, UINT8 out[32]) {
    UINT64 bits = ctx->bitlen + (UINT64)ctx->buflen * 8;
    UINTN i = ctx->buflen;

    ctx->buf[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->buf[i++] = 0;
        sha256_transform(ctx, ctx->buf);
        i = 0;
    }
    while (i < 56) ctx->buf[i++] = 0;
    for (int j = 7; j >= 0; j--) ctx->buf[i++] = (UINT8)(bits >> (j * 8));
    sha256_transform(ctx, ctx->buf);

    for (int j = 0; j < 8; j++) {
        out[j * 4]     = (UINT8)(ctx->state[j] >> 24);
        out[j * 4 + 1] = (UINT8)(ctx->state[j] >> 16);
        out[j * 4 + 2] = (UINT8)(ctx->state[j] >> 8);
        out[j * 4 + 3] = (UINT8)(ctx->state[j]);
    }
}

void sha256(const UINT8 *data, UINTN len, UINT8 out[32]) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}
