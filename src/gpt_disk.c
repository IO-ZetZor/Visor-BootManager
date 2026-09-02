
#include "gpt_disk.h"
#include "efi_helpers.h"
#include <efilib.h>

extern EFI_BOOT_SERVICES *BS;

#define RCOPY(dst, src, n) CopyMem((void*)(dst), (void*)(UINTN)(src), (n))

typedef struct {
    EFI_HANDLE    handle;
    EFI_BLOCK_IO *bio;
    UINT32        open_media_id;    
    UINT32        open_block_size;  
    UINT64        open_last_block;  
} gpt_bio_t;

/* Re-check on every I/O that the medium behind this handle hasn't been
 * swapped. MediaId is the UEFI identity; when it moves, refuse to act. */
static EFI_STATUS bio_refresh_identity(gpt_dev_t *dev, gpt_bio_t *b) {
    EFI_BLOCK_IO_MEDIA *m = b->bio->Media;
    if (!m) return EFI_DEVICE_ERROR;
    if (m->MediaId   != b->open_media_id ||
        m->BlockSize != b->open_block_size ||
        m->LastBlock != b->open_last_block)
        return EFI_MEDIA_CHANGED;

    dev->read_only = m->ReadOnly;
    dev->removable = m->RemovableMedia;
    return EFI_SUCCESS;
}

static EFI_STATUS gpt_bio_read(gpt_dev_t *dev, UINT64 lba, UINTN sectors,
                               void *buf) {
    gpt_bio_t *b = dev->ctx;
    if (!b || !b->bio || !b->bio->Media || !b->bio->ReadBlocks)
        return EFI_DEVICE_ERROR;

    EFI_STATUS chk = bio_refresh_identity(dev, b);
    if (EFI_ERROR(chk)) return chk;

    UINTN   align = b->bio->Media->IoAlign > 1 ? b->bio->Media->IoAlign : 1;
    UINTN   bytes = (UINTN)sectors * dev->sector_size;
    UINT8  *raw = efi_allocate_pool(bytes + align);
    if (!raw) return EFI_OUT_OF_RESOURCES;
    UINT8  *tmp = (UINT8*)(((UINTN)raw + (align - 1)) & ~(align - 1));

    EFI_STATUS st = b->bio->ReadBlocks(b->bio, b->open_media_id, lba,
                                       bytes, tmp);
    if (!EFI_ERROR(st))
        CopyMem(buf, tmp, bytes);
    efi_free_pool(raw);
    return st;
}

static EFI_STATUS gpt_bio_write(gpt_dev_t *dev, UINT64 lba, UINTN sectors,
                                const void *buf) {
    gpt_bio_t *b = dev->ctx;
    if (!b || !b->bio || !b->bio->Media || !b->bio->WriteBlocks)
        return EFI_DEVICE_ERROR;

    EFI_STATUS chk = bio_refresh_identity(dev, b);
    if (EFI_ERROR(chk)) return chk;

    UINTN   align = b->bio->Media->IoAlign > 1 ? b->bio->Media->IoAlign : 1;
    UINTN   bytes = (UINTN)sectors * dev->sector_size;
    UINT8  *raw = efi_allocate_pool(bytes + align);
    if (!raw) return EFI_OUT_OF_RESOURCES;
    UINT8  *tmp = (UINT8*)(((UINTN)raw + (align - 1)) & ~(align - 1));

    CopyMem(tmp, (void*)buf, bytes);
    EFI_STATUS st = b->bio->WriteBlocks(b->bio, b->open_media_id, lba,
                                        bytes, tmp);
    efi_free_pool(raw);
    return st;
}

static EFI_STATUS gpt_bio_flush(gpt_dev_t *dev) {
    gpt_bio_t *b = dev->ctx;
    if (!b || !b->bio || !b->bio->FlushBlocks)
        return EFI_DEVICE_ERROR;
    EFI_STATUS chk = bio_refresh_identity(dev, b);
    if (EFI_ERROR(chk)) return chk;
    return b->bio->FlushBlocks(b->bio);
}

