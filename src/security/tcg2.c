#include <efi.h>
#include <efilib.h>
#include "tcg2.h"
#include "efi_helpers.h"

extern EFI_BOOT_SERVICES *BS;

#define EFI_TCG2_PROTOCOL_GUID \
    { 0x607f766c, 0x7455, 0x42be, { 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f } }

#define EFI_TCG2_EVENT_HEADER_VERSION 1

#define EV_EVENT_TAG 0x00000006
#define EV_IPL       0x0000000D

#define LOADER_CONF_EVENT_TAG_ID 0xf5bc582a

typedef struct {
    UINT8 Major;
    UINT8 Minor;
} tcg2_version_t;

typedef struct {
    UINT8          Size;
    tcg2_version_t StructureVersion;
    tcg2_version_t ProtocolVersion;
    UINT32         HashAlgorithmBitmap;
    UINT32         SupportedEventLogs;
    BOOLEAN        TPMPresentFlag;
    UINT16         MaxCommandSize;
    UINT16         MaxResponseSize;
    UINT32         ManufacturerID;
    UINT32         NumberOfPcrBanks;
    UINT32         ActivePcrBanks;
} tcg2_capability_t;

typedef struct {
    UINT32 HeaderSize;
    UINT16 HeaderVersion;
    UINT32 PCRIndex;
    UINT32 EventType;
} __attribute__((packed)) tcg2_event_header_t;

typedef struct {
    UINT32              Size;
    tcg2_event_header_t Header;
    UINT8               Event[1];
} __attribute__((packed)) tcg2_event_t;

typedef struct {
    UINT32 EventId;
    UINT32 EventSize;
    UINT8  Event[1];
} __attribute__((packed)) tcg2_tagged_event_t;

#define TCG2_EVENT_HDR_BYTES  18
#define TCG2_TAGGED_HDR_BYTES  8

_Static_assert(sizeof(tcg2_capability_t) == 36, "TCG2 capability must be 36 bytes");
_Static_assert(__builtin_offsetof(tcg2_capability_t, ActivePcrBanks) == 32,
               "TCG2 ActivePcrBanks must be at offset 32");
_Static_assert(sizeof(tcg2_event_header_t) == 14, "TCG2 event header must be 14 bytes");
_Static_assert(__builtin_offsetof(tcg2_event_t, Event) == TCG2_EVENT_HDR_BYTES,
               "TCG2 event payload must start at offset 18");
_Static_assert(__builtin_offsetof(tcg2_tagged_event_t, Event) == TCG2_TAGGED_HDR_BYTES,
               "TCG2 tagged event payload must start at offset 8");

typedef struct tcg2_protocol tcg2_protocol_t;
struct tcg2_protocol {
    EFI_STATUS (EFIAPI *GetCapability)(tcg2_protocol_t *This,
                                       tcg2_capability_t *ProtocolCapability);
    EFI_STATUS (EFIAPI *GetEventLog)(tcg2_protocol_t *This, UINT32 EventLogFormat,
                                     EFI_PHYSICAL_ADDRESS *EventLogLocation,
                                     EFI_PHYSICAL_ADDRESS *EventLogLastEntry,
                                     BOOLEAN *EventLogTruncated);
    EFI_STATUS (EFIAPI *HashLogExtendEvent)(tcg2_protocol_t *This, UINT64 Flags,
                                            EFI_PHYSICAL_ADDRESS DataToHash,
                                            UINT64 DataToHashLen,
                                            tcg2_event_t *EfiTcgEvent);
    EFI_STATUS (EFIAPI *SubmitCommand)(tcg2_protocol_t *This,
                                       UINT32 InputParameterBlockSize,
                                       UINT8 *InputParameterBlock,
                                       UINT32 OutputParameterBlockSize,
                                       UINT8 *OutputParameterBlock);
    EFI_STATUS (EFIAPI *GetActivePcrBanks)(tcg2_protocol_t *This, UINT32 *ActivePcrBanks);
    EFI_STATUS (EFIAPI *SetActivePcrBanks)(tcg2_protocol_t *This, UINT32 ActivePcrBanks);
    EFI_STATUS (EFIAPI *GetResultOfSetActivePcrBanks)(tcg2_protocol_t *This,
                                                      UINT32 *OperationPresent,
                                                      UINT32 *Response);
};

static tcg2_protocol_t *g_tcg2      = NULL;
static UINT32           g_banks     = 0;
static int              g_probed    = 0;
static int              g_present   = 0;
static UINTN            g_pcr_config  = TPM_PCR_CONFIG_DEFAULT;
static UINTN            g_pcr_cmdline = TPM_PCR_CMDLINE_DEFAULT;

static UINTN strsize16_(CHAR16 *s) {
    return (efi_strlen16(s) + 1) * sizeof(CHAR16);
}

