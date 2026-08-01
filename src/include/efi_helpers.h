#ifndef EFI_HELPERS_H
#define EFI_HELPERS_H

#include <efi.h>
#include <efilib.h>

void* efi_allocate_pool(UINTN size);
void efi_free_pool(void *ptr);

CHAR16* efi_strdup(CHAR16 *src);
int efi_strcmp(CHAR16 *s1, CHAR16 *s2);
CHAR16* efi_strchr(CHAR16 *s, CHAR16 c);

typedef struct {
    EFI_FILE_PROTOCOL *root;
    EFI_FILE_PROTOCOL *handle;
} efi_file_t;

EFI_FILE_PROTOCOL* efi_boot_volume_root(void);
EFI_HANDLE efi_boot_volume_handle(void);
int efi_handles_same_disk(EFI_HANDLE a, EFI_HANDLE b);
int efi_handle_has_filesystem(EFI_HANDLE handle);
efi_file_t* efi_fopen(CHAR16 *path);

efi_file_t* efi_fopen_uuid(CHAR16 *path, CHAR16 *uuid);
void efi_fclose(efi_file_t *file);
UINTN efi_fread(efi_file_t *file, void *buf, UINTN size);

int efi_readdir(efi_file_t *dir, CHAR16 *name_out, UINTN name_cap, int *is_dir);

int efi_file_exists_root(EFI_FILE_PROTOCOL *root, CHAR16 *path);
EFI_FILE_PROTOCOL* efi_open_dir(EFI_FILE_PROTOCOL *root, CHAR16 *path);
int efi_read_dirent(EFI_FILE_PROTOCOL *dir, CHAR16 *name_out, UINTN name_cap, int *is_dir);
int efi_handle_matches_partition_uuid(EFI_HANDLE handle, CHAR16 *partition_uuid);

CHAR16* efi_handle_partition_uuid(EFI_HANDLE handle);
EFI_DEVICE_PATH* efi_make_file_path(EFI_HANDLE handle, CHAR16 *filename);
EFI_DEVICE_PATH* efi_file_device_path(CHAR16 *path, CHAR16 *partition_uuid);
EFI_DEVICE_PATH* efi_file_device_path_on_handle(EFI_HANDLE volume, CHAR16 *path);

typedef struct {
    void *data;
    UINTN size;
} efi_file_buffer_t;

UINT64 efi_file_size(EFI_FILE_PROTOCOL *fh);
efi_file_buffer_t* efi_load_file(CHAR16 *path);

efi_file_buffer_t* efi_load_file_uuid(CHAR16 *path, CHAR16 *uuid);
efi_file_buffer_t* efi_load_file_on_handle(EFI_HANDLE volume, CHAR16 *path);

int efi_rename_file(CHAR16 *oldp, CHAR16 *newp);

void efi_load_fs_drivers(void);

int  efi_fs_drivers_deferred(void);

int  efi_fs_drivers_pending(void);

void efi_start_deferred_drivers(void);

void efi_start_deferred_images(void);

int  efi_connect_next_block(CHAR16 *prefer_uuid);

void efi_fs_drivers_set_lazy(int enabled);

extern int visor_quiet;
extern int visor_log_to_console;
extern int visor_boot_services_active;

void efi_print(CHAR16 *msg, ...);

void efi_log(CHAR16 *msg);

void efi_log_set_console(int enabled);

void efi_log_set_file(int enabled);

int efi_log_file_enabled(void);

void efi_log_begin(void);

void efi_log_rotate(void);

void efi_log_close(void);

EFI_HANDLE* efi_locate_handle_buffer(EFI_GUID *proto, UINTN *count);

void efi_sleep(UINTN milliseconds);

UINT64 efi_get_tick(void);

int efi_secure_boot_enabled(void);
int efi_shim_verify(void *buf, UINTN size);

CHAR16* efi_get_var_str(CHAR16 *name);
void efi_set_var_str(CHAR16 *name, CHAR16 *val);
int efi_get_var_u32(CHAR16 *name, UINT32 *out);
void efi_set_var_u32(CHAR16 *name, UINT32 val);

UINT32 efi_rand(void);

#endif
