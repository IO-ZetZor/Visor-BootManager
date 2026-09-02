#ifndef VISOR_GPT_H
#define VISOR_GPT_H

#include <efi.h>

#define GPT_HEADER_SIZE_MIN     92u

#define GPT_SECTOR_SIZE_MIN     512u
#define GPT_SECTOR_SIZE_MAX     4096u

#define GPT_ENTRY_SIZE_MIN      128u
#define GPT_ENTRY_SIZE_MAX      4096u
#define GPT_ENTRY_COUNT_MAX     1024u
#define GPT_ARRAY_BYTES_MAX     (512u * 1024u)

#define GPT_NAME_CHARS          36u
#define GPT_REVISION_1_0        0x00010000u

/* Smallest disk that can hold MBR + both headers + two 16 KiB entry arrays. */
#define GPT_MIN_DISK_SECTORS    67u

#define GPT_SIGNATURE           0x5452415020494645ULL

#define GPT_MBR_PROTECTIVE_TYPE 0xEEu

typedef enum {
    GPT_UNKNOWN = 0,    
    GPT_VALID,
    GPT_WARNING,        
    GPT_INVALID,
    GPT_UNSAFE          
} gpt_status_t;

typedef enum {
    GPT_R_NONE = 0,
    GPT_R_DISK_READ_FAILED,
    GPT_R_DISK_WRITE_FAILED,
    GPT_R_DISK_FLUSH_FAILED,
    GPT_R_OUT_OF_MEMORY,
    GPT_R_BAD_SECTOR_SIZE,
    GPT_R_DISK_TOO_SMALL,
    GPT_R_NOT_GPT,
    GPT_R_INVALID_SIGNATURE,
    GPT_R_INVALID_REVISION,
    GPT_R_INVALID_HEADER_SIZE,
    GPT_R_HEADER_CRC_MISMATCH,
    GPT_R_INVALID_MY_LBA,
    GPT_R_INVALID_ALTERNATE_LBA,
    GPT_R_INVALID_USABLE_RANGE,
    GPT_R_INVALID_ENTRY_COUNT,
    GPT_R_INVALID_ENTRY_SIZE,
    GPT_R_ARRAY_TOO_LARGE,
    GPT_R_ARRAY_OFF_DISK,
    GPT_R_ARRAY_IN_USABLE_RANGE,
    GPT_R_ARRAY_CRC_MISMATCH,
    GPT_R_INVALID_PARTITION_RANGE,
    GPT_R_PARTITION_OFF_DISK,
    GPT_R_PARTITION_OVERLAP,
    GPT_R_METADATA_OVERLAP,
    GPT_R_INVALID_PARTITION_GUID,
    GPT_R_DUPLICATE_PARTITION_GUID,
    GPT_R_HALF_USED_ENTRY,
    GPT_R_MBR_NO_SIGNATURE,
    GPT_R_MBR_NOT_PROTECTIVE,
    GPT_R_MBR_HYBRID,
    GPT_R_MBR_WRONG_SIZE,
    GPT_R_GPT_COPIES_DIFFER,
    GPT_R_BACKUP_NOT_AT_END,
    GPT_R_UNSAFE_RECOVERY,
    GPT_R_NO_SAFE_DESTINATION,
    GPT_R_DISK_CHANGED,
    GPT_R_READ_ONLY_MEDIA,
    GPT_R_POST_WRITE_VERIFY_FAILED,
    GPT_R_COUNT
} gpt_reason_t;

const CHAR16* gpt_reason_text(gpt_reason_t reason);
const CHAR16* gpt_status_text(gpt_status_t status);

#define GPT_COPY_NONE     (-1)
#define GPT_COPY_PRIMARY  0
#define GPT_COPY_BACKUP   1

