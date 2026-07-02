#ifndef CRYPTO_H
#define CRYPTO_H

#include <efi.h>

#define VISOR_CRYPT_MAGIC "VISORENC"
#define VISOR_CRYPT_VERSION 2
#define VISOR_CRYPT_SALT_SIZE 16
#define VISOR_CRYPT_NONCE_SIZE 12
#define VISOR_CRYPT_HASH_SIZE 32
#define VISOR_CRYPT_MAX_ITERATIONS 5000000U
#define VISOR_CRYPT_HEADER_AUTH_SIZE 52

typedef struct {
    UINT8  magic[8];
    UINT32 version;
    UINT32 iterations;
    UINT64 plain_size;
    UINT8  salt[VISOR_CRYPT_SALT_SIZE];
    UINT8  nonce[VISOR_CRYPT_NONCE_SIZE];
    UINT8  tag[VISOR_CRYPT_HASH_SIZE];
} __attribute__((packed)) visor_crypt_header_t;

EFI_STATUS visor_decrypt_buffer(const void *input, UINTN input_size,
                                CHAR16 *password,
                                void **plain_out, UINTN *plain_size_out);

#endif
