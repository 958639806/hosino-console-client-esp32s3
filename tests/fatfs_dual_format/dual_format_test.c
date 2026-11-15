/* Host-only FatFs regression test. No file/device I/O is implemented here.
 * The simulated disk consists solely of sparse, zero-filled memory pages.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ff.h"
#include "diskio.h"

#if FF_FS_EXFAT != 1 || FF_LBA64 != 0 || FF_USE_LFN != 3 || FF_LFN_UNICODE != 2 || FF_CODE_PAGE != 936
#error Expected project exFAT, 32-bit LBA, heap LFN, UTF-8 and CP936 configuration
#endif
typedef char require_fsize_64[(sizeof(FSIZE_t) == 8) ? 1 : -1];
typedef char require_lba_32[(sizeof(LBA_t) == 4) ? 1 : -1];

#define PAGE_BYTES 4096U
#define PAGE_SECTORS (PAGE_BYTES / 512U)
#define BUCKETS 8192U
#define DATA_BYTES (1024U * 1024U + 1237U)
#define APPEND_BYTES 1337U
#define CHECK(x) do { if (!(x)) fail(#x, __LINE__); } while (0)
#define OK(call) do { FRESULT r_ = (call); if (r_ != FR_OK) { \
    fprintf(stderr, "FatFs result=%d at line %d: %s\n", r_, __LINE__, #call); exit(1); } } while (0)

typedef struct Page {
    uint32_t index;
    struct Page *next;
    BYTE data[PAGE_BYTES];
} Page;
static Page *pages[BUCKETS];
static LBA_t sector_count;
static size_t page_count;
static size_t live_lfn_allocations;
static size_t peak_lfn_allocations;
static uint64_t sector_reads, sector_writes;
static CRITICAL_SECTION locks[FF_VOLUMES + 1];
static int lock_exists[FF_VOLUMES + 1];

/* Explicit first MBR partition, like a normal removable SD card. */
PARTITION VolToPart[FF_VOLUMES] = {{0, 1}, {0, 1}};

static void fail(const char *expression, int line)
{
    fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    exit(1);
}

static Page *find_page(uint32_t index, int create)
{
    Page **slot = &pages[index % BUCKETS];
    while (*slot && (*slot)->index != index) slot = &(*slot)->next;
    if (!*slot && create) {
        *slot = (Page *)calloc(1, sizeof(Page));
        CHECK(*slot != NULL);
        (*slot)->index = index;
        ++page_count;
    }
    return *slot;
}

static int all_zero(const BYTE *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) if (p[i]) return 0;
    return 1;
}

static void reset_disk(LBA_t sectors)
{
    size_t i;
    for (i = 0; i < BUCKETS; ++i) {
        Page *p = pages[i];
        while (p) { Page *next = p->next; free(p); p = next; }
        pages[i] = NULL;
    }
    page_count = 0;
    sector_reads = sector_writes = 0;
    sector_count = sectors;
}

DSTATUS disk_initialize(BYTE drive) { return drive == 0 && sector_count ? 0 : STA_NOINIT; }
DSTATUS disk_status(BYTE drive) { return disk_initialize(drive); }

DRESULT disk_read(BYTE drive, BYTE *dst, LBA_t sector, UINT count)
{
    if (drive || !dst || !count || sector >= sector_count || count > sector_count - sector) return RES_PARERR;
    sector_reads += count;
    while (count--) {
        Page *p = find_page((uint32_t)(sector / PAGE_SECTORS), 0);
        if (p) memcpy(dst, p->data + (sector % PAGE_SECTORS) * 512U, 512);
        else memset(dst, 0, 512);
        ++sector;
        dst += 512;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE drive, const BYTE *src, LBA_t sector, UINT count)
{
    if (drive || !src || !count || sector >= sector_count || count > sector_count - sector) return RES_PARERR;
    sector_writes += count;
    while (count--) {
        Page *p = find_page((uint32_t)(sector / PAGE_SECTORS), !all_zero(src, 512));
        if (p) memcpy(p->data + (sector % PAGE_SECTORS) * 512U, src, 512);
        ++sector;
        src += 512;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void *out)
{
    if (drive) return RES_PARERR;
    switch (command) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)out = sector_count; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)out = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)out = 8; return RES_OK;
    case CTRL_TRIM: return RES_OK; /* Advisory only; retaining bytes is permitted. */
    default: return RES_PARERR;
    }
}

