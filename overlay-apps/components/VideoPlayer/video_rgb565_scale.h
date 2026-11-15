/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "video_view_math.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t source_width, source_height, width, height;
    uint32_t scale_256;
} hosino_video_rgb565_plan_t;

enum { HOSINO_VIDEO_RGB565_MAX_WIDTH = 480 };
/* Caller-owned internal SRAM scratch: 2880 bytes, independent of frame height.
 * Keeping both rows here avoids per-byte PSRAM accesses and rereading scaled
 * rows from PSRAM when vertical enlargement repeats a source row. */
typedef struct {
    uint16_t source_row[HOSINO_VIDEO_RGB565_MAX_WIDTH];
    uint16_t output_row[HOSINO_VIDEO_RGB565_MAX_WIDTH];
    uint16_t x_index[HOSINO_VIDEO_RGB565_MAX_WIDTH];
} hosino_video_rgb565_scratch_t;

/* Opaque, nearest-neighbour RGB565 only. Output is already cropped to the
 * viewport, so LVGL can use its untransformed copy path. */
bool hosino_video_rgb565_plan(hosino_video_scale_mode_t mode,
    uint16_t source_width, uint16_t source_height, uint16_t view_width,
    uint16_t view_height, hosino_video_rgb565_plan_t *plan);

/* Source and destination must not overlap. Accepts byte-aligned source pixels.
 * Preserves both bytes of every RGB565 pixel, leaving row padding untouched.
 * Native-size copies need no scratch. Scaling requires caller-owned scratch;
 * source/output widths over 480 are rejected without touching output. */
bool hosino_video_rgb565_copy(const hosino_video_rgb565_plan_t *plan,
    const uint8_t *source, size_t source_bytes, size_t source_stride,
    uint8_t *destination, size_t destination_bytes, size_t destination_stride,
    hosino_video_rgb565_scratch_t *scratch);
#ifdef __cplusplus
}
#endif
