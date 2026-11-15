// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOSINO_LCD_STAGING_BYTES (480U * 50U * 2U)

/**
 * Startup-only: attach the SPI2/CS12 display before the LVGL task starts.
 * Allocates one permanent, aligned internal DMA buffer. The caller must abort
 * display setup and delete its just-created IO if this function fails.
 * Only the current 480x480 / RGB565 / 50-row PARTIAL pipeline is supported.
 * All panel operations must still use the existing GUI/display mutex.
 */
esp_err_t hosino_lcd_staging_attach(esp_lcd_panel_io_handle_t io, size_t capacity);

typedef struct {
    size_t capacity;
    size_t max_transaction_bytes;
    uint32_t color_calls;
    uint32_t submitted_calls;
    uint64_t copied_bytes;
    uint32_t drain_failures;
    uint32_t submit_failures;
    uint32_t rejected_calls;
} hosino_lcd_staging_stats_t;

/** Thread-safe task-context snapshot. Submission counters are not FPS. */
void hosino_lcd_staging_get_stats(hosino_lcd_staging_stats_t *stats);

#ifdef __cplusplus
}
#endif