void *ff_memalloc(UINT size)
{
    void *p = malloc(size);
    if (p) {
        ++live_lfn_allocations;
        if (live_lfn_allocations > peak_lfn_allocations) peak_lfn_allocations = live_lfn_allocations;
    }
    return p;
}

void ff_memfree(void *p)
{
    if (p) { CHECK(live_lfn_allocations > 0); --live_lfn_allocations; free(p); }
}

int ff_mutex_create(int volume)
{
    CHECK(volume >= 0 && volume <= FF_VOLUMES);
    CHECK(!lock_exists[volume]);
    InitializeCriticalSection(&locks[volume]);
    lock_exists[volume] = 1;
    return 1;
}
void ff_mutex_delete(int volume)
{
    CHECK(lock_exists[volume]);
    DeleteCriticalSection(&locks[volume]);
    lock_exists[volume] = 0;
}
int ff_mutex_take(int volume) { CHECK(lock_exists[volume]); EnterCriticalSection(&locks[volume]); return 1; }
void ff_mutex_give(int volume) { LeaveCriticalSection(&locks[volume]); }
DWORD get_fattime(void) { return (46UL << 25) | (11UL << 21) | (11UL << 16) | (12UL << 11); }

static BYTE pattern(uint32_t offset)
{
    uint32_t x = offset * 2654435761U;
    x ^= x >> 13;
    return (BYTE)(x ^ (x >> 8) ^ 0xA5);
}

