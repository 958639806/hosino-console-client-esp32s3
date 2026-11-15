/* SPDX-License-Identifier: Apache-2.0 */
#include "video_rgb565_scale.h"
#include <string.h>

bool hosino_video_rgb565_plan(hosino_video_scale_mode_t mode,
    uint16_t source_width, uint16_t source_height, uint16_t view_width,
    uint16_t view_height, hosino_video_rgb565_plan_t *plan)
{
    if (!plan || !source_width || !source_height || !view_width || !view_height ||
        mode < HOSINO_VIDEO_ORIGINAL || mode > HOSINO_VIDEO_FILL) return false;
    const uint32_t scale = hosino_video_scale_256(mode, source_width,
        source_height, view_width, view_height);
    uint32_t width = (uint32_t)source_width * scale / 256;
    uint32_t height = (uint32_t)source_height * scale / 256;
    if (!width) width = 1;
    if (!height) height = 1;
    if (width > view_width) width = view_width;
    if (height > view_height) height = view_height;
    *plan = (hosino_video_rgb565_plan_t){source_width, source_height,
        (uint16_t)width, (uint16_t)height, scale};
    return true;
}

bool hosino_video_rgb565_copy(const hosino_video_rgb565_plan_t *p,
    const uint8_t *source, size_t source_bytes, size_t source_stride,
    uint8_t *destination, size_t destination_bytes, size_t destination_stride,
    hosino_video_rgb565_scratch_t *scratch)
{
    if (!p || !source || !destination || !p->source_width || !p->source_height ||
        !p->width || !p->height || p->scale_256 < 1 || p->scale_256 > 4096 ||
        source_stride < (size_t)p->source_width * 2 ||
        source_stride > SIZE_MAX / p->source_height ||
        source_bytes < source_stride * p->source_height ||
        destination_stride < (size_t)p->width * 2 ||
        destination_stride > SIZE_MAX / p->height ||
        destination_bytes < destination_stride * p->height) return false;
    const size_t row_bytes = (size_t)p->width * 2;
    if (p->scale_256 == 256 && p->width == p->source_width && p->height == p->source_height) {
        if (source_stride == row_bytes && destination_stride == row_bytes) memcpy(destination, source, row_bytes * p->height);
        else for (unsigned y = 0; y < p->height; ++y)
            memcpy(destination + y * destination_stride, source + y * source_stride, row_bytes);
        return true;
    }
    if (!scratch || p->source_width > HOSINO_VIDEO_RGB565_MAX_WIDTH ||
        p->width > HOSINO_VIDEO_RGB565_MAX_WIDTH) return false;
    /* Center-align pixel centres. Increment quotient/remainder instead of
     * performing a division for each pixel; equal scale on both axes preserves
     * aspect ratio. The clamp only covers sub-pixel rounding of tiny images. */
    const uint32_t denominator = p->scale_256 * 2;
    const int64_t x_start = (int64_t)p->source_width * p->scale_256 + (1 - (int32_t)p->width) * 256;
    const int64_t y_start = (int64_t)p->source_height * p->scale_256 + (1 - (int32_t)p->height) * 256;
    if (x_start < 0 || y_start < 0) return false;
    const uint32_t x_first = (uint32_t)(x_start / denominator), x_remainder = (uint32_t)(x_start % denominator);
    uint32_t sy = (uint32_t)(y_start / denominator), y_remainder = (uint32_t)(y_start % denominator);
    const uint32_t step = 512 / denominator, remainder_step = 512 % denominator;
    uint32_t sx = x_first, remainder = x_remainder;
    for (unsigned x = 0; x < p->width; ++x) {
        scratch->x_index[x] = (uint16_t)(sx < p->source_width ? sx : p->source_width - 1U);
        sx += step;
        remainder += remainder_step;
        if (remainder >= denominator) { remainder -= denominator; ++sx; }
    }
    uint32_t last_y = UINT32_MAX;
    for (unsigned y = 0; y < p->height; ++y) {
        const uint32_t sample_y = sy < p->source_height ? sy : p->source_height - 1U;
        uint8_t *out = destination + y * destination_stride;
        if (sample_y != last_y) {
            const uint8_t *in = source + sample_y * source_stride;
            memcpy(scratch->source_row, in, (size_t)p->source_width * 2);
            for (unsigned x = 0; x < p->width; ++x)
                scratch->output_row[x] = scratch->source_row[scratch->x_index[x]];
        }
        memcpy(out, scratch->output_row, row_bytes);
        last_y = sample_y;
        sy += step;
        y_remainder += remainder_step;
        if (y_remainder >= denominator) { y_remainder -= denominator; ++sy; }
    }
    return true;
}
