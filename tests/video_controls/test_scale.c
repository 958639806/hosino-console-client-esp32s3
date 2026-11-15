#include "video_view_math.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

int main(void)
{
    assert(hosino_video_scale_256(HOSINO_VIDEO_ORIGINAL, 320, 180, 480, 372) == 256);
    assert(hosino_video_scale_256(HOSINO_VIDEO_FIT, 320, 180, 480, 372) == 384);
    assert(hosino_video_scale_256(HOSINO_VIDEO_FILL, 320, 180, 480, 372) == 530);
    assert(hosino_video_scale_256(HOSINO_VIDEO_FIT, 480, 270, 480, 372) == 256);
    assert(hosino_video_scale_256(HOSINO_VIDEO_FILL, 480, 270, 480, 372) == 353);
    assert(hosino_video_scale_256(HOSINO_VIDEO_FIT, 180, 320, 480, 372) == 297);
    assert(hosino_video_scale_256(HOSINO_VIDEO_FILL, 180, 320, 480, 372) == 683);
    assert(hosino_video_scale_256(HOSINO_VIDEO_FIT, 0, 320, 480, 372) == 256);
    assert(hosino_video_scale_256(HOSINO_VIDEO_FILL, 1, 1, 480, 372) == 4096);
    unsigned checked = 9;
    /* Every normally supported source size: contain must fit; cover must cover,
     * except explicitly bounded >16x cases. Equal X/Y scale preserves aspect. */
    for (uint32_t w = 1; w <= 480; ++w) {
        for (uint32_t h = 1; h <= 372; ++h) {
            uint32_t fit = hosino_video_scale_256(HOSINO_VIDEO_FIT, w, h, 480, 372);
            uint32_t fill = hosino_video_scale_256(HOSINO_VIDEO_FILL, w, h, 480, 372);
            assert(fit >= 1 && fit <= 4096 && fill >= fit && fill <= 4096);
            assert((uint64_t)w * fit <= 480 * 256 && (uint64_t)h * fit <= 372 * 256);
            if (fill < 4096) assert((uint64_t)w * fill >= 480 * 256 && (uint64_t)h * fill >= 372 * 256);
            ++checked;
        }
    }
    printf("PASS %u scale cases (all 480x372 supported source dimensions plus targeted edges)\n", checked);
    return 0;
}