typedef struct gpt_dev {
    void   *ctx;                
    CHAR16  name[40];           
    UINT32  sector_size;        
    UINT64  total_sectors;      
    UINT32  media_id;           
    int     read_only;
    int     removable;

    EFI_STATUS (*read)(struct gpt_dev *dev, UINT64 lba, UINTN sectors, void *buf);
    EFI_STATUS (*write)(struct gpt_dev *dev, UINT64 lba, UINTN sectors, const void *buf);
    EFI_STATUS (*flush)(struct gpt_dev *dev);
} gpt_dev_t;

typedef struct {
    UINT64 signature;
    UINT32 revision;
    UINT32 header_size;
    UINT32 header_crc32;
    UINT32 reserved;
    UINT64 current_lba;
    UINT64 backup_lba;
    UINT64 first_usable_lba;
    UINT64 last_usable_lba;
    UINT8  disk_guid[16];
    UINT64 entry_lba;
    UINT32 entry_count;
    UINT32 entry_size;
    UINT32 entries_crc32;
} gpt_header_t;

typedef struct {
    UINT8  type_guid[16];
    UINT8  uniq_guid[16];
    UINT64 first_lba;
    UINT64 last_lba;
    UINT64 attributes;
    UINT16 name[GPT_NAME_CHARS];    
    int    used;
} gpt_entry_t;

typedef struct {
    int          present;           
    gpt_header_t hdr;

    gpt_status_t header_status;
    gpt_reason_t header_reason;
    UINT32       header_crc_stored;
    UINT32       header_crc_computed;
    int          header_crc_ok;

    int          geometry_ok;

UINT8       *raw;             
    UINTN        raw_bytes;
    UINT8       *hdr_raw;         
    UINTN        hdr_raw_bytes;   
    UINT64       array_sectors;
    int          loaded;            
    int          best_effort;       

    gpt_status_t entries_status;    
    gpt_reason_t entries_reason;
    UINT32       entries_crc_stored;
    UINT32       entries_crc_computed;
    int          entries_crc_ok;

    gpt_status_t layout_status;     
    gpt_reason_t layout_reason;
    INTN         layout_bad;        
    INTN         layout_bad2;       

    gpt_entry_t *ents;
    UINT32       ent_count;
    UINT32       used_count;
} gpt_table_t;

#define GPT_DIFF_DISK_GUID      (1u << 0)
#define GPT_DIFF_ENTRY_COUNT    (1u << 1)
#define GPT_DIFF_ENTRY_SIZE     (1u << 2)
#define GPT_DIFF_FIRST_USABLE   (1u << 3)
#define GPT_DIFF_LAST_USABLE    (1u << 4)
#define GPT_DIFF_ARRAY_CRC      (1u << 5)
#define GPT_DIFF_PART_COUNT     (1u << 6)
#define GPT_DIFF_PART_TYPE      (1u << 7)
#define GPT_DIFF_PART_GUID      (1u << 8)
#define GPT_DIFF_PART_RANGE     (1u << 9)
#define GPT_DIFF_PART_ATTRS     (1u << 10)
#define GPT_DIFF_PART_NAME      (1u << 11)

typedef enum {
    GPT_CMP_UNKNOWN = 0,
    GPT_CMP_IDENTICAL,
    GPT_CMP_DIFFERENT,
    GPT_CMP_PRIMARY_INVALID,
    GPT_CMP_BACKUP_INVALID,
    GPT_CMP_BOTH_INVALID,
    GPT_CMP_AMBIGUOUS       
} gpt_cmp_kind_t;

typedef struct {
    gpt_cmp_kind_t kind;
    UINT32 diff;            
    INTN   first_diff;      
    int    raw_identical;   
    int    crc_only;        
} gpt_cmp_t;

typedef enum {
    GPT_CLASS_UNKNOWN = 0,
    GPT_CLASS_NOT_GPT,                      
    GPT_CLASS_HEALTHY,
    GPT_CLASS_PRIMARY_CORRUPT_BACKUP_VALID, 
    GPT_CLASS_BACKUP_CORRUPT_PRIMARY_VALID, 
    GPT_CLASS_BOTH_COPIES_CORRUPT,
    GPT_CLASS_COPIES_DIFFER,
    GPT_CLASS_INVALID_LAYOUT,
    GPT_CLASS_UNSAFE_TO_RECOVER
} gpt_class_t;

