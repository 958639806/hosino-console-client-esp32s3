/* Tests the production scaler, including byte alignment, cropped endpoints,
 * padding and guard bytes. Reference uses direct rational pixel coordinates. */
#include "video_rgb565_scale.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned cases;
static hosino_video_rgb565_scratch_t scratch;
static void check(unsigned w, unsigned h, unsigned vw, unsigned vh, int mode)
{
    hosino_video_rgb565_plan_t p;
    assert(hosino_video_rgb565_plan(mode, w, h, vw, vh, &p));
    assert(p.width && p.height && p.width <= vw && p.height <= vh);
    const size_t ss = w * 2 + 3, ds = p.width * 2 + 5;
    const size_t sb = ss * h, db = ds * p.height;
    unsigned char *src = malloc(sb + 2), *dst = malloc(db + 2);
    assert(src && dst);
    memset(src, 0xa5, sb + 2); memset(dst, 0x5a, db + 2);
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        src[1 + y * ss + 2 * x] = (unsigned char)(x * 7 + y * 31);
        src[1 + y * ss + 2 * x + 1] = (unsigned char)(x * 37 + y * 13);
    }
    assert(!hosino_video_rgb565_copy(&p, src + 1, sb - 1, ss, dst + 1, db, ds, &scratch));
    assert(!hosino_video_rgb565_copy(&p, src + 1, sb, ss, dst + 1, db - 1, ds, &scratch));
    for (size_t i = 0; i < db + 2; ++i) assert(dst[i] == 0x5a);
    assert(hosino_video_rgb565_copy(&p, src + 1, sb, ss, dst + 1, db, ds, &scratch));
    for (unsigned y = 0; y < p.height; ++y) {
        for (unsigned x = 0; x < p.width; ++x) {
            long long sx = ((long long)w * p.scale_256 + (2 * (int)x + 1 - (int)p.width) * 256) / (2 * p.scale_256);
            long long sy = ((long long)h * p.scale_256 + (2 * (int)y + 1 - (int)p.height) * 256) / (2 * p.scale_256);
            if (sx >= w) sx = w - 1; if (sy >= h) sy = h - 1;
            assert(sx >= 0 && sy >= 0);
            assert(!memcmp(dst + 1 + y * ds + x * 2, src + 1 + sy * ss + sx * 2, 2));
        }
        for (size_t x = p.width * 2; x < ds; ++x) assert(dst[1 + y * ds + x] == 0x5a);
    }
    assert(dst[0] == 0x5a && dst[db + 1] == 0x5a);
    assert(src[0] == 0xa5 && src[sb + 1] == 0xa5);
    free(src); free(dst); ++cases;
}

int main(void)
{
    for (unsigned w = 1; w <= 32; ++w) for (unsigned h = 1; h <= 32; ++h)
        for (int mode = 0; mode < 3; ++mode) check(w, h, 40, 36, mode);
    const unsigned sizes[][2] = {{320,180},{480,270},{320,240},{180,320},{479,371},{1,372},{480,1},{1,1},{480,372}};
    for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i)
        for (int mode = 0; mode < 3; ++mode) check(sizes[i][0], sizes[i][1], 480, 372, mode);
    hosino_video_rgb565_plan_t p;
    assert(!hosino_video_rgb565_plan(HOSINO_VIDEO_FILL, 0, 1, 480, 372, &p));
    assert(!hosino_video_rgb565_plan((hosino_video_scale_mode_t)3, 1, 1, 480, 372, &p));
    /* Native tightly packed path must be byte-exact, including odd widths. */
    unsigned char source[30], destination[30];
    for (unsigned i = 0; i < sizeof(source); ++i) source[i] = (unsigned char)(i * 19);
    assert(hosino_video_rgb565_plan(HOSINO_VIDEO_ORIGINAL, 5, 3, 480, 372, &p));
    assert(hosino_video_rgb565_copy(&p, source, 30, 10, destination, 30, 10, NULL));
    assert(!memcmp(source, destination, 30));
    /* Non-native scaling must not silently run without bounded SRAM scratch. */
    assert(hosino_video_rgb565_plan(HOSINO_VIDEO_FIT, 5, 3, 20, 20, &p));
    unsigned char enlarged[20 * 20 * 2];
    memset(enlarged, 0x3c, sizeof(enlarged));
    assert(!hosino_video_rgb565_copy(&p, source, 30, 10,
        enlarged, sizeof(enlarged), p.width * 2, NULL));
    for (unsigned i = 0; i < sizeof(enlarged); ++i) assert(enlarged[i] == 0x3c);
    assert(sizeof(scratch) == 2880);
    /* An over-wide input or output is rejected before touching any pixels. */
    unsigned char wide_source[481 * 2], wide_destination[481 * 2 * 2];
    memset(wide_source, 0x7a, sizeof(wide_source));
    memset(wide_destination, 0x3c, sizeof(wide_destination));
    assert(hosino_video_rgb565_plan(HOSINO_VIDEO_FIT, 481, 1, 480, 2, &p));
    assert(!hosino_video_rgb565_copy(&p, wide_source, sizeof(wide_source), sizeof(wide_source),
        wide_destination, sizeof(wide_destination), p.width * 2, &scratch));
    assert(hosino_video_rgb565_plan(HOSINO_VIDEO_FIT, 480, 1, 481, 2, &p));
    /* Quantization may retain width 480 here; force a valid >480 scaled plan. */
    p.width = 481; p.scale_256 = 257;
    assert(!hosino_video_rgb565_copy(&p, wide_source, sizeof(wide_source), sizeof(wide_source),
        wide_destination, sizeof(wide_destination), p.width * 2, &scratch));
    for (unsigned i = 0; i < sizeof(wide_destination); ++i) assert(wide_destination[i] == 0x3c);
    printf("PASS %u resampling cases; byte alignment, reference pixels, guards, padded rows and native memcpy\n", cases);
    return 0;
}
