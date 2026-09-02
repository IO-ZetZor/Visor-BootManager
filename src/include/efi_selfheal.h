#ifndef EFI_SELFHEAL_H
#define EFI_SELFHEAL_H

#ifdef NVSH_HOST
/* types (UINT16, UINT8, ...) provided by the harness (efi_stub.h) */
#else
#include <efi.h>
#endif

/* Boot-order repair policies. See docs/boot.conf.schema.json (boot_order).
 *
 * OFF    - never touch NVRAM Boot entries or BootOrder.
 * ENSURE - recreate a missing Visor Boot#### entry and append it to BootOrder;
 *          never rearrange an existing order.
 * FIRST  - ENSURE plus: when firmware reports BootCurrent == our entry (a
 *          normal boot through the boot manager, including one-time selection
 *          from the firmware menu), move our entry to the head of BootOrder.
 *          On fallback / one-time no-slot boots behaves like ENSURE.
 */
#define NVSH_ORDER_OFF     0
#define NVSH_ORDER_ENSURE  1
#define NVSH_ORDER_FIRST   2

#define NVSH_ERR_OK            0
#define NVSH_ERR_DISABLED      1  /* all self-heal disabled by config */
#define NVSH_ERR_REMOVABLE     2  /* booted from removable media - skipped */
#define NVSH_ERR_NO_IMAGE      3  /* cannot resolve our own loaded path */
#define NVSH_ERR_BOOTMGR       4  /* generic firmware-variable failure */
#define NVSH_ERR_ORDER_LOCKED  5  /* SetVariable(BootOrder) denied */
#define NVSH_ERR_ENTRY_LOCKED  6  /* SetVariable(Boot####) denied */
#define NVSH_ERR_NOSLOT        7  /* no free Boot#### slot */

typedef struct {
    int order_mode;       /* NVSH_ORDER_* */
    int restore_fallback; /* restore \EFI\BOOT\BOOTx64.EFI from our binary */
    int dry_run;          /* compute and log the plan, write nothing */
} nvsh_policy_t;

typedef struct {
    int    entry_present;    /* a Boot#### pointing at our image exists */
    int    entry_in_order;   /* our entry appears in BootOrder */
    int    entry_created;    /* this run created our Boot#### */
    int    order_missing;    /* BootOrder variable is absent */
    int    order_updated;    /* this run rewrote BootOrder */
    int    normal_boot;      /* BootCurrent matched our entry */
    int    removable;        /* booted from removable media */
    int    fallback_restored;
    int    fallback_unneeded;
    UINT16 our_entry;        /* our Boot#### slot (0xFFFF = none) */
    UINT16 bootcurrent;      /* raw BootCurrent value */
    int    error;            /* NVSH_ERR_* */
    CHAR16 our_path[164];    /* image path we ran from */
} nvsh_report_t;

void nvsh_policy_defaults(nvsh_policy_t *policy);

nvsh_report_t nvram_self_heal(nvsh_policy_t *policy);

const CHAR16* nvsh_order_name(int mode);
const CHAR16* nvsh_err_text(int err);

/* --- Pure helpers, also exercised by the host unit harness --- */

UINTN nvsh_load_option_size(const CHAR16 *desc, UINTN path_len);
int   nvsh_build_load_option(UINT8 *out, UINTN cap, UINT32 attributes,
                             const CHAR16 *desc, const UINT8 *path,
                             UINTN path_len, UINTN *out_size);
int   nvsh_parse_load_option(const UINT8 *buf, UINTN size, UINT32 *attributes,
                             UINT16 *path_len, const CHAR16 **desc,
                             const UINT8 **path);
int   nvsh_parse_bootorder(const UINT8 *buf, UINTN size, UINT16 *list,
                           UINTN cap, UINTN *count);
int   nvsh_replan_order(UINT16 *list, UINTN n, UINT16 ours, int mode,
                        int normal_boot, UINTN *new_n);
int   nvsh_walk_filepath(const UINT8 *dp, UINTN dp_len, const CHAR16 **path);
int   nvsh_paths_equal16(const CHAR16 *a, const CHAR16 *b);

#endif