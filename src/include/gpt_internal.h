/* gpt_internal.h - shared internals of the src/gpt/gpt.c translation units. */
#ifndef GPT_INTERNAL_H
#define GPT_INTERNAL_H

#include <efi.h>
#include <efilib.h>

#include "gpt.h"

void* efi_allocate_pool(UINTN size);
void  efi_free_pool(void *ptr);


static inline UINT16 le16(const UINT8 *p) {
    return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}

static inline UINT32 le32(const UINT8 *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) |
           ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static inline UINT64 le64(const UINT8 *p) {
    return (UINT64)le32(p) | ((UINT64)le32(p + 4) << 32);
}

static inline void put_le32(UINT8 *p, UINT32 v) {
    p[0] = (UINT8)(v);
    p[1] = (UINT8)(v >> 8);
    p[2] = (UINT8)(v >> 16);
    p[3] = (UINT8)(v >> 24);
}

static inline void put_le64(UINT8 *p, UINT64 v) {
    put_le32(p, (UINT32)v);
    put_le32(p + 4, (UINT32)(v >> 32));
}

static inline int guid_is_zero(const UINT8 g[16]) {
    for (int i = 0; i < 16; i++) if (g[i]) return 0;
    return 1;
}

static inline int guid_is_valid(const UINT8 g[16]) {

    return !guid_is_zero(g);
}
#define RCOPY(dest, src, n)  CopyMem((dest), (void*)(const void*)(src), (n))
void* gpt_alloc(UINTN size);
void gpt_free(void *p);
gpt_status_t gpt_parse_header(const gpt_dev_t *dev, UINT64 expected_lba,
                                     const UINT8 *sector, gpt_table_t *t);
void gpt_check_mbr(gpt_diag_t *diag, const UINT8 *lba0);
void gpt_load_ct(gpt_dev_t *dev, UINT64 lba, gpt_table_t *t, int copy);
void gpt_compare(const gpt_diag_t *diag, gpt_cmp_t *cmp);
void gpt_diag_reset(gpt_diag_t *diag);

#endif
