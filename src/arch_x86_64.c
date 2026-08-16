#include "arch.h"
#include <efi.h>
#include <efilib.h>

extern EFI_BOOT_SERVICES *BS;

static UINT64 tsc_per_us = 0;

void arch_clock_init(void) {
    if (tsc_per_us) return;
    UINT64 t0 = __builtin_ia32_rdtsc();
    BS->Stall(10000);
    UINT64 elapsed = __builtin_ia32_rdtsc() - t0;
    tsc_per_us = elapsed / 10000;
    if (tsc_per_us == 0) tsc_per_us = 1;
}

UINT64 arch_now_us(void) {
    if (!tsc_per_us) arch_clock_init();
    return __builtin_ia32_rdtsc() / tsc_per_us;
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

#define MSR_MTRRCAP        0x0FE
#define MSR_MTRR_DEF_TYPE  0x2FF
#define MSR_MTRR_PHYSBASE0 0x200
#define MTRR_TYPE_WC       0x01
#define MSR_IA32_PAT       0x277

#define PTE_P     (1ull << 0)
#define PTE_PWT   (1ull << 3)
#define PTE_PCD   (1ull << 4)
#define PTE_PS    (1ull << 7)
#define PTE_PAT4K (1ull << 7)
#define PTE_PATLG (1ull << 12)
#define PTE_ADDR  0x000FFFFFFFFFF000ull

static inline UINT64 x_rdmsr(UINT32 i){ UINT32 lo,hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(i)); return ((UINT64)hi<<32)|lo; }
static inline void   x_wrmsr(UINT32 i, UINT64 v){ __asm__ volatile("wrmsr"::"c"(i),"a"((UINT32)v),"d"((UINT32)(v>>32))); }
static inline void   x_wbinvd(void){ __asm__ volatile("wbinvd":::"memory"); }
static inline UINT64 x_rdcr0(void){ UINT64 v; __asm__ volatile("mov %%cr0,%0":"=r"(v)); return v; }
static inline void   x_wrcr0(UINT64 v){ __asm__ volatile("mov %0,%%cr0"::"r"(v):"memory"); }
static inline UINT64 x_rdcr3(void){ UINT64 v; __asm__ volatile("mov %%cr3,%0":"=r"(v)); return v; }
static inline void   x_wrcr3(UINT64 v){ __asm__ volatile("mov %0,%%cr3"::"r"(v):"memory"); }
static inline UINT64 x_rdcr4(void){ UINT64 v; __asm__ volatile("mov %%cr4,%0":"=r"(v)); return v; }
static inline void   x_wrcr4(UINT64 v){ __asm__ volatile("mov %0,%%cr4"::"r"(v):"memory"); }
static inline void   x_cpuid(UINT32 leaf, UINT32 *a, UINT32 *b, UINT32 *c, UINT32 *d){
    __asm__ volatile("cpuid":"=a"(*a),"=b"(*b),"=c"(*c),"=d"(*d):"a"(leaf),"c"(0)); }

static int mtrr_add_wc(UINT64 base, UINT64 size) {
    UINT64 cap = x_rdmsr(MSR_MTRRCAP);
    UINT32 vcnt = (UINT32)(cap & 0xFF);
    if (!(cap & (1ull << 10)) || vcnt == 0) return 0;

    UINT32 a, b, c, d, maxphys = 36;
    x_cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000008) { x_cpuid(0x80000008, &a, &b, &c, &d); maxphys = a & 0xFF; }
    if (maxphys < 32 || maxphys > 52) maxphys = 40;
    UINT64 pmask = (maxphys >= 64 ? ~0ull : ((1ull << maxphys) - 1)) & ~0xFFFull;

    UINT64 sz = 0x1000;
    while (sz < size) sz <<= 1;
    if (base & (sz - 1)) return 0;

    int slot = -1;
    for (UINT32 i = 0; i < vcnt; i++) {
        UINT64 pm = x_rdmsr(MSR_MTRR_PHYSBASE0 + 2*i + 1);
        UINT64 pb = x_rdmsr(MSR_MTRR_PHYSBASE0 + 2*i);
        if (!(pm & (1ull << 11))) { if (slot < 0) slot = (int)i; continue; }
        if ((pb & 0xFF) == MTRR_TYPE_WC && (pb & pmask) == (base & pmask)) return 1;
    }
    if (slot < 0) return 0;

    UINT64 nb = (base & pmask) | MTRR_TYPE_WC;
    UINT64 nm = ((~(sz - 1)) & pmask) | (1ull << 11);

    UINT64 flags; __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    UINT64 cr0 = x_rdcr0();
    x_wrcr0((cr0 & ~(1ull << 29)) | (1ull << 30));
    x_wbinvd();
    x_wrcr3(x_rdcr3());
    UINT64 deftype = x_rdmsr(MSR_MTRR_DEF_TYPE);
    x_wrmsr(MSR_MTRR_DEF_TYPE, deftype & ~(1ull << 11));

    x_wrmsr(MSR_MTRR_PHYSBASE0 + 2*slot,     nb);
    x_wrmsr(MSR_MTRR_PHYSBASE0 + 2*slot + 1, nm);

    x_wrmsr(MSR_MTRR_DEF_TYPE, deftype | (1ull << 11));
    x_wbinvd();
    x_wrcr3(x_rdcr3());
    x_wrcr0(cr0);
    if (flags & 0x200) __asm__ volatile("sti");
    return 1;
}

