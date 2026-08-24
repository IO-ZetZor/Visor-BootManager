#include "crypto.h"
#include "sha256.h"
#include "efi_helpers.h"
#include <efi.h>
#include <efilib.h>

static int memeq_ct(const UINT8 *a, const UINT8 *b, UINTN n) {
    UINT8 diff = 0;
    for (UINTN i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

static void secure_zero(void *ptr, UINTN n) {
    volatile UINT8 *p = (volatile UINT8*)ptr;
    while (n--) *p++ = 0;
}

static UINT32 load32_le(const UINT8 *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) |
           ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static void store32_le(UINT8 *p, UINT32 v) {
    p[0] = (UINT8)v;
    p[1] = (UINT8)(v >> 8);
    p[2] = (UINT8)(v >> 16);
    p[3] = (UINT8)(v >> 24);
}

static UINT32 rotl32(UINT32 v, int n) {
    return (v << n) | (v >> (32 - n));
}

#define QR(a, b, c, d) do { \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  \
    c += d; b ^= c; b = rotl32(b, 7);  \
} while (0)

static void chacha20_block(const UINT8 key[32], const UINT8 nonce[12],
                           UINT32 counter, UINT8 out[64]) {
    static const UINT8 constants[16] = {
        'e','x','p','a','n','d',' ','3','2','-','b','y','t','e',' ','k'
    };
    UINT32 st[16], x[16];

    st[0] = load32_le(constants + 0);
    st[1] = load32_le(constants + 4);
    st[2] = load32_le(constants + 8);
    st[3] = load32_le(constants + 12);
    for (int i = 0; i < 8; i++) st[4 + i] = load32_le(key + i * 4);
    st[12] = counter;
    st[13] = load32_le(nonce + 0);
    st[14] = load32_le(nonce + 4);
    st[15] = load32_le(nonce + 8);

    for (int i = 0; i < 16; i++) x[i] = st[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) store32_le(out + i * 4, x[i] + st[i]);

    secure_zero(st, sizeof(st));
    secure_zero(x, sizeof(x));
}

static void hmac_sha256(const UINT8 *key, UINTN key_len,
                        const UINT8 *data1, UINTN data1_len,
                        const UINT8 *data2, UINTN data2_len,
                        UINT8 out[32]) {
    UINT8 k0[64];
    UINT8 ipad[64];
    UINT8 opad[64];
    UINT8 inner[32];

    for (UINTN i = 0; i < sizeof(k0); i++) k0[i] = 0;
    if (key_len > sizeof(k0)) {
        sha256(key, key_len, k0);
    } else if (key_len) {
        CopyMem(k0, (void*)key, key_len);
    }

    for (UINTN i = 0; i < sizeof(k0); i++) {
        ipad[i] = (UINT8)(k0[i] ^ 0x36);
        opad[i] = (UINT8)(k0[i] ^ 0x5c);
    }

    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, sizeof(ipad));
    if (data1 && data1_len) sha256_update(&ctx, data1, data1_len);
    if (data2 && data2_len) sha256_update(&ctx, data2, data2_len);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, sizeof(opad));
    sha256_update(&ctx, inner, sizeof(inner));
    sha256_final(&ctx, out);

    secure_zero(k0, sizeof(k0));
    secure_zero(ipad, sizeof(ipad));
    secure_zero(opad, sizeof(opad));
    secure_zero(inner, sizeof(inner));
}

static UINTN password_bytes(CHAR16 *password, UINT8 *out, UINTN cap) {
    UINTN n = 0;
    if (!password) return 0;
    while (password[n] && n + 1 < cap) {
        CHAR16 c = password[n];
        out[n] = (c < 0x80) ? (UINT8)c : (UINT8)'?';
        n++;
    }
    return n;
}

