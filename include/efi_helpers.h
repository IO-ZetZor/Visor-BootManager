#ifndef EFI_HELPERS_H
#define EFI_HELPERS_H

#include <efi.h>
#include <efilib.h>

void efi_exit_boot_services(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table);

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
efi_file_t* efi_fopen(CHAR16 *path);
void efi_fclose(efi_file_t *file);
UINTN efi_fread(efi_file_t *file, void *buf, UINTN size);
int efi_file_exists(CHAR16 *path);

int efi_readdir(efi_file_t *dir, CHAR16 *name_out, UINTN name_cap, int *is_dir);

UINTN efi_volume_count(void);
EFI_FILE_PROTOCOL* efi_open_volume(UINTN index);
int efi_file_exists_root(EFI_FILE_PROTOCOL *root, CHAR16 *path);
EFI_FILE_PROTOCOL* efi_open_dir(EFI_FILE_PROTOCOL *root, CHAR16 *path);
int efi_read_dirent(EFI_FILE_PROTOCOL *dir, CHAR16 *name_out, UINTN name_cap, int *is_dir);
int efi_handle_matches_partition_uuid(EFI_HANDLE handle, CHAR16 *partition_uuid);
EFI_DEVICE_PATH* efi_make_file_path(EFI_HANDLE handle, CHAR16 *filename);
EFI_DEVICE_PATH* efi_file_device_path(CHAR16 *path, CHAR16 *partition_uuid);

typedef struct {
    void *data;
    UINTN size;
} efi_file_buffer_t;

efi_file_buffer_t* efi_load_file(CHAR16 *path);

void efi_load_fs_drivers(void);

extern int visor_quiet;

void efi_print(CHAR16 *msg, ...);

void efi_log(CHAR16 *msg);

void efi_log_begin(void);

EFI_HANDLE efi_get_device_handle(EFI_DEVICE_PATH *dp);

EFI_HANDLE* efi_locate_handle_buffer(EFI_GUID *proto, UINTN *count);

void efi_sleep(UINTN milliseconds);

UINT64 efi_get_tick(void);

#endif
