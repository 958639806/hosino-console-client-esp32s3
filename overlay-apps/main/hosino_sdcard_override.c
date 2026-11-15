/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Apps-only wrapper for Waveshare ESP32-S3-Touch-AMOLED-2.16 SD mounting.
 * Pin assignment and mount options follow its BSP. Request SD high-speed
 * mode (40 MHz) and provide a permanently owned DMA buffer; never format.
 * Link with --wrap=bsp_sdcard_mount and --wrap=bsp_sdcard_unmount.
 * Call through storage_service, which serializes mount/unmount and users.
 */

#include <inttypes.h>
#include <stdint.h>

#include "bsp/esp32_s3_touch_amoled_2_16.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "diskio_sdmmc.h"
#include "freertos/FreeRTOS.h"
#include "hosino_sdcard_override.h"

static const char *TAG = "hosino_sdcard";

#define SD_DMA_BUFFER_BYTES (4U * 1024U)
#define SD_DMA_CHUNK_BLOCKS (SD_DMA_BUFFER_BYTES / 512U)

static portMUX_TYPE s_dma_lock = portMUX_INITIALIZER_UNLOCKED;
static void *s_dma_buffer;

esp_err_t __real_bsp_sdcard_unmount(void);

esp_err_t hosino_sdcard_dma_init(void)
{
    portENTER_CRITICAL(&s_dma_lock);
    const bool initialized = s_dma_buffer != NULL;
    portEXIT_CRITICAL(&s_dma_lock);
    if (initialized) {
        return ESP_OK;
    }

    // The S3 SDMMC peripheral cannot DMA PSRAM. Allocate this owner-specific
    // buffer before small GUI allocations fragment internal DMA memory.
    // IDF queries heap_caps_get_allocated_size() for this pointer, so a static
    // DMA_ATTR array cannot be substituted for this heap allocation.
    void *candidate = heap_caps_malloc(SD_DMA_BUFFER_BYTES,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (candidate == NULL) {
        ESP_LOGE(TAG, "Cannot reserve %u-byte SD DMA buffer", SD_DMA_BUFFER_BYTES);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_dma_lock);
    const bool publish = s_dma_buffer == NULL;
    if (publish) {
        s_dma_buffer = candidate;
    }
    portEXIT_CRITICAL(&s_dma_lock);
    if (!publish) {
        heap_caps_free(candidate);
    } else {
        ESP_LOGI(TAG, "Reserved fixed SD DMA buffer: %u bytes, max %u sectors/chunk",
                 SD_DMA_BUFFER_BYTES, SD_DMA_CHUNK_BLOCKS);
    }
    return ESP_OK;
}

_Static_assert(BSP_SD_CMD == GPIO_NUM_1 && BSP_SD_CLK == GPIO_NUM_2 &&
               BSP_SD_D0 == GPIO_NUM_3, "This override is for the 2.16 board");

static const char *filesystem_name(BYTE type)
{
    switch (type) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    case FS_EXFAT: return "exFAT";
    default: return "unknown";
    }
}

static const char *mounted_filesystem_name(const sdmmc_card_t *card)
{
    const BYTE drive = ff_diskio_get_pdrv_card(card);
    if (drive == 0xff || drive >= FF_VOLUMES || drive > 9) {
        ESP_LOGW(TAG, "Mounted card has no valid FatFs drive: %u", drive);
        return "unknown";
    }

    /* Match IDF's numeric FatFs drive mapping, not the VFS mount path or 0:.
     * Opening the root only obtains its filesystem object. No directory entry
     * is read, and no file is created or changed. Avoid f_getfree here because
     * it can scan the entire FAT/bitmap merely to identify the filesystem. */
    const TCHAR root[] = {(TCHAR)('0' + drive), ':', '/', 0};
    FF_DIR directory = {0};
    const FRESULT open_result = f_opendir(&directory, root);
    if (open_result != FR_OK) {
        ESP_LOGW(TAG, "Filesystem type query failed: FatFs=%d", open_result);
        return "unknown";
    }

    const char *name = filesystem_name(directory.obj.fs->fs_type);
    const FRESULT close_result = f_closedir(&directory);
    if (close_result != FR_OK) {
        ESP_LOGW(TAG, "Filesystem type query close failed: FatFs=%d", close_result);
    }
    return name;
}

