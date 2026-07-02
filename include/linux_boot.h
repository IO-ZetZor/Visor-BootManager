#ifndef LINUX_BOOT_H
#define LINUX_BOOT_H

#include <efi.h>
#include "gui.h"

typedef struct {
    UINT32 loaddr;
    UINT32 hiaddr;
    UINT32 entry;
} setup_header_32_t;

typedef struct {
    UINT8  setup_sects;
    UINT16 root_flags;
    UINT32 syssize;
    UINT16 ram_size;
    UINT16 vid_mode;
    UINT16 root_dev;
    UINT16 boot_flag;
    UINT16 jump;
    UINT32 header;
    UINT16 version;
    UINT32 realmode_swtch;
    UINT16 start_sys_seg;
    UINT16 kernel_version;
    UINT8  type_of_loader;
    UINT8  loadflags;
    UINT16 setup_move_size;
    UINT32 code32_start;
    UINT32 ramdisk_image;
    UINT32 ramdisk_size;
    UINT32 bootsect_kludge;
    UINT16 heap_end_ptr;
    UINT8  ext_loader_ver;
    UINT8  ext_loader_type;
    UINT32 cmd_line_ptr;
    UINT32 initrd_addr_max;
    UINT32 kernel_alignment;
    UINT8  relocatable_kernel;
    UINT8  min_alignment;
    UINT16 xloadflags;
    UINT32 cmdline_size;
    UINT32 hardware_subarch;
    UINT64 hardware_subarch_data;
    UINT32 payload_offset;
    UINT32 payload_length;
    UINT64 setup_data;
    UINT64 pref_address;
    UINT32 init_size;
    UINT32 handover_offset;
    UINT32 kernel_info_offset;
} __attribute__((packed)) setup_header_t;

typedef struct {
    UINT8  pad0[498];
    UINT16 pad1;
    UINT16 protocol_type;
    setup_header_t hdr;
    UINT8  pad2[512];
    UINT32 signature;
    UINT16 checksum;
} __attribute__((packed)) setup_block_t;

#define LINUX_SIGNATURE 0x53726452
#define HANDOVER_MASK   0x01

EFI_STATUS visor_boot(boot_entry_t *entry, EFI_SYSTEM_TABLE *st);

#endif
