#ifndef ARCH_H
#define ARCH_H

#include <efi.h>

void   arch_clock_init(void);
UINT64 arch_now_us(void);

int    arch_clock_since_power_on(void);

const CHAR16* arch_fb_make_wc(UINT64 base, UINT64 size);

#endif