static void pbkdf2_hmac_sha256(CHAR16 *password, const visor_crypt_header_t *h,
                               UINT8 out[64]) {
    UINT8 pass[512];
    UINTN pass_len = password_bytes(password, pass, sizeof(pass));
    UINT32 iters = h->iterations;
    UINT8 salt_block[VISOR_CRYPT_SALT_SIZE + 4];
    UINT8 u[32], t[32];

    CopyMem(salt_block, (void*)h->salt, VISOR_CRYPT_SALT_SIZE);

    for (UINT32 block = 1; block <= 2; block++) {
        salt_block[VISOR_CRYPT_SALT_SIZE + 0] = (UINT8)(block >> 24);
        salt_block[VISOR_CRYPT_SALT_SIZE + 1] = (UINT8)(block >> 16);
        salt_block[VISOR_CRYPT_SALT_SIZE + 2] = (UINT8)(block >> 8);
        salt_block[VISOR_CRYPT_SALT_SIZE + 3] = (UINT8)block;

        hmac_sha256(pass, pass_len, salt_block, sizeof(salt_block), NULL, 0, u);
        CopyMem(t, u, sizeof(t));
        for (UINT32 i = 1; i < iters; i++) {
            hmac_sha256(pass, pass_len, u, sizeof(u), NULL, 0, u);
            for (UINTN j = 0; j < sizeof(t); j++) t[j] ^= u[j];
        }
        CopyMem(out + (block - 1) * 32, t, sizeof(t));
    }

    secure_zero(pass, sizeof(pass));
    secure_zero(salt_block, sizeof(salt_block));
    secure_zero(u, sizeof(u));
    secure_zero(t, sizeof(t));
}

static void crypt_stream(UINT8 *buf, UINTN size, const UINT8 key[32],
                         const visor_crypt_header_t *h) {
    UINT8 block[64];
    UINT32 counter = 1;
    UINTN pos = 0;

    while (pos < size) {
        chacha20_block(key, h->nonce, counter++, block);
        for (UINTN i = 0; i < sizeof(block) && pos < size; i++, pos++)
            buf[pos] ^= block[i];
    }

    secure_zero(block, sizeof(block));
}

EFI_STATUS visor_decrypt_buffer(const void *input, UINTN input_size,
                                CHAR16 *password,
                                void **plain_out, UINTN *plain_size_out) {
    if (!input || !password || !plain_out || !plain_size_out)
        return EFI_INVALID_PARAMETER;
    *plain_out = NULL;
    *plain_size_out = 0;

    if (input_size < sizeof(visor_crypt_header_t))
        return EFI_INVALID_PARAMETER;

    const visor_crypt_header_t *h = (const visor_crypt_header_t*)input;
    if (CompareMem((void*)h->magic, VISOR_CRYPT_MAGIC, 8) != 0 ||
        h->version != VISOR_CRYPT_VERSION)
        return EFI_UNSUPPORTED;

    if (h->iterations == 0 || h->iterations > VISOR_CRYPT_MAX_ITERATIONS)
        return EFI_INVALID_PARAMETER;

    if (h->plain_size == 0 || h->plain_size > 256ULL * 1024 * 1024)
        return EFI_INVALID_PARAMETER;
    if (h->plain_size != (UINT64)(input_size - sizeof(visor_crypt_header_t)))
        return EFI_INVALID_PARAMETER;

    UINTN plain_size = (UINTN)h->plain_size;
    UINTN cipher_size = input_size - sizeof(visor_crypt_header_t);
    const UINT8 *cipher = (const UINT8*)input + sizeof(visor_crypt_header_t);

    UINT8 keys[64], tag[32];
    UINT8 *enc_key = keys;
    UINT8 *mac_key = keys + 32;
    pbkdf2_hmac_sha256(password, h, keys);

    hmac_sha256(mac_key, 32, (const UINT8*)h,
                VISOR_CRYPT_HEADER_AUTH_SIZE, cipher, cipher_size, tag);

    if (!memeq_ct(tag, h->tag, sizeof(tag))) {
        secure_zero(keys, sizeof(keys));
        secure_zero(tag, sizeof(tag));
        return EFI_SECURITY_VIOLATION;
    }
    secure_zero(tag, sizeof(tag));

    UINT8 *plain = efi_allocate_pool(plain_size);
    if (!plain) {
        secure_zero(keys, sizeof(keys));
        return EFI_OUT_OF_RESOURCES;
    }
    CopyMem(plain, (void*)cipher, plain_size);

    crypt_stream(plain, plain_size, enc_key, h);
    secure_zero(keys, sizeof(keys));

    *plain_out = plain;
    *plain_size_out = plain_size;
    return EFI_SUCCESS;
}
