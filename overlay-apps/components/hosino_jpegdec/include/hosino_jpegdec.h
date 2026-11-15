/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Hosino contributors */
#pragma once

#include "esp_video_dec_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOSINO_JPEGDEC_CODEC_CC ESP_VIDEO_CODEC_FOURCC('J', 'D', 'E', 'C')

/* Register once with codec_type MJPEG, is_hw false and the FourCC above.
 * Each open owns its own PSRAM context. The caller serializes operations on a
 * handle, as with Espressif's decoder. Only baseline JPEG -> packed RGB565_LE
 * up to 480x480 is supported; no resize/rotation or hardware access occurs.
 */
const esp_video_dec_ops_t *hosino_jpegdec_get_ops(void);

#ifdef __cplusplus
}
#endif
