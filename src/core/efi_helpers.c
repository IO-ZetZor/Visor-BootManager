/* efi_helpers.c - pool allocation, string helpers, time and the boot volume */
#include "efi_helpers_internal.h"

EFI_HANDLE boot_device_handle(void) {
    static EFI_HANDLE cached = NULL;
    static int resolved = 0;
    if (resolved) return cached;
    resolved = 1;
    EFI_LOADED_IMAGE *li = NULL;
    if (!EFI_ERROR(BS->HandleProtocol(IH, &gEfiLoadedImageProtocolGuid, (void**)&li)) && li)
        cached = li->DeviceHandle;
    return cached;
}

UINTN g_volume_opens = 0;

UINTN efi_volume_open_count(void) {
    return g_volume_opens;
}

EFI_FILE_PROTOCOL *open_root_on_handle(EFI_HANDLE h) {
    if (!h) return NULL;
    EFI_FILE_IO_INTERFACE *io = NULL;
    if (EFI_ERROR(BS->HandleProtocol(h, &gEfiSimpleFileSystemProtocolGuid, (void**)&io)) || !io)
        return NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    g_volume_opens++;
    if (EFI_ERROR(io->OpenVolume(io, &root))) return NULL;
    return root;
}

EFI_FILE_PROTOCOL* efi_boot_volume_root(void) {
    return open_root_on_handle(boot_device_handle());
}

EFI_HANDLE efi_boot_volume_handle(void) {
    return boot_device_handle();
}

void* efi_allocate_pool(UINTN size) {
    void *ptr = NULL;
    BS->AllocatePool(EfiLoaderData, size, &ptr);
    return ptr;
}

void efi_free_pool(void *ptr) {
    if (ptr) {
        BS->FreePool(ptr);
    }
}

CHAR16* efi_strdup(CHAR16 *src) {
    if (!src) return NULL;
    UINTN len = 0;
    while (src[len]) len++;
    CHAR16 *dst = efi_allocate_pool((len + 1) * sizeof(CHAR16));
    if (!dst) return NULL;
    for (UINTN i = 0; i <= len; i++) {
        dst[i] = src[i];
    }
    return dst;
}

int efi_strcmp(CHAR16 *s1, CHAR16 *s2) {
    if (!s1 || !s2) return 1;
    while (*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

UINTN efi_strlen16(CHAR16 *s) {
    if (!s) return 0;
    UINTN n = 0;
    while (s[n]) n++;
    return n;
}

UINTN efi_strlen8(const char *s) {
    if (!s) return 0;
    UINTN n = 0;
    while (s[n]) n++;
    return n;
}

int text_eq_ci(CHAR16 *a, const CHAR16 *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        CHAR16 ca = *a++;
        CHAR16 cb = *b++;
        if (ca >= L'A' && ca <= L'Z') ca = (CHAR16)(ca + 32);
        if (cb >= L'A' && cb <= L'Z') cb = (CHAR16)(cb + 32);
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

int visor_cmdline_has_word(CHAR16 *cmdline, CHAR16 *word) {
    if (!cmdline || !word) return 0;
    UINTN wl = efi_strlen16(word);
    if (!wl) return 0;

    UINTN i = 0;
    while (cmdline[i]) {
        while (cmdline[i] == L' ') i++;
        UINTN start = i;
        while (cmdline[i] && cmdline[i] != L' ') i++;
        if (i - start == wl) {
            UINTN k = 0;
            while (k < wl && cmdline[start + k] == word[k]) k++;
            if (k == wl) return 1;
        }
    }
    return 0;
}

int mem_equal(const void *a, const void *b, UINTN n) {
    const UINT8 *x = (const UINT8*)a, *y = (const UINT8*)b;
    UINT8 diff = 0;
    for (UINTN i = 0; i < n; i++) diff |= x[i] ^ y[i];
    return diff == 0;
}

CHAR16* efi_strchr(CHAR16 *s, CHAR16 c) {
    while (*s && *s != c) s++;
    return (*s == c) ? s : NULL;
}

void efi_sleep(UINTN milliseconds) {
    if (!visor_boot_services_active) return;
    BS->Stall(milliseconds * 1000);
}

int efi_key_pending(void) {
    if (!visor_boot_services_active || !ST || !ST->ConIn) return 0;
    if (!ST->ConIn->WaitForKey) return 0;
    return BS->CheckEvent(ST->ConIn->WaitForKey) == EFI_SUCCESS;
}

UINT64 efi_get_tick(void) {
    return arch_now_us() / 1000;
}

EFI_HANDLE* efi_locate_handle_buffer(EFI_GUID *proto, UINTN *count) {
    EFI_HANDLE *buffer = NULL;
    EFI_STATUS status = BS->LocateHandleBuffer(ByProtocol, proto, NULL, count, &buffer);
    if (EFI_ERROR(status)) {
        *count = 0;
        return NULL;
    }
    return buffer;
}

UINT32 efi_rand(void) {
    static UINT32 state = 0;
    if (!state) {
        EFI_TIME t;
        if (!EFI_ERROR(RT->GetTime(&t, NULL)))
            state = t.Nanosecond ^ ((UINT32)t.Second << 24) ^
                    ((UINT32)t.Minute << 16) ^ ((UINT32)t.Hour << 8) ^ t.Day;
        state ^= (UINT32)efi_get_tick();
        if (!state) state = 0x2545F491;
    }
    state = state * 1103515245u + 12345u;
    return state;
}
