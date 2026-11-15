/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "storage_service.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "diskio_sdmmc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

/* Present in the 2.16 BSP implementation but absent from its public header. */
extern esp_err_t bsp_sdcard_unmount(void);

#define STORAGE_BENCHMARK_PATH_FORMAT STORAGE_SERVICE_MOUNT_POINT "/.waveshare-sd-benchmark-%08lx.tmp"
#define STORAGE_BENCHMARK_PATH_SIZE   96
#define STORAGE_BENCHMARK_CHUNK_SIZE (16 * 1024)

static const char *TAG = "storage_service";

static StaticSemaphore_t s_mutex_buffer;
static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;

static storage_service_state_t s_state = STORAGE_SERVICE_STATE_UNINITIALIZED;
static esp_err_t s_last_error = ESP_OK;
static uint32_t s_generation;
static uint32_t s_active_leases;
static bool s_volume_mounted;
static bool s_benchmark_active;
static bool s_safe_ejected;

static esp_err_t ensure_initialized(void)
{
    portENTER_CRITICAL(&s_init_lock);
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
        if (s_mutex != NULL) {
            s_state = STORAGE_SERVICE_STATE_UNMOUNTED;
            s_last_error = ESP_OK;
        }
    }
    portEXIT_CRITICAL(&s_init_lock);
    return s_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void clear_card_details(storage_service_info_t *info)
{
    info->card_name[0] = '\0';
    info->filesystem[0] = '\0';
    info->capacity_bytes = 0;
    info->filesystem_bytes = 0;
    info->free_bytes = 0;
    info->bus_width = 0;
    info->frequency_khz = 0;
}

