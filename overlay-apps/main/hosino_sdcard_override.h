/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate the SDMMC-owned 4 KiB DMA bounce buffer before GUI initialization.
 * This is idempotent and does not mount or access a card. The buffer remains
 * owned by this module across unmount/remount for the lifetime of the firmware.
 */
esp_err_t hosino_sdcard_dma_init(void);

#ifdef __cplusplus
}
#endif
