/* efi_path.c - device paths, partition UUIDs and path normalisation */
#include "efi_helpers_internal.h"

static int parse_partition_uuid(CHAR16 *s, EFI_GUID *out) {
    if (!s || !out) return 0;
    UINT8 raw[16];
    int pos = 0;

    for (UINTN i = 0; s[i];) {
        if (s[i] == '-') {
            i++;
            continue;
        }
        if (!s[i + 1] || pos >= 16 || !parse_hex_byte(s + i, &raw[pos++]))
            return 0;
        i += 2;
    }
    if (pos != 16) return 0;

    out->Data1 = ((UINT32)raw[0] << 24) | ((UINT32)raw[1] << 16) |
                 ((UINT32)raw[2] << 8)  | raw[3];
    out->Data2 = ((UINT16)raw[4] << 8) | raw[5];
    out->Data3 = ((UINT16)raw[6] << 8) | raw[7];
    for (int i = 0; i < 8; i++) out->Data4[i] = raw[8 + i];
    return 1;
}

int efi_handle_matches_partition_uuid(EFI_HANDLE handle, CHAR16 *partition_uuid) {
    if (!partition_uuid || partition_uuid[0] == '\0') return 1;

    EFI_GUID want;
    if (!parse_partition_uuid(partition_uuid, &want)) return 0;

    EFI_DEVICE_PATH *dp = NULL;
    if (EFI_ERROR(BS->HandleProtocol(handle, &gEfiDevicePathProtocolGuid, (void**)&dp)) || !dp)
        return 0;

    EFI_DEVICE_PATH *node = dp;
    while (!IsDevicePathEnd(node)) {
        if (DevicePathType(node) == MEDIA_DEVICE_PATH &&
            DevicePathSubType(node) == MEDIA_HARDDRIVE_DP) {
            HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH*)node;
            if (hd->SignatureType == SIGNATURE_TYPE_GUID &&
                CompareMem(hd->Signature, &want, sizeof(want)) == 0)
                return 1;
        }
        node = (EFI_DEVICE_PATH*)((UINT8*)node + DevicePathNodeLength(node));
    }

    return 0;
}

CHAR16* efi_handle_partition_uuid(EFI_HANDLE handle) {
    EFI_DEVICE_PATH *dp = NULL;
    if (EFI_ERROR(BS->HandleProtocol(handle, &gEfiDevicePathProtocolGuid, (void**)&dp)) || !dp)
        return NULL;

    for (EFI_DEVICE_PATH *node = dp; !IsDevicePathEnd(node);
         node = (EFI_DEVICE_PATH*)((UINT8*)node + DevicePathNodeLength(node))) {
        if (DevicePathType(node) != MEDIA_DEVICE_PATH ||
            DevicePathSubType(node) != MEDIA_HARDDRIVE_DP)
            continue;
        HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH*)node;
        if (hd->SignatureType != SIGNATURE_TYPE_GUID) continue;

        EFI_GUID g;
        CopyMem(&g, hd->Signature, sizeof(g));
        UINT8 b[16] = {
            (UINT8)(g.Data1 >> 24), (UINT8)(g.Data1 >> 16),
            (UINT8)(g.Data1 >> 8),  (UINT8)g.Data1,
            (UINT8)(g.Data2 >> 8),  (UINT8)g.Data2,
            (UINT8)(g.Data3 >> 8),  (UINT8)g.Data3,
            g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
            g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7],
        };

        CHAR16 *s = efi_allocate_pool(37 * sizeof(CHAR16));
        if (!s) return NULL;
        static const CHAR16 hexd[] = L"0123456789abcdef";
        UINTN o = 0;
        for (UINTN i = 0; i < 16; i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10) s[o++] = '-';
            s[o++] = hexd[b[i] >> 4];
            s[o++] = hexd[b[i] & 0xF];
        }
        s[o] = 0;
        return s;
    }
    return NULL;
}
CHAR16* collapse_backslashes(CHAR16 *path, CHAR16 *buf, UINTN cap) {
    if (!path) return path;
    UINTN o = 0;
    for (UINTN i = 0; path[i]; i++) {
        if (path[i] == '\\' && o > 0 && buf[o - 1] == '\\') continue;
        if (o + 1 >= cap) return path;
        buf[o++] = path[i];
    }
    buf[o] = '\0';
    return buf;
}