static void pat_set_slot1_wc(void) {
    UINT64 flags; __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    UINT64 cr4 = x_rdcr4();
    if (cr4 & (1ull << 7)) x_wrcr4(cr4 & ~(1ull << 7));
    UINT64 cr0 = x_rdcr0();
    x_wrcr0((cr0 & ~(1ull << 29)) | (1ull << 30));
    x_wbinvd();
    x_wrcr3(x_rdcr3());
    UINT64 pat = x_rdmsr(MSR_IA32_PAT);
    pat = (pat & ~(0xFFull << 8)) | (0x01ull << 8);
    x_wrmsr(MSR_IA32_PAT, pat);
    x_wbinvd();
    x_wrcr3(x_rdcr3());
    x_wrcr0(cr0);
    if (cr4 & (1ull << 7)) x_wrcr4(cr4);
    if (flags & 0x200) __asm__ volatile("sti");
}

static UINT64 pte_set_wc(UINT64 va) {
    UINT64 *pml4 = (UINT64*)(x_rdcr3() & PTE_ADDR);
    UINT64 *e = &pml4[(va >> 39) & 0x1FF];
    if (!(*e & PTE_P)) return 0;
    UINT64 *pdpt = (UINT64*)(*e & PTE_ADDR);
    e = &pdpt[(va >> 30) & 0x1FF];
    if (!(*e & PTE_P)) return 0;
    if (*e & PTE_PS) { *e = (*e & ~(PTE_PCD | PTE_PATLG)) | PTE_PWT; return 1ull << 30; }
    UINT64 *pd = (UINT64*)(*e & PTE_ADDR);
    e = &pd[(va >> 21) & 0x1FF];
    if (!(*e & PTE_P)) return 0;
    if (*e & PTE_PS) { *e = (*e & ~(PTE_PCD | PTE_PATLG)) | PTE_PWT; return 1ull << 21; }
    UINT64 *pt = (UINT64*)(*e & PTE_ADDR);
    e = &pt[(va >> 12) & 0x1FF];
    if (!(*e & PTE_P)) return 0;
    *e = (*e & ~(PTE_PCD | PTE_PAT4K)) | PTE_PWT;
    return 1ull << 12;
}

static int fb_pt_set_wc(UINT64 base, UINT64 size) {
    UINT64 flags; __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    UINT64 cr0 = x_rdcr0();
    x_wrcr0(cr0 & ~(1ull << 16));

    int ok = 1;
    UINT64 va = base & ~0xFFFull, end = base + size;
    while (va < end) {
        UINT64 pg = pte_set_wc(va);
        if (!pg) { ok = 0; break; }
        va = (va & ~(pg - 1)) + pg;
    }

    x_wrcr0(cr0);

    UINT64 cr4 = x_rdcr4();
    if (cr4 & (1ull << 7)) { x_wrcr4(cr4 & ~(1ull << 7)); x_wrcr4(cr4); }
    else x_wrcr3(x_rdcr3());
    if (flags & 0x200) __asm__ volatile("sti");
    return ok;
}

const CHAR16* arch_fb_make_wc(UINT64 base, UINT64 size) {
    static EFI_GUID cpu_guid = { 0x26baccb1, 0x6f42, 0x11d4,
        { 0xbc, 0xe7, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
    if (!base || !size) return L"none";

    const CHAR16 *method = L"none";

    visor_cpu_arch_t *cpu = NULL;
    if (!EFI_ERROR(BS->LocateProtocol(&cpu_guid, NULL, (void**)&cpu)) && cpu && cpu->SetMemoryAttributes
        && !EFI_ERROR(cpu->SetMemoryAttributes(cpu, base, size, EFI_MEMORY_WC)))
        method = L"cpu-arch";

    int have_mtrr = mtrr_add_wc(base, size);

    static int pat_ready = 0;
    if (!pat_ready) { pat_set_slot1_wc(); pat_ready = 1; }
    int have_pat = fb_pt_set_wc(base, size);

    if (have_pat)       method = have_mtrr ? L"pat+mtrr" : L"pat";
    else if (have_mtrr) method = L"mtrr";
    if (method[0] == L'n' && cpu) method = L"cpu-arch";
    return method;
}

