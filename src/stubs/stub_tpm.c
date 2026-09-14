/* stub_tpm.c - stub when TPM measurement is compiled out */

#include "tcg2.h"

int tpm_init(void) {
    return 0;
}

int tpm_measure_cmdline(CHAR16 *cmdline) {
    (void)cmdline;
    return 0;
}

int tpm_measure_config(const void *data, UINTN size, CHAR16 *name) {
    (void)data; (void)size; (void)name;
    return 0;
}

int tpm_present(void) {
    return 0;
}

void tpm_publish_banks(void) { }

void tpm_set_pcrs(UINTN config_pcr, UINTN cmdline_pcr) {
    (void)config_pcr; (void)cmdline_pcr;
}