EFI_DEVICE_PATH* efi_make_file_path(EFI_HANDLE handle, CHAR16 *filename) {
    CHAR16 nbuf[NORM_PATH_MAX];
    filename = collapse_backslashes(filename, nbuf, NORM_PATH_MAX);
    EFI_DEVICE_PATH *dp = NULL;
    BS->HandleProtocol(handle, &gEfiDevicePathProtocolGuid, (void**)&dp);
    if (!dp) return NULL;

    UINTN dp_total = DevicePathSize(dp);
    if (dp_total < sizeof(EFI_DEVICE_PATH_PROTOCOL)) return NULL;
    UINTN dp_len    = dp_total - sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN fname_len = StrLen(filename) * sizeof(CHAR16);
    UINTN fp_size   = sizeof(FILEPATH_DEVICE_PATH) + fname_len;
    UINTN end_size  = sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN total_len = dp_len + fp_size + end_size;

    if (fp_size > 0xFFFF) {
        efi_log(L"ERROR: file path too long for a device-path node");
        return NULL;
    }

    UINT8 *new_dp = efi_allocate_pool(total_len);
    if (!new_dp) return NULL;

    CopyMem(new_dp, dp, dp_len);

    FILEPATH_DEVICE_PATH *fp = (FILEPATH_DEVICE_PATH*)(new_dp + dp_len);
    fp->Header.Type    = MEDIA_DEVICE_PATH;
    fp->Header.SubType = MEDIA_FILEPATH_DP;
    SetDevicePathNodeLength(&fp->Header, (UINT16)fp_size);
    StrCpy(fp->PathName, filename);

    EFI_DEVICE_PATH_PROTOCOL *end = (EFI_DEVICE_PATH_PROTOCOL*)(new_dp + dp_len + fp_size);
    end->Type    = END_DEVICE_PATH_TYPE;
    end->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    SetDevicePathNodeLength(end, (UINT16)end_size);

    return (EFI_DEVICE_PATH*)new_dp;
}

EFI_DEVICE_PATH* efi_file_device_path(CHAR16 *path, CHAR16 *partition_uuid) {
    EFI_HANDLE boot_handle = boot_device_handle();
    EFI_FILE_PROTOCOL *root = open_root_on_handle(boot_handle);
    if (root) {
        if (efi_handle_matches_partition_uuid(boot_handle, partition_uuid) &&
            efi_file_exists_root(root, path)) {
            root->Close(root);
            return efi_make_file_path(boot_handle, path);
        }
        root->Close(root);
    }

    UINTN count = 0;
    EFI_HANDLE *handles = efi_locate_handle_buffer(
        &gEfiSimpleFileSystemProtocolGuid, &count);
    if (!handles) return NULL;

    EFI_DEVICE_PATH *dp = NULL;
    for (UINTN i = 0; i < count; i++) {
        if (!efi_handle_matches_partition_uuid(handles[i], partition_uuid))
            continue;

        EFI_FILE_IO_INTERFACE *io = NULL;
        if (EFI_ERROR(BS->HandleProtocol(handles[i],
                &gEfiSimpleFileSystemProtocolGuid, (void**)&io)) || !io)
            continue;

        EFI_FILE_PROTOCOL *r = NULL;
        g_volume_opens++;
        UINT64 t_mount = arch_now_us();
        EFI_STATUS ms = io->OpenVolume(io, &r);
        efi_log_slow(L"OpenVolume (mounting a volume during a path search)", t_mount);
        if (EFI_ERROR(ms) || !r)
            continue;

        if (efi_file_exists_root(r, path))
            dp = efi_make_file_path(handles[i], path);
        r->Close(r);
        if (dp) break;
    }

    efi_free_pool(handles);
    return dp;
}

EFI_DEVICE_PATH* efi_file_device_path_on_handle(EFI_HANDLE volume, CHAR16 *path) {
    if (!volume) return NULL;
    EFI_FILE_PROTOCOL *root = open_root_on_handle(volume);
    if (!root) return NULL;
    int found = efi_file_exists_root(root, path);
    root->Close(root);
    return found ? efi_make_file_path(volume, path) : NULL;
}

int efi_handles_same_disk(EFI_HANDLE a, EFI_HANDLE b) {
    if (!a || !b) return 0;
    if (a == b) return 1;

    EFI_DEVICE_PATH *pa = NULL, *pb = NULL;
    if (EFI_ERROR(BS->HandleProtocol(a, &gEfiDevicePathProtocolGuid, (void**)&pa)) || !pa)
        return 0;
    if (EFI_ERROR(BS->HandleProtocol(b, &gEfiDevicePathProtocolGuid, (void**)&pb)) || !pb)
        return 0;

    for (;;) {
        int end_a = IsDevicePathEnd(pa) || dp_node_is_harddrive(pa);
        int end_b = IsDevicePathEnd(pb) || dp_node_is_harddrive(pb);
        if (end_a || end_b) return end_a && end_b;

        UINTN la = DevicePathNodeLength(pa);
        UINTN lb = DevicePathNodeLength(pb);
        if (la != lb || la < 4 || CompareMem(pa, pb, la) != 0) return 0;

        pa = (EFI_DEVICE_PATH*)((UINT8*)pa + la);
        pb = (EFI_DEVICE_PATH*)((UINT8*)pb + lb);
    }
}