void tpm_set_pcrs(UINTN config_pcr, UINTN cmdline_pcr) {
    g_pcr_config  = config_pcr;
    g_pcr_cmdline = cmdline_pcr;
}

/* Vendor variable holding the TPM stage marker.  Non-volatile so it survives
 * the reset that a hung probe forces the user to perform.
 *
 *   absent   - never probed
 *   PROBING  - armed, tpm_init() has not returned yet
 *   DONE     - the probe completed
 *   DISABLED - a previous boot died inside the probe; do not probe again until
 *              the user clears the variable (after fixing the TPM in firmware
 *              setup) - from Linux:
 *                rm /sys/firmware/efi/efivars/VisorTpmStage-9f1c2d34-5a6b-4c7d-8e9f-a0b1c2d3e4f5 */
#define VISOR_TPM_STAGE_PROBING  1
#define VISOR_TPM_STAGE_DONE     2
#define VISOR_TPM_STAGE_DISABLED 3

static EFI_GUID g_visor_tpm_guid = {
    0x9f1c2d34, 0x5a6b, 0x4c7d, { 0x8e, 0x9f, 0xa0, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5 }
};

static int tpm_stage_read(void) {
    UINT8  v   = 0;
    UINTN  sz  = sizeof(v);
    UINT32 attr = 0;
    EFI_STATUS s = RT->GetVariable(L"VisorTpmStage", &g_visor_tpm_guid,
                                   &attr, &sz, &v);
    if (EFI_ERROR(s) || sz != sizeof(v)) return 0;
    return (int)v;
}

static EFI_STATUS tpm_stage_write(UINT8 v) {
    return RT->SetVariable(L"VisorTpmStage", &g_visor_tpm_guid,
                           EFI_VARIABLE_NON_VOLATILE |
                           EFI_VARIABLE_BOOTSERVICE_ACCESS |
                           EFI_VARIABLE_RUNTIME_ACCESS,
                           sizeof(v), &v);
}

int tpm_probe_skip(void) {
    int stage = tpm_stage_read();
    if (stage == VISOR_TPM_STAGE_PROBING) {
        /* This boot is running after a reset in the middle of the probe, so
         * the probe is what killed the machine.  Record it permanently: the
         * firmware state that caused it does not change on its own, and a
         * second hang would cost the user another power cycle. */
        efi_log(L"tpm: last boot stopped inside the TPM probe - disabling measured boot");
        tpm_stage_write(VISOR_TPM_STAGE_DISABLED);
        return 1;
    }
    if (stage == VISOR_TPM_STAGE_DISABLED) {
        efi_log(L"tpm: measured boot stays off; clear the VisorTpmStage variable to retry");
        return 1;
    }
    return 0;
}

EFI_STATUS tpm_stage_begin(void) {
    /* Armed on every boot.  Leaving DONE in place would make the next hang
     * undetectable - which is exactly the case the marker exists for. */
    return tpm_stage_write(VISOR_TPM_STAGE_PROBING);
}

void tpm_stage_end(void) {
    tpm_stage_write(VISOR_TPM_STAGE_DONE);
}

int tpm_init(void) {
    if (g_probed) return g_present;

    static EFI_GUID tcg2_guid = EFI_TCG2_PROTOCOL_GUID;
    tcg2_protocol_t *tcg = NULL;
    /* Checkpoints around the two firmware calls that can hang.  If a future
     * log stops after one of these lines and before the next, that call is
     * the one blocking on this platform. */
    efi_log(L"tpm: locating EFI_TCG2_PROTOCOL");
    if (EFI_ERROR(BS->LocateProtocol(&tcg2_guid, NULL, (void**)&tcg)) || !tcg) {
        efi_log(L"tpm: no EFI_TCG2_PROTOCOL - measured boot unavailable");
        return 0;
    }
    /* Cache only a completed probe.  A protocol that is installed later
     * must still be discoverable by a subsequent call. */
    g_probed = 1;
    if (!tcg->GetCapability || !tcg->HashLogExtendEvent) {
        efi_log(L"tpm: TCG2 protocol is missing required methods - skipping");
        return 0;
    }

    tcg2_capability_t cap;
    ZeroMem(&cap, sizeof(cap));
    cap.Size = (UINT8)sizeof(cap);
    efi_log(L"tpm: querying TCG2 capabilities");
    if (EFI_ERROR(tcg->GetCapability(tcg, &cap))) {
        efi_log(L"tpm: TCG2 GetCapability failed - skipping measurements");
        return 0;
    }
    /* A protocol whose capability block is empty or carries a placeholder
     * manufacturer id cannot drive a real TPM; do not build measurements on
     * top of it.  The size has to cover the field itself, not just its
     * offset: a block that stops at ActivePcrBanks does not contain it, and
     * reading it would publish an empty bank list. */
    if (cap.Size < (UINT8)sizeof(tcg2_capability_t) ||
        cap.ManufacturerID == 0 || cap.ManufacturerID == 0xFFFFFFFF) {
        CHAR16 why[96];
        SPrint(why, sizeof(why),
               L"tpm: bogus capability block (size=%d mfr=0x%08x) - skipping",
               (int)cap.Size, (unsigned)cap.ManufacturerID);
        efi_log(why);
        return 0;
    }
    if (!cap.TPMPresentFlag) {
        efi_log(L"tpm: TCG2 protocol present but no TPM is active");
        return 0;
    }

    g_tcg2  = tcg;
    g_banks = cap.ActivePcrBanks;
    if (!g_banks && tcg->GetActivePcrBanks)
        tcg->GetActivePcrBanks(tcg, &g_banks);
    g_present = 1;

    CHAR16 d[160];
    SPrint(d, sizeof(d),
           L"tpm: TPM%d.%d active, banks=0x%08x mfr=0x%08x (spec %d.%d)",
           (int)cap.StructureVersion.Major, (int)cap.StructureVersion.Minor,
           (unsigned)g_banks, (unsigned)cap.ManufacturerID,
           (int)cap.ProtocolVersion.Major, (int)cap.ProtocolVersion.Minor);
    efi_log(d);
    return 1;
}

