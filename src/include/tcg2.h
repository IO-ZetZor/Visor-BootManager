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

/* Boot-loop breaker around the firmware TPM probe.  The probe can hang inside
 * firmware when the platform advertises a TPM it has actually disabled, and no
 * software watchdog can recover from that.  So: a non-volatile marker is set
 * before tpm_init() and cleared once the TPM stage completes.  If the marker
 * is still "probing" on the next boot, the previous boot died in that probe
 * and measured boot stays off until the user clears the marker.
 *
 * The marker is armed on every boot on purpose: if it were only written when
 * missing, a machine that probed successfully once would carry DONE forever
 * and a later hang would go undetected. */
int tpm_probe_skip(void);

EFI_STATUS tpm_stage_begin(void);

void tpm_stage_end(void);

#endif
