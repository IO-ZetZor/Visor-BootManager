/* stub_crypto.c - stub when encrypted entry support is compiled out */

#include "crypto.h"

EFI_STATUS visor_decrypt_buffer(const void *input, UINTN input_size, CHAR16 *password, void **plain_out, UINTN *plain_size_out) {
    (void)input; (void)input_size; (void)password; (void)plain_out; (void)plain_size_out;
    return EFI_UNSUPPORTED;
}