static uint32_t crc_byte(uint32_t crc, BYTE value)
{
    unsigned i;
    crc ^= value;
    for (i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    return crc;
}

static void write_pattern(FIL *file, uint32_t start, uint32_t length)
{
    BYTE data[7777];
    uint32_t offset = start, end = start + length;
    while (offset < end) {
        UINT i, written, count = end - offset;
        if (count > sizeof(data)) count = sizeof(data);
        for (i = 0; i < count; ++i) data[i] = pattern(offset + i);
        OK(f_write(file, data, count, &written));
        CHECK(written == count);
        offset += count;
    }
}

static uint32_t verify_file(const char *path, uint32_t size)
{
    FIL file;
    BYTE data[32003];
    uint32_t offset = 0, crc = 0xFFFFFFFFU;
    OK(f_open(&file, path, FA_READ));
    CHECK(f_size(&file) == (FSIZE_t)size);
    while (offset < size) {
        UINT i, got, request = size - offset;
        if (request > sizeof(data)) request = sizeof(data);
        OK(f_read(&file, data, request, &got));
        CHECK(got == request);
        for (i = 0; i < got; ++i) { CHECK(data[i] == pattern(offset + i)); crc = crc_byte(crc, data[i]); }
        offset += got;
    }
    {
        UINT got;
        OK(f_read(&file, data, 1, &got));
        CHECK(got == 0);
        OK(f_lseek(&file, 32761));
        OK(f_read(&file, data, 33, &got));
        CHECK(got == 33);
        for (got = 0; got < 33; ++got) CHECK(data[got] == pattern(32761 + got));
    }
    OK(f_close(&file));
    return crc ^ 0xFFFFFFFFU;
}

static void run_case(LBA_t sectors, BYTE format, const char *format_name)
{
    static const char folder[] = "0:/星野测试目录";
    static const char original[] = "0:/星野测试目录/中文长文件名 mixed-case 0123456789.bin";
    static const char renamed[] = "0:/星野测试目录/重命名后的校验文件 mixed-case 0123456789.bin";
    static const char leaf[] = "重命名后的校验文件 mixed-case 0123456789.bin";
    BYTE work[4096], mbr[512];
    FATFS fs, remount;
    FIL file;
    FILINFO info;
    FF_DIR dir;
    DWORD partitions[4] = {100, 0, 0, 0}, free_clusters;
    FATFS *mounted = NULL;
    MKFS_PARM options = {0};
    uint32_t crc_before, crc_after;
    int entries = 0;

    reset_disk(sectors);
    options.fmt = format;
    options.n_fat = 2;
    OK(f_fdisk(0, partitions, work));
    OK(f_mkfs("0:", &options, work, sizeof(work)));
    CHECK(disk_read(0, mbr, 0, 1) == RES_OK);
    CHECK(mbr[510] == 0x55 && mbr[511] == 0xAA && mbr[446 + 4] != 0);
    memset(&fs, 0, sizeof(fs));
    OK(f_mount(&fs, "0:", 1));
    CHECK(fs.fs_type == (format == FM_FAT32 ? FS_FAT32 : FS_EXFAT));
    OK(f_getfree("0:", &free_clusters, &mounted));
    CHECK(mounted == &fs && free_clusters > 0);
    OK(f_mkdir(folder));
    OK(f_open(&file, original, FA_WRITE | FA_CREATE_NEW));
    write_pattern(&file, 0, DATA_BYTES);
    OK(f_sync(&file));
    OK(f_close(&file));
    crc_before = verify_file(original, DATA_BYTES);
    OK(f_open(&file, original, FA_WRITE | FA_OPEN_APPEND));
    CHECK(f_tell(&file) == DATA_BYTES);
    write_pattern(&file, DATA_BYTES, APPEND_BYTES);
    OK(f_close(&file));
    OK(f_rename(original, renamed));
    CHECK(f_stat(original, &info) == FR_NO_FILE);
    OK(f_stat(renamed, &info));
    CHECK(info.fsize == DATA_BYTES + APPEND_BYTES);
    crc_after = verify_file(renamed, DATA_BYTES + APPEND_BYTES);
    CHECK(crc_before != crc_after);

    OK(f_mount(NULL, "0:", 0));
    memset(&fs, 0xCC, sizeof(fs)); /* Prove the second mount doesn't reuse its cache. */
    memset(&remount, 0, sizeof(remount));
    OK(f_mount(&remount, "0:", 1));
    CHECK(remount.fs_type == (format == FM_FAT32 ? FS_FAT32 : FS_EXFAT));
    CHECK(verify_file(renamed, DATA_BYTES + APPEND_BYTES) == crc_after);
    OK(f_opendir(&dir, folder));
    for (;;) {
        OK(f_readdir(&dir, &info));
        if (!info.fname[0]) break;
        CHECK(strcmp(info.fname, leaf) == 0);
        CHECK(info.fsize == DATA_BYTES + APPEND_BYTES);
        ++entries;
    }
    CHECK(entries == 1);
    OK(f_closedir(&dir));
    OK(f_unlink(renamed));
    OK(f_unlink(folder));
    OK(f_mount(NULL, "0:", 0));
    CHECK(live_lfn_allocations == 0);
    printf("PASS format=%s volume_MiB=%lu cluster_bytes=%lu payload=%u crc32=%08lx sparse_RAM_bytes=%lu\n",
           format_name, (unsigned long)(sectors / 2048U), (unsigned long)remount.csize * 512UL,
           DATA_BYTES + APPEND_BYTES, (unsigned long)crc_after, (unsigned long)page_count * PAGE_BYTES);
}

int main(void)
{
    printf("CONFIG EXFAT=%d LBA64=%d LFN=%d UTF8=%d CP=%d FSIZE=%u LBA=%u\n",
           FF_FS_EXFAT, FF_LBA64, FF_USE_LFN, FF_LFN_UNICODE, FF_CODE_PAGE,
           (unsigned)sizeof(FSIZE_t), (unsigned)sizeof(LBA_t));
    run_case(64U * 2048U, FM_FAT32, "FAT32");
    run_case(64U * 2048U, FM_EXFAT, "exFAT");
    run_case(128U * 1024U * 2048U, FM_FAT32, "FAT32");
    run_case(128U * 1024U * 2048U, FM_EXFAT, "exFAT");
    reset_disk(0);
    printf("PASS all four cases; peak simultaneous LFN allocations=%lu; no physical disk APIs\n",
           (unsigned long)peak_lfn_allocations);
    return 0;
}
