/* config_internal.h - shared internals of the src/config translation units. */
#ifndef CONFIG_INTERNAL_H
#define CONFIG_INTERNAL_H

#include "config.h"
#include "efi_helpers.h"
#include "accent.h"
#include "arch.h"
#include "path_compat.h"
#include "tcg2.h"
#include "efi_selfheal.h"
#include <efi.h>
#include <efilib.h>

extern EFI_BOOT_SERVICES *BS;
extern EFI_SYSTEM_TABLE *ST;

CHAR16* trim(CHAR16 *s);
void strip_inline_comment(CHAR16 *s);
CHAR16* dup_path(CHAR16 *value);
int parse_sha256(CHAR16 *s, UINT8 out[32]);
int parse_color(CHAR16 *s, color_t *out);
int parse_spec(CHAR16 *value, accent_spec_t *sp);
int parse_spec_color(CHAR16 *value, accent_spec_t *sp, color_t *fallback);
void wipe16(CHAR16 *s);
void free_char16(CHAR16 **p);
void set_char16(CHAR16 **slot, CHAR16 *val);
void free_deployments(deployment_t *deps, UINTN count);
void free_entry_contents(boot_entry_t *e);
UINTN parse_uint(CHAR16 *s);
int is_header(CHAR16 *line, const CHAR16 *kw);
int looks_windows(CHAR16 *kernel_path);
int str_eq_ci(CHAR16 *a, const CHAR16 *b);
CHAR16* luks_cmdline_from_preset(CHAR16 *preset, CHAR16 *key_path);
EFI_STATUS parse_entry(config_t *config, CHAR16 **lines, UINTN *idx,
                              UINTN count, int win_hint);
CHAR16 lc16(CHAR16 c);
int contains_ci(CHAR16 *hay, const CHAR16 *needle);
int ends_with_ci(CHAR16 *s, const CHAR16 *suf);
CHAR16* icon_path_for(const CHAR16 *file);
CHAR16* distro_icon(CHAR16 *hint);
int scan_uki_dir(config_t *config, EFI_FILE_PROTOCOL *root, CHAR16 *dir);
int is_kernel_name(CHAR16 *name);
int entry_takes_default_cmdline(CHAR16 *kernel_path, int type);
extern int dc_foreign_volume;
CHAR16* dc_from_loader_entries(EFI_FILE_PROTOCOL *root, CHAR16 *kernel_name);
CHAR16* dc_derive_cmdline(EFI_FILE_PROTOCOL *root, EFI_HANDLE volume,
                                CHAR16 *kernel_name, int global_src);
int add_kernel_entry(config_t *config, EFI_FILE_PROTOCOL *root,
                            EFI_HANDLE volume, CHAR16 *dir, CHAR16 *name,
                            int global_cmdline, CHAR16 **auto_cmd,
                            int *auto_tried);
EFI_FILE_PROTOCOL *root_from_handle(EFI_HANDLE h);
int equals_ci(CHAR16 *a, const CHAR16 *b);
int scan_vendor_loaders(config_t *config, EFI_FILE_PROTOCOL *root);
CHAR16* read_text_from_root(EFI_FILE_PROTOCOL *root, CHAR16 *path);
int wcs16casecmp(CHAR16 *a, CHAR16 *b);
int bls_detect(config_t *config, int quick);
void tag_entries_since(config_t *config, UINTN first,
                              EFI_HANDLE volume);
int scope_wants_volume(int quick, EFI_HANDLE vol);
extern EFI_HANDLE dc_scanned_vols[64];
extern UINTN      dc_scanned_n;
extern int show_names_set;
extern int center_info_set;
EFI_STATUS detect_entries(config_t *config);
CHAR16* read_text_file(CHAR16 *path);
void apply_global(config_t *config, CHAR16 *key, CHAR16 *value);
void apply_theme(config_t *config, CHAR16 *name);
CHAR16* resolve_rotating_theme(int cycle);
void add_recovery_entries(config_t *config);
CHAR16* find_sub(CHAR16 *hay, const CHAR16 *needle);
int is_digits(CHAR16 *s);
INTN cmp16(const CHAR16 *a, const CHAR16 *b);
void snapshots_apply(config_t *config);
boot_entry_t* config_add_entry(config_t *config,
                                      CHAR16 *name,
                                      CHAR16 *icon_path,
                                      CHAR16 *kernel_path,
                                      CHAR16 *initrd_path,
                                      CHAR16 *cmdline,
                                      CHAR16 *uuid,
                                      int type,
                                      int encrypted,
                                      int initrd_encrypted);

#endif