int gpt_disk_from_bio(gpt_dev_t *dev, EFI_HANDLE handle) {
    ZeroMem(dev, sizeof(*dev));
    if (!handle) return 0;

    EFI_BLOCK_IO *bio = NULL;
    if (EFI_ERROR(BS->HandleProtocol(handle, &gEfiBlockIoProtocolGuid,
                                     (void**)&bio)) || !bio || !bio->Media)
        return 0;

    EFI_BLOCK_IO_MEDIA *m = bio->Media;
    if (!m->MediaPresent) return 0;
    if (m->LogicalPartition) return 0;              
    if (m->BlockSize < GPT_SECTOR_SIZE_MIN ||
        m->BlockSize > GPT_SECTOR_SIZE_MAX) return 0;

    gpt_bio_t *b = efi_allocate_pool(sizeof(*b));
    if (!b) return 0;
    b->handle = handle;
    b->bio    = bio;
    b->open_media_id   = m->MediaId;
    b->open_block_size = m->BlockSize;
    b->open_last_block = m->LastBlock;

    dev->ctx             = b;
    SPrint((CHAR16*)dev->name, sizeof(dev->name), L"disk@%d",
           m->MediaId);
    dev->sector_size     = m->BlockSize;
    dev->total_sectors   = m->LastBlock + 1;
    dev->media_id        = m->MediaId;
    dev->read_only       = m->ReadOnly;
    dev->removable       = m->RemovableMedia;
    dev->read            = gpt_bio_read;
    dev->write           = gpt_bio_write;
    dev->flush           = gpt_bio_flush;

    if (dev->total_sectors < 2) {
        efi_free_pool(b);
        ZeroMem(dev, sizeof(*dev));
        return 0;
    }
    return 1;
}

void gpt_disk_close(gpt_dev_t *dev) {
    if (!dev) return;
    if (dev->ctx) {
        efi_free_pool(dev->ctx);
        dev->ctx = NULL;
    }
    dev->read = NULL;
    dev->write = NULL;
    dev->flush = NULL;
}

/* Open the disk whose Media->MediaId equals media_id. */
int gpt_disk_open_media_id(UINT32 media_id, gpt_dev_t *dev) {
    UINTN n = 0;
    EFI_HANDLE *hs = gpt_disk_enum(&n);
    if (!hs) return 0;
    int ok = 0;
    for (UINTN i = 0; i < n; i++) {
        EFI_BLOCK_IO *bio = NULL;
        if (EFI_ERROR(BS->HandleProtocol(hs[i], &gEfiBlockIoProtocolGuid,
                                         (void**)&bio)) || !bio || !bio->Media)
            continue;
        if (bio->Media->MediaId != media_id) continue;
        ok = gpt_disk_from_bio(dev, hs[i]);
        break;
    }
    efi_free_pool(hs);
    return ok;
}

EFI_HANDLE* gpt_disk_enum(UINTN *out_count) {
    if (!out_count) return NULL;
    UINTN n = *out_count;
    EFI_HANDLE *hs = efi_locate_handle_buffer(&gEfiBlockIoProtocolGuid, &n);
    if (!hs) return NULL;

    *out_count = 0;
    for (UINTN i = 0; i < n; i++) {
        EFI_BLOCK_IO *bio = NULL;
        if (EFI_ERROR(BS->HandleProtocol(hs[i], &gEfiBlockIoProtocolGuid,
                                         (void**)&bio)) || !bio || !bio->Media)
            continue;
        EFI_BLOCK_IO_MEDIA *m = bio->Media;
        if (!m->MediaPresent || m->LogicalPartition) continue;
        if (m->BlockSize < GPT_SECTOR_SIZE_MIN ||
            m->BlockSize > GPT_SECTOR_SIZE_MAX) continue;
        if (m->LastBlock < 1) continue;
        hs[(*out_count)++] = hs[i];
    }
    return hs;
}