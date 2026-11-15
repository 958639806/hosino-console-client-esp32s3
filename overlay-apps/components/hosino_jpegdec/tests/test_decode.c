/* SPDX-License-Identifier: Apache-2.0
 * Executes the real portable JPEGDEC core and production adapter, with heap
 * and log stubs only. It does not emulate or benchmark S3 SIMD instructions. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "hosino_jpegdec.h"
#include "JPEGDEC.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define GUARD 64u
static int fail_alloc;
static unsigned live_allocs;
void *heap_caps_aligned_alloc(size_t align, size_t n, unsigned caps)
{
    CHECK(caps == 3);
    if (fail_alloc) return NULL;
    void *base = malloc(n + align + sizeof(void *));
    if (!base) return NULL;
    uintptr_t p = ((uintptr_t)base + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1);
    ((void **)p)[-1] = base;
    ++live_allocs;
    return (void *)p;
}
void heap_caps_free(void *ptr)
{
    if (ptr) { --live_allocs; free(((void **)ptr)[-1]); }
}
static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb"); CHECK(f);
    CHECK(fseek(f, 0, SEEK_END) == 0); long n = ftell(f); CHECK(n > 0);
    rewind(f); uint8_t *data = malloc((size_t)n); CHECK(data);
    CHECK(fread(data, 1, (size_t)n, f) == (size_t)n); fclose(f);
    *size = (size_t)n; return data;
}
typedef struct { uint8_t *allocation, *pixels; size_t size; } guarded_t;
static guarded_t guarded(size_t size, int misalign)
{
    guarded_t g = {0}; g.size = size;
    g.allocation = malloc(size + 2 * GUARD + 32); CHECK(g.allocation);
    g.pixels = (uint8_t *)(((uintptr_t)g.allocation + GUARD + 15) & ~(uintptr_t)15);
    g.pixels += misalign;
    memset(g.pixels - GUARD, 0xa5, size + 2 * GUARD);
    return g;
}
static void guards(guarded_t g)
{
    for (unsigned i = 0; i < GUARD; ++i) {
        CHECK(g.pixels[-(int)i - 1] == 0xa5);
        CHECK(g.pixels[g.size + i] == 0xa5);
    }
}
static size_t sof_offset(uint8_t *data, size_t n)
{
    for (size_t i = 2; i + 10 < n; ++i)
        if (data[i] == 0xff && data[i + 1] == 0xc0) return i;
    CHECK(0); return 0;
}
static void test_one(const char *file, unsigned w, unsigned h, int color)
{
    const esp_video_dec_ops_t *ops = hosino_jpegdec_get_ops();
    esp_video_dec_cfg_t cfg = {0}; cfg.codec_type = ESP_VIDEO_CODEC_TYPE_MJPEG;
    cfg.out_fmt = ESP_VIDEO_CODEC_PIXEL_FMT_RGB565_LE;
    esp_video_dec_handle_t dec = NULL;
    CHECK(ops->open(&cfg, &dec) == ESP_VC_ERR_OK && dec);
    size_t n; uint8_t *jpeg = load(file, &n);
    esp_video_dec_in_frame_t in = {0}; in.data = jpeg; in.size = (uint32_t)n; in.pts = 123; in.dts = 120;
    guarded_t probe = guarded(32, 0);
    esp_video_dec_out_frame_t out = {0}; out.data = probe.pixels; out.size = 32;
    if (color == -2) {
        CHECK(ops->decode(dec, &in, &out) == ESP_VC_ERR_NOT_SUPPORTED);
        guards(probe); free(probe.allocation); free(jpeg);
        CHECK(ops->close(dec) == ESP_VC_ERR_OK && live_allocs == 0);
        printf("PASS %s explicitly rejects non-unit chroma sampling\n", file);
        return;
    }
    CHECK(ops->decode(dec, &in, &out) == ESP_VC_ERR_BUF_NOT_ENOUGH);
    CHECK(in.consumed == 0 && out.decoded_size == 0);
    for (unsigned i = 0; i < 32; ++i) CHECK(probe.pixels[i] == 0xa5);
    guards(probe); free(probe.allocation);
    esp_video_codec_frame_info_t info;
    CHECK(ops->get_frame_info(dec, &info) == ESP_VC_ERR_OK);
    CHECK(info.res.width == w && info.res.height == h);
    const size_t visible = w * h * 2u;
    guarded_t direct = guarded(((w + 15) & ~15u) * ((h + 15) & ~15u) * 2u, 0);
    out.data = direct.pixels; out.size = (uint32_t)direct.size;
    esp_vc_err_t result = ops->decode(dec, &in, &out);
    if (result != ESP_VC_ERR_OK) fprintf(stderr, "decode failed file=%s result=%d core_error=%d sampling=%x\n",
        file, result, JPEG_getLastError((JPEGIMAGE *)dec), ((JPEGIMAGE *)dec)->ucSubSample);
    CHECK(result == ESP_VC_ERR_OK);
    CHECK(in.consumed == n && out.decoded_size == visible && out.pts == 123 && out.dts == 120);
    guards(direct);
    guarded_t clipped = guarded(visible, 1); /* force callback even for 16-aligned height */
    out.data = clipped.pixels; out.size = (uint32_t)clipped.size;
    CHECK(ops->decode(dec, &in, &out) == ESP_VC_ERR_OK);
    CHECK(out.decoded_size == visible && memcmp(direct.pixels, clipped.pixels, visible) == 0);
    guards(clipped);
    char reference_path[1024]; snprintf(reference_path, sizeof(reference_path), "%s.rgb565le", file);
    FILE *reference_file = fopen(reference_path, "rb");
    if (reference_file) {
        fclose(reference_file);
        size_t reference_size; uint8_t *reference = load(reference_path, &reference_size);
        CHECK(reference_size == visible);
        for (size_t i = 0; i < visible; i += 2) {
            unsigned actual = clipped.pixels[i] | ((unsigned)clipped.pixels[i + 1] << 8);
            unsigned expected = reference[i] | ((unsigned)reference[i + 1] << 8);
            CHECK(abs((int)(actual >> 11) - (int)(expected >> 11)) <= 2);
            CHECK(abs((int)((actual >> 5) & 63) - (int)((expected >> 5) & 63)) <= 2);
            CHECK(abs((int)(actual & 31) - (int)(expected & 31)) <= 2);
        }
        free(reference);
    }
    if (color >= 0) {
        unsigned index = ((h / 2) * w + w / 2) * 2;
        unsigned rgb = clipped.pixels[index] | ((unsigned)clipped.pixels[index + 1] << 8);
        unsigned r = rgb >> 11, g = (rgb >> 5) & 63, b = rgb & 31;
        CHECK(color != 0 || (r >= 28 && g <= 3 && b <= 3));
        CHECK(color != 1 || (g >= 60 && r <= 3 && b <= 3));
        CHECK(color != 2 || (b >= 28 && r <= 3 && g <= 3));
    }
    /* Insufficient capacity must not write any pixels. */
    memset(clipped.pixels, 0xa5, visible); out.size = (uint32_t)visible - 1;
    CHECK(ops->decode(dec, &in, &out) == ESP_VC_ERR_BUF_NOT_ENOUGH);
    for (size_t i = 0; i < visible; ++i) CHECK(clipped.pixels[i] == 0xa5);
    guards(clipped);
    size_t sof = sof_offset(jpeg, n);
    jpeg[sof + 1] = 0xc2;
    CHECK(ops->decode(dec, &in, &out) == ESP_VC_ERR_NOT_SUPPORTED);
    jpeg[sof + 1] = 0xc0;
    uint8_t savew[2] = {jpeg[sof + 7], jpeg[sof + 8]};
    jpeg[sof + 7] = 1; jpeg[sof + 8] = 225; /* width 481 */
    CHECK(ops->decode(dec, &in, &out) == ESP_VC_ERR_NOT_SUPPORTED);
    jpeg[sof + 7] = savew[0]; jpeg[sof + 8] = savew[1];
    jpeg[sof + 8] ^= 1; /* valid but changed geometry cannot reuse GMF's pool */
    CHECK(ops->decode(dec, &in, &out) == ESP_VC_ERR_NOT_SUPPORTED);
    jpeg[sof + 8] ^= 1;
    in.size = 3; CHECK(ops->decode(dec, &in, &out) == ESP_VC_ERR_WRONG_DATA);
    in.size = 0; CHECK(ops->decode(dec, &in, &out) == ESP_VC_ERR_NO_MORE_DATA);
    CHECK(ops->close(dec) == ESP_VC_ERR_OK);
    free(direct.allocation); free(clipped.allocation); free(jpeg);
    CHECK(live_allocs == 0);
    printf("PASS %s %ux%u bounds, direct/copy identity, probe, malformed/EOS\n", file, w, h);
}
int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    const esp_video_dec_ops_t *ops = hosino_jpegdec_get_ops();
    esp_video_dec_cfg_t cfg = {0}; cfg.codec_type = ESP_VIDEO_CODEC_TYPE_MJPEG;
    cfg.out_fmt = ESP_VIDEO_CODEC_PIXEL_FMT_RGB565_LE;
    esp_video_dec_handle_t dec = (void *)1;
    fail_alloc = 1;
    CHECK(ops->open(&cfg, &dec) == ESP_VC_ERR_NO_MEMORY && dec == NULL);
    fail_alloc = 0;
    esp_video_dec_caps_t caps;
    CHECK(ops->get_caps(&caps) == ESP_VC_ERR_OK && caps.out_frame_align == 16 && caps.max_res.width == 480);
    for (int i = 1; i < argc; i += 4) {
        CHECK(i + 3 < argc);
        test_one(argv[i], (unsigned)atoi(argv[i + 1]), (unsigned)atoi(argv[i + 2]), atoi(argv[i + 3]));
    }
    puts("ALL HOST CHECKS PASSED (scalar core; no S3 performance claims)");
    return 0;
}
