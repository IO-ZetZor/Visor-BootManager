#ifndef EFI_SELFHEAL_H
#define EFI_SELFHEAL_H

#ifdef NVSH_HOST
#else
#include <efi.h>
#endif

/* Boot-order repair policies:
 * OFF    - never touch NVRAM Boot entries or BootOrder.
 * ENSURE - recreate a missing Boot#### and append it to BootOrder.
 * FIRST  - ENSURE plus: promote our entry to the head of BootOrder on a
 *          normal boot-manager boot; behaves like ENSURE otherwise.
 */
#define NVSH_ORDER_OFF     0
#define NVSH_ORDER_ENSURE  1
#define NVSH_ORDER_FIRST   2

#define NVSH_ERR_OK            0
#define NVSH_ERR_DISABLED      1
#define NVSH_ERR_REMOVABLE     2
#define NVSH_ERR_NO_IMAGE      3
#define NVSH_ERR_BOOTMGR       4
#define NVSH_ERR_ORDER_LOCKED  5
#define NVSH_ERR_ENTRY_LOCKED  6
#define NVSH_ERR_NOSLOT        7

typedef struct {
    int order_mode;
    int restore_fallback;
    int dry_run;
} nvsh_policy_t;

typedef struct {
    int    entry_present;
    int    entry_in_order;
    int    entry_created;
    int    order_missing;
    int    order_updated;
    int    normal_boot;
    int    removable;
    int    fallback_restored;
    int    fallback_unneeded;
    UINT16 our_entry;
    UINT16 bootcurrent;
    int    error;
    CHAR16 our_path[164];
} nvsh_report_t;

void nvsh_policy_defaults(nvsh_policy_t *policy);

nvsh_report_t nvram_self_heal(nvsh_policy_t *policy);

const CHAR16* nvsh_order_name(int mode);
const CHAR16* nvsh_err_text(int err);

/* Pure helpers, also exercised by the host unit harness */

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