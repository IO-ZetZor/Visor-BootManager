#ifndef TCG2_H
#define TCG2_H

#include <efi.h>

#define TPM_PCR_CONFIG_DEFAULT   5
#define TPM_PCR_CMDLINE_DEFAULT 12

#define TPM_ALG_SHA1     0x0001
#define TPM_ALG_SHA256   0x000B
#define TPM_ALG_SHA384   0x000C
#define TPM_ALG_SHA512   0x000D
#define TPM_ALG_SM3_256  0x0012

int tpm_init(void);

int tpm_present(void);

UINT32 tpm_active_banks(void);

void tpm_publish_banks(void);

int tpm_measure_config(const void *data, UINTN size, CHAR16 *name);

int tpm_measure_cmdline(CHAR16 *cmdline);

void tpm_set_pcrs(UINTN config_pcr, UINTN cmdline_pcr);

#endif