typedef enum {
    GPT_RECOVER_NONE = 0,           
    GPT_RECOVER_PRIMARY_FROM_BACKUP,
    GPT_RECOVER_MANUAL_ONLY         
} gpt_recover_t;

const CHAR16* gpt_class_text(gpt_class_t klass);

#define GPT_NOTE_MAX 24

typedef struct {
    gpt_reason_t reason;
    gpt_status_t severity;      
    int          copy;          
    INTN         index;         
    INTN         index2;        
} gpt_note_t;

typedef struct {

    UINT32 sector_size;
    UINT64 total_sectors;
    UINT32 media_id;
    int    read_only;
    int    removable;

    gpt_status_t mbr_status;
    gpt_reason_t mbr_reason;
    int          mbr_protective;
    int          mbr_other_parts;

    gpt_table_t  primary;
    gpt_table_t  backup;

    gpt_cmp_t     cmp;
    gpt_class_t   klass;
    gpt_recover_t capability;
    gpt_status_t  overall;

    gpt_note_t notes[GPT_NOTE_MAX];
    UINTN      note_count;
    int        notes_truncated;
} gpt_diag_t;

typedef struct {

    UINT32 sector_size;
    UINT64 total_sectors;
    UINT32 media_id;

    UINT64 src_header_lba;
    UINT64 src_entries_lba;
    UINT32 src_header_crc;
    UINT32 src_entries_crc;
    UINT8  src_disk_guid[16];

    UINT64 dst_header_lba;
    UINT64 dst_entries_lba;

    UINT32 entry_count;
    UINT32 entry_size;
    UINT64 entry_array_bytes;
    UINT64 entry_array_sectors;

    UINT64 first_usable_lba;
    UINT64 last_usable_lba;
    UINT64 backup_lba;

    UINT32 new_entries_crc;
    UINT32 new_header_crc;

    UINT8 *entry_bytes;         
    UINT8 *header_sector;       

    UINTN  part_count;          

    gpt_status_t safety;        
    gpt_reason_t reason;
} gpt_plan_t;

typedef struct {
    int        success;
    gpt_reason_t reason;
    EFI_STATUS status;          

    int wrote_entries;
    int wrote_header;
    int flushed;

    int verify_primary_ok;
    int verify_backup_ok;
    int verify_match;

    int        have_preimage;  
    UINT8     *preimage;       
    UINTN      preimage_bytes; 

    gpt_diag_t after;           
    int        have_after;
} gpt_result_t;

UINT32 gpt_crc32(UINT32 seed, const UINT8 *data, UINTN len);

EFI_STATUS gpt_diagnose(gpt_dev_t *dev, int full, gpt_diag_t *out);

void gpt_diag_free(gpt_diag_t *diag);

int  gpt_build_primary_plan(const gpt_diag_t *diag, gpt_plan_t *plan);
void gpt_plan_free(gpt_plan_t *plan);

EFI_STATUS gpt_read_preimage(gpt_dev_t *dev, const gpt_plan_t *plan,
                             UINT8 **out, UINTN *out_bytes);

EFI_STATUS gpt_execute_plan(gpt_dev_t *dev, const gpt_plan_t *plan,
                            gpt_result_t *result);
void gpt_result_free(gpt_result_t *result);

const CHAR16* gpt_type_name(const UINT8 type_guid[16]);   
void gpt_format_guid(const UINT8 guid[16], CHAR16 *out, UINTN cap);
void gpt_entry_name(const gpt_entry_t *entry, CHAR16 *out, UINTN cap);
void gpt_note_text(const gpt_note_t *note, CHAR16 *out, UINTN cap);

gpt_status_t gpt_table_status(const gpt_table_t *table);

int gpt_table_is_source(const gpt_table_t *table);

#endif 

