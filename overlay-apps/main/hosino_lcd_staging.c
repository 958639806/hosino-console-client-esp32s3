// SPDX-License-Identifier: Apache-2.0
/* Fixed color staging for the ESP32-S3/CO5300 480x480, B50x2 pipeline.
 *
 * IDF 5.5.5 esp_lcd_panel_io_spi.c:208-262 explicitly supports
 * tx_param(io, -1, NULL, 0): acquire bus, reap all queued transactions, send
 * neither command nor data, release bus. This drain MUST precede memcpy.
 * It also protects reuse after an earlier color submission queued one chunk
 * successfully and then failed before queueing its last chunk/callback.
 *
 * Keep one call to the original tx_color with the original command and size.
 * IDF splits at 32768 bytes and retains the original last-chunk completion
 * callback. This module never invokes a callback or lv_display_flush_ready,
 * changes the panel window, swaps bytes, clips pixels, or splits LVGL flushes.
 */
#include "sdkconfig.h"
#include "hosino_lcd_staging.h"

#include <string.h>
#include "driver/spi_master.h"
#include "esp_private/esp_cache_private.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Fixed LCD staging was audited for ESP32-S3 only"
#endif

static const char *TAG = "lcd_staging";
static esp_lcd_panel_io_handle_t s_io;
static uint8_t *s_buffer;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static hosino_lcd_staging_stats_t s_stats;

esp_err_t __real_esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io,
                                         int lcd_cmd, const void *color,
                                         size_t color_size);

esp_err_t hosino_lcd_staging_attach(esp_lcd_panel_io_handle_t io, size_t capacity)
{
    if (io == NULL || capacity != HOSINO_LCD_STAGING_BYTES) return ESP_ERR_INVALID_ARG;
    if (s_io != NULL) {
        return s_io == io && s_stats.capacity == capacity ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    size_t max_transaction = 0;
    esp_err_t result = spi_bus_get_max_transaction_len(SPI2_HOST, &max_transaction);
    if (result != ESP_OK) return result;
    // Internal SRAM on S3 is uncached. IDF therefore only requires aligned
    // starting addresses, not 4/64-byte color lengths: a 2-byte pixel is valid.
    size_t cache_alignment = 0;
    result = esp_cache_get_alignment(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL, &cache_alignment);
    if (result != ESP_OK) return result;
    // The buffer base is 64-byte aligned; 4-byte-aligned chunk boundaries
    // suffice for internal DMA too (e.g. a smaller bus maximum of 4092).
    if (cache_alignment > 1 || max_transaction == 0 || (max_transaction % 4) != 0) {
        ESP_LOGE(TAG, "Unexpected SPI DMA constraints: cache=%u max_chunk=%u",
                 (unsigned)cache_alignment, (unsigned)max_transaction);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t *buffer = heap_caps_aligned_alloc(
        64, capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Cannot reserve %u-byte fixed DMA buffer; largest=%u free=%u",
                 (unsigned)capacity,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        return ESP_ERR_NO_MEM;
    }
    if (!esp_ptr_internal(buffer) || !esp_ptr_dma_capable(buffer) || ((uintptr_t)buffer % 64) != 0) {
        heap_caps_free(buffer);
        return ESP_ERR_INVALID_STATE;
    }
    // Static mutex storage avoids another general-heap allocation. It guards
    // staging reuse only; the existing GUI lock still serializes all LCD APIs.
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    if (s_mutex == NULL) {
        heap_caps_free(buffer);
        return ESP_ERR_NO_MEM;
    }
    s_buffer = buffer;
    s_stats.capacity = capacity;
    s_stats.max_transaction_bytes = max_transaction;
    s_io = io;
    ESP_LOGI(TAG, "Fixed internal DMA staging ready: %u bytes @%p, chunk=%u, cache_align=%u",
             (unsigned)capacity, buffer, (unsigned)max_transaction, (unsigned)cache_alignment);
    return ESP_OK;
}

esp_err_t __wrap_esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io,
                                         int lcd_cmd, const void *color,
                                         size_t color_size)
{
    if (io == NULL || io != s_io) {
        return __real_esp_lcd_panel_io_tx_color(io, lcd_cmd, color, color_size);
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;
    s_stats.color_calls++;
    esp_err_t result;
    if ((color == NULL && color_size != 0) || color_size > s_stats.capacity) {
        s_stats.rejected_calls++;
        ESP_LOGE(TAG, "Reject color payload: ptr=%p bytes=%u capacity=%u",
                 color, (unsigned)color_size, (unsigned)s_stats.capacity);
        result = color_size > s_stats.capacity ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
        goto done;
    }

    // No SPI wire traffic for -1/NULL/0. Returning successfully proves that
    // all old descriptors, including ones from a partial failure, are reaped.
    result = esp_lcd_panel_io_tx_param(io, -1, NULL, 0);
    if (result != ESP_OK) {
        s_stats.drain_failures++;
        ESP_LOGE(TAG, "Color drain failed: %s", esp_err_to_name(result));
        goto done;
    }
    if (color_size != 0) {
        memcpy(s_buffer, color, color_size);
        s_stats.copied_bytes += color_size;
    }
    result = __real_esp_lcd_panel_io_tx_color(
        io, lcd_cmd, color_size != 0 ? s_buffer : color, color_size);
    if (result == ESP_OK) {
        s_stats.submitted_calls++;
        if (s_stats.submitted_calls == 1) {
            ESP_LOGI(TAG, "First color submission through fixed staging: %u bytes, command=0x%08x",
                     (unsigned)color_size, (unsigned)lcd_cmd);
        }
    } else {
        s_stats.submit_failures++;
        ESP_LOGE(TAG, "Staged color submission failed: %s", esp_err_to_name(result));
    }
done:
    xSemaphoreGive(s_mutex);
    return result;
}

void hosino_lcd_staging_get_stats(hosino_lcd_staging_stats_t *stats)
{
    if (stats == NULL) return;
    if (s_mutex == NULL) { memset(stats, 0, sizeof(*stats)); return; }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        *stats = s_stats;
        xSemaphoreGive(s_mutex);
    }
}
