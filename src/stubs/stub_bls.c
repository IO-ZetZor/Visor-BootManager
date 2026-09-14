/* stub_bls.c - stub when the BLS feature is compiled out */

#include "config_internal.h"

int bls_detect(config_t *config, int quick) {
    (void)config; (void)quick;
    return 0;
}