esp_err_t __wrap_bsp_sdcard_mount(void)
{
    /* The matching unmount wrapper clears the BSP's dangling global. FatFs's
     * card lookup alone cannot detect unmount: IDF retains its old s_cards entry. */
    if (bsp_sdcard != NULL && ff_diskio_get_pdrv_card(bsp_sdcard) != 0xff) {
        ESP_LOGW(TAG, "SD is already mounted");
        return ESP_ERR_INVALID_STATE;
    }
    bsp_sdcard = NULL;

    // Normally already allocated by app_main. If startup allocation failed,
    // a later explicit mount may retry without proceeding with a NULL buffer.
    const esp_err_t buffer_result = hosino_sdcard_dma_init();
    if (buffer_result != ESP_OK) {
        return buffer_result;
    }

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    // Do not let sdmmc_card_init replace our buffer with its optional 512-byte
    // SDIO allocation. IDF reuses this 4 KiB buffer for unaligned/PSRAM sector
    // reads AND writes; FATFS's per-volume reentrant mutex serializes them.
    // The driver bounds each chunk by the actual heap allocation size too.
    host.flags &= ~SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host.dma_aligned_buffer = s_dma_buffer;
    host.unaligned_multi_block_rw_max_chunk_size = SD_DMA_CHUNK_BLOCKS;
    const sdmmc_slot_config_t slot_config = {
        .clk = BSP_SD_CLK,
        .cmd = BSP_SD_CMD,
        .d0 = BSP_SD_D0,
        .d1 = GPIO_NUM_NC,
        .d2 = GPIO_NUM_NC,
        .d3 = GPIO_NUM_NC,
        .d4 = GPIO_NUM_NC,
        .d5 = GPIO_NUM_NC,
        .d6 = GPIO_NUM_NC,
        .d7 = GPIO_NUM_NC,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = 0,
    };

    ESP_LOGI(TAG, "Mounting %s: SDMMC 1-bit, requested=%d kHz, "
             "CMD=%d CLK=%d D0=%d, format=off, exFAT=%s, fixed DMA=%u bytes",
             BSP_SD_MOUNT_POINT, host.max_freq_khz, BSP_SD_CMD, BSP_SD_CLK,
             BSP_SD_D0, FF_FS_EXFAT ? "enabled" : "disabled", SD_DMA_BUFFER_BYTES);

    const esp_err_t result = esp_vfs_fat_sdmmc_mount(
        BSP_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &bsp_sdcard);
    if (result != ESP_OK) {
        /* IDF may fill out_card before a later allocation fails and frees it.
         * No caller may retain that failed mount's possibly dangling pointer. */
        bsp_sdcard = NULL;
        ESP_LOGE(TAG, "SD mount failed at requested=%d kHz: %s (0x%x); "
                 "no formatting or lower-speed retry",
                 host.max_freq_khz, esp_err_to_name(result), (unsigned)result);
        return result;
    }

    int actual_khz = 0;
    const esp_err_t clock_result = sdmmc_host_get_real_freq(
        bsp_sdcard->host.slot, &actual_khz);
    if (clock_result != ESP_OK) {
        actual_khz = bsp_sdcard->real_freq_khz;
        ESP_LOGW(TAG, "Clock query failed (%s); using card's recorded clock",
                 esp_err_to_name(clock_result));
    }

    const uint64_t capacity_bytes = (uint64_t)(uint32_t)bsp_sdcard->csd.capacity *
                                    (uint32_t)bsp_sdcard->csd.sector_size;
    ESP_LOGI(TAG, "Mounted: filesystem=%s, capacity=%" PRIu64
             " bytes (%" PRIu64 " MiB), requested=%d kHz, actual=%d kHz",
             mounted_filesystem_name(bsp_sdcard), capacity_bytes,
             capacity_bytes / (1024U * 1024U), host.max_freq_khz, actual_khz);
    if (actual_khz > 0 && actual_khz < host.max_freq_khz) {
        ESP_LOGW(TAG, "Card/host negotiated below the requested high-speed clock");
    }
    return ESP_OK;
}

esp_err_t __wrap_bsp_sdcard_unmount(void)
{
    if (bsp_sdcard == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Storage-service leases serialize this call with every card user. IDF
    // releases its VFS context and card before the final VFS unregister result,
    // so even a late error may leave the old BSP pointer dangling. Never retain
    // or dereference it afterwards. The next mount allocates a fresh card.
    const esp_err_t result = __real_bsp_sdcard_unmount();
    bsp_sdcard = NULL;
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "SD unmount returned %s; cleared BSP card pointer",
                 esp_err_to_name(result));
    }
    // Neither the BSP nor IDF frees host.dma_aligned_buffer in this fixed
    // version. Keep our single allocation for the next mount; do not free it.
    return result;
}
