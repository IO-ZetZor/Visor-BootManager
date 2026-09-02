#ifndef VISOR_GPT_DISK_H
#define VISOR_GPT_DISK_H

#include <efi.h>
#include "gpt.h"

int gpt_disk_from_bio(gpt_dev_t *dev, EFI_HANDLE handle);

int gpt_disk_open_media_id(UINT32 media_id, gpt_dev_t *dev);

void gpt_disk_close(gpt_dev_t *dev);

EFI_HANDLE* gpt_disk_enum(UINTN *out_count);

#endif 