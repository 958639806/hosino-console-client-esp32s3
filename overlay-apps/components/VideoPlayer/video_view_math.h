/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

/* LVGL's fixed-point scale uses 256 == 1:1. Scaling does not change decoder
 * limits or allocate another frame. Bound pathological tiny/aspect-ratio clips
 * to 16x; their fill mode may still leave space instead of enormous transforms. */
typedef enum {
    HOSINO_VIDEO_ORIGINAL = 0, HOSINO_VIDEO_FIT = 1, HOSINO_VIDEO_FILL = 2
} hosino_video_scale_mode_t;
static inline uint32_t hosino_video_scale_256(hosino_video_scale_mode_t mode,
    uint32_t width, uint32_t height, uint32_t view_width, uint32_t view_height)
{
    if (!width || !height || !view_width || !view_height || mode == HOSINO_VIDEO_ORIGINAL) return 256;
    const uint64_t x = (uint64_t)view_width * 256;
    const uint64_t y = (uint64_t)view_height * 256;
    uint64_t scale;
    if (mode == HOSINO_VIDEO_FILL) {
        const uint64_t sx = (x + width - 1) / width, sy = (y + height - 1) / height;
        scale = sx > sy ? sx : sy;
    } else {
        const uint64_t sx = x / width, sy = y / height;
        scale = sx < sy ? sx : sy;
    }
    return scale < 1 ? 1 : scale > 4096 ? 4096 : (uint32_t)scale;
}