static esp_err_t populate_card_details_locked(storage_service_info_t *info)
{
    clear_card_details(info);
    if (!s_volume_mounted || bsp_sdcard == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(info->card_name, bsp_sdcard->cid.name, sizeof(info->card_name));
    const BYTE drive = ff_diskio_get_pdrv_card(bsp_sdcard);
    if (drive < FF_VOLUMES && drive <= 9) {
        const TCHAR root[] = {(TCHAR)('0' + drive), ':', '/', 0};
        FF_DIR directory = {0};
        if (f_opendir(&directory, root) == FR_OK) {
            const BYTE type = directory.obj.fs->fs_type;
            const char *name = type == FS_EXFAT ? "exFAT" : type == FS_FAT32 ? "FAT32"
                               : type == FS_FAT16 ? "FAT16" : type == FS_FAT12 ? "FAT12" : "unknown";
            strlcpy(info->filesystem, name, sizeof(info->filesystem));
            (void)f_closedir(&directory);
        }
    }
    info->capacity_bytes =
        (uint64_t)bsp_sdcard->csd.capacity * (uint64_t)bsp_sdcard->csd.sector_size;
    if (bsp_sdcard->host.get_bus_width != NULL) {
        const size_t width = bsp_sdcard->host.get_bus_width(bsp_sdcard->host.slot);
        info->bus_width = width <= UINT32_MAX ? (uint32_t)width : 0;
    }
    if (info->bus_width == 0) {
        info->bus_width = 1U << bsp_sdcard->log_bus_width;
    }
    info->frequency_khz = bsp_sdcard->real_freq_khz > 0
                              ? (uint32_t)bsp_sdcard->real_freq_khz
                              : bsp_sdcard->max_freq_khz;

    uint64_t filesystem_total = 0;
    uint64_t filesystem_free = 0;
    const esp_err_t result = esp_vfs_fat_info(
        STORAGE_SERVICE_MOUNT_POINT, &filesystem_total, &filesystem_free
    );
    if (result == ESP_OK) {
        info->filesystem_bytes = filesystem_total;
        info->free_bytes = filesystem_free;
    }
    return result;
}

static esp_err_t probe_card_locked(void)
{
    if (!s_volume_mounted || bsp_sdcard == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = sdmmc_get_status(bsp_sdcard);
    if (result != ESP_OK) {
        return result;
    }

    uint64_t filesystem_total = 0;
    uint64_t filesystem_free = 0;
    return esp_vfs_fat_info(
        STORAGE_SERVICE_MOUNT_POINT, &filesystem_total, &filesystem_free
    );
}

static esp_err_t unmount_locked(void)
{
    if (!s_volume_mounted) {
        s_state = STORAGE_SERVICE_STATE_UNMOUNTED;
        s_last_error = ESP_OK;
        return ESP_OK;
    }

    s_state = STORAGE_SERVICE_STATE_EJECTING;
    const esp_err_t result = bsp_sdcard_unmount();
    if (result == ESP_OK || result == ESP_ERR_INVALID_STATE) {
        s_volume_mounted = false;
        s_state = STORAGE_SERVICE_STATE_UNMOUNTED;
        s_last_error = ESP_OK;
        ++s_generation;
        return ESP_OK;
    }

    s_state = STORAGE_SERVICE_STATE_ERROR;
    s_last_error = result;
    return result;
}

static esp_err_t mount_locked(bool probe_existing)
{
    if (!s_volume_mounted && s_active_leases != 0) {
        /* A live lease and an unmounted volume is an inconsistent state. Do
         * not hide it by mounting a new filesystem generation underneath the
         * owner of that lease. */
        return ESP_ERR_INVALID_STATE;
    }

    if (s_volume_mounted) {
        const bool recover_error = s_state == STORAGE_SERVICE_STATE_ERROR;
        if (recover_error && s_active_leases != 0) {
            return s_last_error != ESP_OK ? s_last_error : ESP_FAIL;
        }
        if (probe_existing && s_active_leases != 0) {
            // A lease can own active stdio/FatFs objects. Do not issue an
            // out-of-band card probe or remount underneath those objects. The
            // caller can retry the explicit rescan after the owners finish.
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t probe_result =
            (probe_existing || recover_error) ? probe_card_locked() : ESP_OK;
        if (probe_result == ESP_OK) {
            s_state = STORAGE_SERVICE_STATE_MOUNTED;
            s_last_error = ESP_OK;
            return ESP_OK;
        }

        s_last_error = probe_result;
        if (s_active_leases != 0) {
            s_state = STORAGE_SERVICE_STATE_ERROR;
            return probe_result;
        }

        ESP_LOGW(TAG, "Mounted volume probe failed (%s), remounting",
                 esp_err_to_name(probe_result));
        const esp_err_t unmount_result = unmount_locked();
        if (unmount_result != ESP_OK) {
            return unmount_result;
        }
    }

    s_state = STORAGE_SERVICE_STATE_MOUNTING;
    const esp_err_t result = bsp_sdcard_mount();
    if (result != ESP_OK) {
        s_volume_mounted = false;
        s_state = STORAGE_SERVICE_STATE_ERROR;
        s_last_error = result;
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(result));
        return result;
    }

    s_volume_mounted = true;
    s_state = STORAGE_SERVICE_STATE_MOUNTED;
    s_last_error = ESP_OK;
    ++s_generation;
    ESP_LOGI(TAG, "SD mounted at %s (generation %lu)", STORAGE_SERVICE_MOUNT_POINT,
             (unsigned long)s_generation);
    return ESP_OK;
}

static void fill_benchmark_pattern(uint8_t *buffer, size_t length)
{
    uint32_t value = 0x6d2b79f5U;
    for (size_t i = 0; i < length; ++i) {
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        buffer[i] = (uint8_t)(value ^ (uint32_t)i);
    }
}

static uint32_t elapsed_ms(int64_t start_us)
{
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    return (uint32_t)((elapsed_us + 999) / 1000);
}

static float throughput_mib_per_second(size_t bytes, uint32_t elapsed_time_ms)
{
    if (bytes == 0 || elapsed_time_ms == 0) {
        return 0.0f;
    }
    return ((float)bytes * 1000.0f) / ((float)elapsed_time_ms * 1024.0f * 1024.0f);
}

static void log_benchmark_failure(const char *stage, esp_err_t error, int error_number,
                                  const storage_service_benchmark_t *result)
{
    const int preserved_errno = errno;
    ESP_LOGE(TAG, "Benchmark failed: stage=%s result=%s errno=%d requested=%zu written=%zu read=%zu",
             stage, esp_err_to_name(error), error_number,
             result != NULL ? result->requested_bytes : 0,
             result != NULL ? result->written_bytes : 0,
             result != NULL ? result->read_bytes : 0);
    errno = preserved_errno;
}

static esp_err_t create_benchmark_file(char *path, size_t path_size, FILE **file,
                                       const storage_service_benchmark_t *result)
{
    if (path == NULL || path_size == 0 || file == NULL) {
        log_benchmark_failure("create_arguments", ESP_ERR_INVALID_ARG, 0, result);
        return ESP_ERR_INVALID_ARG;
    }
    path[0] = '\0';
    *file = NULL;

    const uint32_t seed = (uint32_t)esp_timer_get_time();
    for (uint32_t attempt = 0; attempt < 16; ++attempt) {
        const int path_length = snprintf(path, path_size, STORAGE_BENCHMARK_PATH_FORMAT,
                                         (unsigned long)(seed + attempt));
        if (path_length < 0 || (size_t)path_length >= path_size) {
            log_benchmark_failure("create_path", ESP_ERR_INVALID_SIZE, 0, result);
            path[0] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }

        const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) {
            if (errno == EEXIST) {
                continue;
            }
            log_benchmark_failure("create_open", ESP_FAIL, errno, result);
            path[0] = '\0';
            return ESP_FAIL;
        }

        *file = fdopen(fd, "w+b");
        if (*file != NULL) {
            return ESP_OK;
        }

        log_benchmark_failure("create_fdopen", ESP_FAIL, errno, result);
        (void)close(fd);
        (void)remove(path);
        path[0] = '\0';
        return ESP_FAIL;
    }

    log_benchmark_failure("create_name_collisions", ESP_ERR_NOT_FOUND, EEXIST, result);
    path[0] = '\0';
    return ESP_ERR_NOT_FOUND;
}

esp_err_t storage_service_init(void)
{
    return ensure_initialized();
}

esp_err_t storage_service_mount(void)
{
    esp_err_t result = ensure_initialized();
    if (result != ESP_OK) {
        return result;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    // An explicit rescan is the operation that re-arms on-demand mounting
    // after the user selected Safe eject.
    s_safe_ejected = false;
    result = mount_locked(true);
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t storage_service_acquire(storage_service_lease_t *lease)
{
    if (lease == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lease->active) {
        // Reusing a live token would increment the global owner count twice,
        // while a single release could only decrement it once.
        return ESP_ERR_INVALID_STATE;
    }
    lease->generation = 0;
    lease->active = false;

    esp_err_t result = ensure_initialized();
    if (result != ESP_OK) {
        return result;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_safe_ejected) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        /* The first lease after an idle period must validate the mounted card.
         * This recovers from an unexpected remove/reinsert without requiring a
         * Settings rescan. Never probe or remount underneath an active lease,
         * because it may still own FILE/FatFs objects. */
        result = mount_locked(s_active_leases == 0);
    }
    if (result == ESP_OK) {
        ++s_active_leases;
        lease->generation = s_generation;
        lease->active = true;
    }
    xSemaphoreGive(s_mutex);
    return result;
}

void storage_service_release(storage_service_lease_t *lease)
{
    if (lease == NULL || !lease->active || ensure_initialized() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (lease->generation == s_generation && s_active_leases > 0) {
        --s_active_leases;
    } else {
        ESP_LOGW(TAG, "Ignoring stale SD lease (lease generation %lu, current %lu)",
                 (unsigned long)lease->generation, (unsigned long)s_generation);
    }
    lease->generation = 0;
    lease->active = false;
    xSemaphoreGive(s_mutex);
}

esp_err_t storage_service_safe_eject(void)
{
    esp_err_t result = ensure_initialized();
    if (result != ESP_OK) {
        return result;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_active_leases != 0) {
        result = ESP_ERR_INVALID_STATE;
        s_last_error = result;
        ESP_LOGW(TAG, "Safe eject refused: %lu active lease(s)",
                 (unsigned long)s_active_leases);
    } else {
        result = unmount_locked();
        if (result == ESP_OK) {
            // Background producers (for example chat history) must not mount
            // the card again between this confirmation and physical removal.
            s_safe_ejected = true;
        }
    }
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t storage_service_get_info(storage_service_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));

    esp_err_t result = ensure_initialized();
    if (result != ESP_OK) {
        info->state = STORAGE_SERVICE_STATE_UNINITIALIZED;
        info->last_error = result;
        return result;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    info->state = s_state;
    info->last_error = s_last_error;
    info->generation = s_generation;
    info->active_leases = s_active_leases;
    if (s_volume_mounted) {
        const esp_err_t details_result = populate_card_details_locked(info);
        if (details_result != ESP_OK) {
            s_state = STORAGE_SERVICE_STATE_ERROR;
            s_last_error = details_result;
            info->state = s_state;
            info->last_error = s_last_error;
            result = details_result;
        }
    }
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t storage_service_run_benchmark(size_t bytes, storage_service_benchmark_t *result)
{
    if (result == NULL || bytes == 0) {
        log_benchmark_failure("arguments", ESP_ERR_INVALID_ARG, 0, NULL);
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->requested_bytes = bytes;
    result->result = ESP_FAIL;

    uint8_t *buffer = malloc(STORAGE_BENCHMARK_CHUNK_SIZE);
    if (buffer == NULL) {
        result->result = ESP_ERR_NO_MEM;
        log_benchmark_failure("allocate_buffer", result->result, errno, result);
        return result->result;
    }
    // Newlib's default 128-byte FILE buffer fragments a 16 KiB fread into
    // small VFS reads. This separate buffer belongs to the FILE until fclose;
    // never alias it with the application's read/write payload buffer.
    uint8_t *stdio_buffer = heap_caps_malloc(
        STORAGE_BENCHMARK_CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stdio_buffer == NULL) {
        result->result = ESP_ERR_NO_MEM;
        log_benchmark_failure("allocate_stdio_buffer", result->result, ENOMEM, result);
        free(buffer);
        return result->result;
    }
    fill_benchmark_pattern(buffer, STORAGE_BENCHMARK_CHUNK_SIZE);

    storage_service_lease_t lease = {0};
    esp_err_t operation_result = storage_service_acquire(&lease);
    if (operation_result != ESP_OK) {
        log_benchmark_failure("acquire_lease", operation_result, 0, result);
        heap_caps_free(stdio_buffer);
        free(buffer);
        result->result = operation_result;
        return operation_result;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_benchmark_active) {
        xSemaphoreGive(s_mutex);
        storage_service_release(&lease);
        heap_caps_free(stdio_buffer);
        free(buffer);
        result->result = ESP_ERR_INVALID_STATE;
        log_benchmark_failure("already_running", result->result, 0, result);
        return result->result;
    }
    s_benchmark_active = true;
    xSemaphoreGive(s_mutex);

    char benchmark_path[STORAGE_BENCHMARK_PATH_SIZE] = {0};
    FILE *file = NULL;
    uint64_t read_io_us = 0;
    uint64_t read_crc_us = 0;
    operation_result = create_benchmark_file(benchmark_path, sizeof(benchmark_path), &file, result);
    if (operation_result != ESP_OK) {
        goto cleanup;
    }
    errno = 0;
    if (setvbuf(file, (char *)stdio_buffer, _IOFBF, STORAGE_BENCHMARK_CHUNK_SIZE) != 0) {
        operation_result = ESP_FAIL;
        log_benchmark_failure("set_write_buffer", operation_result, errno, result);
        goto cleanup;
    }

    int64_t start_us = esp_timer_get_time();
    while (result->written_bytes < bytes) {
        const size_t remaining = bytes - result->written_bytes;
        const size_t chunk = remaining < STORAGE_BENCHMARK_CHUNK_SIZE
                                 ? remaining
                                 : STORAGE_BENCHMARK_CHUNK_SIZE;
        const size_t written = fwrite(buffer, 1, chunk, file);
        const int write_errno = errno;
        if (written > 0) {
            result->expected_crc32 =
                esp_crc32_le(result->expected_crc32, buffer, (uint32_t)written);
            result->written_bytes += written;
        }
        if (written != chunk) {
            operation_result = ESP_FAIL;
            log_benchmark_failure("write", operation_result, write_errno, result);
            break;
        }
    }
    if (operation_result == ESP_OK) {
        if (fflush(file) != 0) {
            operation_result = ESP_FAIL;
            log_benchmark_failure("flush", operation_result, errno, result);
        } else if (fsync(fileno(file)) != 0) {
            operation_result = ESP_FAIL;
            log_benchmark_failure("sync", operation_result, errno, result);
        }
    }
    result->write_time_ms = elapsed_ms(start_us);
    result->write_mib_per_s =
        throughput_mib_per_second(result->written_bytes, result->write_time_ms);
    if (fclose(file) != 0) {
        log_benchmark_failure("close_write", ESP_FAIL, errno, result);
        if (operation_result == ESP_OK) operation_result = ESP_FAIL;
    }
    file = NULL;
    if (operation_result != ESP_OK || result->written_bytes != bytes) {
        goto cleanup;
    }

    file = fopen(benchmark_path, "rb");
    if (file == NULL) {
        operation_result = ESP_FAIL;
        log_benchmark_failure("reopen_read", operation_result, errno, result);
        goto cleanup;
    }
    // The write stream has been closed; the same owned buffer can now be
    // attached to the read stream, before its first stdio operation.
    errno = 0;
    if (setvbuf(file, (char *)stdio_buffer, _IOFBF, STORAGE_BENCHMARK_CHUNK_SIZE) != 0) {
        operation_result = ESP_FAIL;
        log_benchmark_failure("set_read_buffer", operation_result, errno, result);
        goto cleanup;
    }

    start_us = esp_timer_get_time();
    while (result->read_bytes < bytes) {
        const size_t remaining = bytes - result->read_bytes;
        const size_t chunk = remaining < STORAGE_BENCHMARK_CHUNK_SIZE
                                 ? remaining
                                 : STORAGE_BENCHMARK_CHUNK_SIZE;
        const int64_t io_start_us = esp_timer_get_time();
        const size_t read = fread(buffer, 1, chunk, file);
        const int read_errno = errno;
        read_io_us += (uint64_t)(esp_timer_get_time() - io_start_us);
        if (read > 0) {
            const int64_t crc_start_us = esp_timer_get_time();
            result->actual_crc32 =
                esp_crc32_le(result->actual_crc32, buffer, (uint32_t)read);
            read_crc_us += (uint64_t)(esp_timer_get_time() - crc_start_us);
            result->read_bytes += read;
        }
        if (read != chunk) {
            operation_result = ESP_FAIL;
            log_benchmark_failure("read", operation_result, read_errno, result);
            break;
        }
    }
    result->read_time_ms = elapsed_ms(start_us);
    result->read_mib_per_s = throughput_mib_per_second(result->read_bytes, result->read_time_ms);
    if (fclose(file) != 0) {
        log_benchmark_failure("close_read", ESP_FAIL, errno, result);
        if (operation_result == ESP_OK) operation_result = ESP_FAIL;
    }
    file = NULL;
    if (operation_result == ESP_OK &&
            (result->read_bytes != bytes || result->actual_crc32 != result->expected_crc32)) {
        operation_result = ESP_ERR_INVALID_CRC;
        log_benchmark_failure("verify_crc", operation_result, 0, result);
    }

cleanup:
    if (file != NULL) {
        if (fclose(file) != 0) {
            log_benchmark_failure("cleanup_close", ESP_FAIL, errno, result);
        }
    }
    if (benchmark_path[0] != '\0' && remove(benchmark_path) != 0 && errno != ENOENT) {
        log_benchmark_failure("cleanup_remove", ESP_FAIL, errno, result);
        if (operation_result == ESP_OK) operation_result = ESP_FAIL;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_benchmark_active = false;
    xSemaphoreGive(s_mutex);
    storage_service_release(&lease);
    heap_caps_free(stdio_buffer);
    free(buffer);
    result->result = operation_result;
    ESP_LOGI(TAG, "Benchmark stdio=%u bytes: write_total_ms=%" PRIu32
             " read_total_ms=%" PRIu32 " read_io_us=%" PRIu64
             " read_crc_us=%" PRIu64 " written=%zu read=%zu result=%s",
             (unsigned)STORAGE_BENCHMARK_CHUNK_SIZE, result->write_time_ms,
             result->read_time_ms, read_io_us, read_crc_us,
             result->written_bytes, result->read_bytes, esp_err_to_name(operation_result));
    return operation_result;
}

const char *storage_service_state_name(storage_service_state_t state)
{
    switch (state) {
    case STORAGE_SERVICE_STATE_UNINITIALIZED:
        return "Uninitialized";
    case STORAGE_SERVICE_STATE_UNMOUNTED:
        return "Ejected";
    case STORAGE_SERVICE_STATE_MOUNTING:
        return "Mounting";
    case STORAGE_SERVICE_STATE_MOUNTED:
        return "Mounted";
    case STORAGE_SERVICE_STATE_EJECTING:
        return "Ejecting";
    case STORAGE_SERVICE_STATE_ERROR:
        return "Unavailable";
    default:
        return "Unknown";
    }
}
