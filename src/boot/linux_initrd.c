/* linux_initrd.c - LoadFile2 initrd protocol for stub kernels */
#include "linux_internal.h"

#define LINUX_EFI_INITRD_MEDIA_GUID \
    { 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } }

typedef struct visor_lf2_protocol visor_lf2_protocol_t;
typedef EFI_STATUS (EFIAPI *visor_lf2_load_t)(visor_lf2_protocol_t *This,
                                              EFI_DEVICE_PATH_PROTOCOL *FilePath,
                                              BOOLEAN BootPolicy,
                                              UINTN *BufferSize, VOID *Buffer);
struct visor_lf2_protocol { visor_lf2_load_t LoadFile; };

static EFI_GUID visor_load_file2_guid =
    { 0x4006c0c1, 0xfcb3, 0x403e, { 0x99, 0x6d, 0x4a, 0x6c, 0x87, 0x24, 0xe0, 0x6d } };

typedef struct {
    VENDOR_DEVICE_PATH vendor;
    EFI_DEVICE_PATH    end;
} initrd_dev_path_t;

static void  *g_initrd_data = NULL;
static UINTN  g_initrd_size = 0;

static EFI_STATUS EFIAPI initrd_load_file(visor_lf2_protocol_t *This,
                                          EFI_DEVICE_PATH_PROTOCOL *FilePath,
                                          BOOLEAN BootPolicy, UINTN *BufferSize, VOID *Buffer) {
    (void)This; (void)FilePath;
    if (BootPolicy) return EFI_UNSUPPORTED;
    if (!BufferSize) return EFI_INVALID_PARAMETER;
    if (!g_initrd_data || g_initrd_size == 0) return EFI_NOT_FOUND;
    if (Buffer == NULL || *BufferSize < g_initrd_size) {
        *BufferSize = g_initrd_size;
        return EFI_BUFFER_TOO_SMALL;
    }
    CopyMem(Buffer, g_initrd_data, g_initrd_size);
    *BufferSize = g_initrd_size;
    return EFI_SUCCESS;
}

static visor_lf2_protocol_t initrd_lf2 = { initrd_load_file };
static initrd_dev_path_t initrd_dp = {
    .vendor = {
        .Header = { MEDIA_DEVICE_PATH, MEDIA_VENDOR_DP,
                    { (UINT8)(sizeof(VENDOR_DEVICE_PATH) & 0xFF),
                      (UINT8)((sizeof(VENDOR_DEVICE_PATH) >> 8) & 0xFF) } },
        .Guid = LINUX_EFI_INITRD_MEDIA_GUID
    },
    .end = { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
             { (UINT8)sizeof(EFI_DEVICE_PATH), 0 } }
};

EFI_HANDLE initrd_register(void *data, UINTN size) {
    g_initrd_data = data;
    g_initrd_size = size;
    EFI_HANDLE h = NULL;
    EFI_STATUS s = BS->InstallMultipleProtocolInterfaces(&h,
        &gEfiDevicePathProtocolGuid, &initrd_dp,
        &visor_load_file2_guid, &initrd_lf2,
        NULL);
    if (EFI_ERROR(s)) { g_initrd_data = NULL; g_initrd_size = 0; return NULL; }
    return h;
}

void initrd_unregister(EFI_HANDLE h) {
    if (h)
        BS->UninstallMultipleProtocolInterfaces(h,
            &gEfiDevicePathProtocolGuid, &initrd_dp,
            &visor_load_file2_guid, &initrd_lf2,
            NULL);
    g_initrd_data = NULL;
    g_initrd_size = 0;
}
