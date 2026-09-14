#include "arch.h"
#include <efi.h>
#include <efilib.h>

extern EFI_BOOT_SERVICES *BS;

static UINT64 cnt_per_us = 0;

static inline UINT64 read_cntvct(void) {
    UINT64 v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline UINT64 read_cntfrq(void) {
    UINT64 v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

void arch_clock_init(void) {
    if (cnt_per_us) return;
    UINT64 hz = read_cntfrq();
    cnt_per_us = hz / 1000000;
    if (cnt_per_us == 0) cnt_per_us = 1;
}

UINT64 arch_now_us(void) {
    if (!cnt_per_us) arch_clock_init();
    return read_cntvct() / cnt_per_us;
}

typedef struct visor_cpu_arch visor_cpu_arch_t;
struct visor_cpu_arch {
    void *FlushDataCache;
    void *EnableInterrupt;
    void *DisableInterrupt;
    void *GetInterruptState;
    void *Init;
    void *RegisterInterruptHandler;
    void *GetTimerValue;
    EFI_STATUS (EFIAPI *SetMemoryAttributes)(visor_cpu_arch_t *This,
        EFI_PHYSICAL_ADDRESS BaseAddress, UINT64 Length, UINT64 Attributes);
};

const CHAR16* arch_fb_make_wc(UINT64 base, UINT64 size) {
    static EFI_GUID cpu_guid = { 0x26baccb1, 0x6f42, 0x11d4,
        { 0xbc, 0xe7, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
    if (!base || !size) return L"none";

    visor_cpu_arch_t *cpu = NULL;
    if (!EFI_ERROR(BS->LocateProtocol(&cpu_guid, NULL, (void**)&cpu)) && cpu && cpu->SetMemoryAttributes
        && !EFI_ERROR(cpu->SetMemoryAttributes(cpu, base, size, EFI_MEMORY_WC)))
        return L"cpu-arch";

    return L"none";
}