int tpm_present(void) {
    return g_present;
}

UINT32 tpm_active_banks(void) {
    return g_present ? g_banks : 0;
}

void tpm_publish_banks(void) {

    if (efi_loader_var_exists(L"LoaderTpm2ActivePcrBanks")) return;

    UINT32 banks = tpm_active_banks();
    CHAR16 buf[16];
    static const CHAR16 hex[] = L"0123456789abcdef";
    buf[0] = L'0';
    buf[1] = L'x';
    for (int i = 0; i < 8; i++)
        buf[2 + i] = hex[(banks >> ((7 - i) * 4)) & 0xF];
    buf[10] = 0;

    efi_set_loader_var(L"LoaderTpm2ActivePcrBanks", buf, 0);
}

static int tpm_log(UINTN pcr, const void *data, UINTN size,
                   UINT32 tag, CHAR16 *desc) {
    if (!g_present || !g_tcg2) return 0;
    if (pcr == (UINTN)-1) return 0;
    if (!data || !size || !desc) return 0;

    UINTN desc_len = strsize16_(desc);
    UINTN total = TCG2_EVENT_HDR_BYTES + desc_len;
    if (tag) total += TCG2_TAGGED_HDR_BYTES;

    tcg2_event_t *ev = efi_allocate_pool(total);
    if (!ev) {
        efi_log(L"WARN: tpm: out of memory building the measurement event");
        return 0;
    }
    ZeroMem(ev, total);

    ev->Size                 = (UINT32)total;
    ev->Header.HeaderSize    = (UINT32)sizeof(tcg2_event_header_t);
    ev->Header.HeaderVersion = EFI_TCG2_EVENT_HEADER_VERSION;
    ev->Header.PCRIndex      = (UINT32)pcr;
    ev->Header.EventType     = tag ? EV_EVENT_TAG : EV_IPL;

    if (tag) {
        tcg2_tagged_event_t *te = (tcg2_tagged_event_t*)ev->Event;
        te->EventId   = tag;
        te->EventSize = (UINT32)desc_len;
        CopyMem(te->Event, desc, desc_len);
    } else {
        CopyMem(ev->Event, desc, desc_len);
    }

    EFI_STATUS s = g_tcg2->HashLogExtendEvent(g_tcg2, 0,
                                              (EFI_PHYSICAL_ADDRESS)(UINTN)data,
                                              (UINT64)size, ev);
    efi_free_pool(ev);

    if (EFI_ERROR(s)) {
        CHAR16 d[128];
        SPrint(d, sizeof(d),
               L"WARN: tpm: measuring into PCR %d failed (status=0x%lx)",
               (int)pcr, (long)s);
        efi_log(d);
        return 0;
    }

    CHAR16 d[160];
    SPrint(d, sizeof(d), L"tpm: measured %s (%d bytes) into PCR %d",
           desc, (int)size, (int)pcr);
    efi_log(d);
    return 1;
}

int tpm_measure_config(const void *data, UINTN size, CHAR16 *name) {
    return tpm_log(g_pcr_config, data, size, LOADER_CONF_EVENT_TAG_ID,
                   name ? name : L"boot.conf");
}

int tpm_measure_cmdline(CHAR16 *cmdline) {
    if (!cmdline || !cmdline[0]) return 0;

    return tpm_log(g_pcr_cmdline, cmdline, strsize16_(cmdline), 0, cmdline);
}
