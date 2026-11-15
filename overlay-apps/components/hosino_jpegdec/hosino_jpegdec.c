/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Hosino contributors
 * JPEGDEC adapter for esp_video_codec 0.5.7. No display/audio/SD ownership. */
#include "hosino_jpegdec.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "JPEGDEC.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define MAX_DIMENSION 480u
#define ALIGN16(n) (((n) + 15u) & ~15u)
static const char *TAG = "hosino_jpegdec";

typedef struct {
    JPEGIMAGE jpeg;
    uint32_t width, height;
    uint8_t *output;
    bool frame_info_valid;
    bool copy_failed;
    bool logged_path;
} decoder_t;

/* Validate the SOF before letting the decoder initialize lookup tables. This
 * deliberately excludes progressive/DC-thumbnail decoding and CMYK JPEGs.
 * Every marker-length read is bounded by the compressed packet size. */
static esp_vc_err_t baseline_size(const uint8_t *data, uint32_t size,
                                 uint32_t *width, uint32_t *height)
{
    if (size < 4 || data[0] != 0xff || data[1] != 0xd8) return ESP_VC_ERR_WRONG_DATA;
    uint32_t p = 2;
    while (p < size) {
        if (data[p++] != 0xff) return ESP_VC_ERR_WRONG_DATA;
        while (p < size && data[p] == 0xff) ++p;
        if (p == size) return ESP_VC_ERR_WRONG_DATA;
        const uint8_t marker = data[p++];
        if (marker == 0xda || marker == 0xd9 || marker == 0x00) return ESP_VC_ERR_WRONG_DATA;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (size - p < 2) return ESP_VC_ERR_WRONG_DATA;
        const uint32_t length = ((uint32_t)data[p] << 8) | data[p + 1];
        if (length < 2 || length > size - p) return ESP_VC_ERR_WRONG_DATA;
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (marker != 0xc0) return ESP_VC_ERR_NOT_SUPPORTED;
            if (length < 8 || data[p + 2] != 8) return ESP_VC_ERR_WRONG_DATA;
            const uint8_t components = data[p + 7];
            if (components != 1 && components != 3) return ESP_VC_ERR_NOT_SUPPORTED;
            if (length != 8u + 3u * components) return ESP_VC_ERR_WRONG_DATA;
            /* JPEGDEC assumes unit chroma sampling. Some MJPEG encoders use
             * Y22/Cb12/Cr12 for 4:2:2 or all12 for 4:4:4; these require a
             * different MCU block order and cannot be normalized by editing
             * SOF bytes. Reject them before the core decodes wrong blocks. */
            const uint8_t y_sampling = data[p + 9];
            if (components == 1) {
                if (y_sampling != 0x11) return ESP_VC_ERR_NOT_SUPPORTED;
            } else if ((y_sampling != 0x11 && y_sampling != 0x12 &&
                        y_sampling != 0x21 && y_sampling != 0x22) ||
                       data[p + 12] != 0x11 || data[p + 15] != 0x11) {
                return ESP_VC_ERR_NOT_SUPPORTED;
            }
            *height = ((uint32_t)data[p + 3] << 8) | data[p + 4];
            *width = ((uint32_t)data[p + 5] << 8) | data[p + 6];
            if (*width == 0 || *height == 0) return ESP_VC_ERR_WRONG_DATA;
            if (*width > MAX_DIMENSION || *height > MAX_DIMENSION) return ESP_VC_ERR_NOT_SUPPORTED;
            return ESP_VC_ERR_OK;
        }
        p += length;
    }
    return ESP_VC_ERR_WRONG_DATA;
}

/* Fallback for non-MCU-aligned widths or exact-sized caller buffers.
 * JPEGDRAW.iWidth is source stride; iWidthUsed is the valid edge width. */
static int copy_block(JPEGDRAW *block)
{
    decoder_t *d = block ? (decoder_t *)block->pUser : NULL;
    if (!d) return 0;
    if (!block->pPixels || block->iBpp != 16 || block->x < 0 || block->y < 0 ||
        block->iWidth <= 0 || block->iWidth > (int)MAX_DIMENSION + 16 ||
        block->iHeight <= 0 || block->iHeight > 16 || block->iWidthUsed <= 0 ||
        block->iWidthUsed > block->iWidth || (uint32_t)block->x >= d->width ||
        (uint32_t)block->y >= d->height) {
        d->copy_failed = true;
        return 0;
    }
    uint32_t w = (uint32_t)block->iWidthUsed;
    uint32_t h = (uint32_t)block->iHeight;
    if (w > d->width - (uint32_t)block->x) w = d->width - (uint32_t)block->x;
    if (h > d->height - (uint32_t)block->y) h = d->height - (uint32_t)block->y;
    for (uint32_t row = 0; row < h; ++row) {
        memcpy(d->output + (((uint32_t)block->y + row) * d->width + (uint32_t)block->x) * 2u,
               block->pPixels + row * (uint32_t)block->iWidth, w * 2u);
    }
    return 1;
}

static esp_vc_err_t get_caps(esp_video_dec_caps_t *caps)
{
    static const esp_video_codec_pixel_fmt_t formats[] = { ESP_VIDEO_CODEC_PIXEL_FMT_RGB565_LE };
    if (!caps) return ESP_VC_ERR_INVALID_ARG;
    memset(caps, 0, sizeof(*caps));
    caps->in_frame_align = 1;
    caps->out_frame_align = 16;
    caps->out_fmts = formats;
    caps->out_fmt_num = 1;
    caps->typical_res = (esp_video_codec_resolution_t){320, 240};
    caps->max_res = (esp_video_codec_resolution_t){MAX_DIMENSION, MAX_DIMENSION};
    /* No fabricated performance promise; cadence comes from source timestamps. */
    caps->typical_fps = 0;
    return ESP_VC_ERR_OK;
}

static esp_vc_err_t open_decoder(esp_video_dec_cfg_t *cfg, esp_video_dec_handle_t *handle)
{
    if (!cfg || !handle) return ESP_VC_ERR_INVALID_ARG;
    *handle = NULL;
    if (cfg->codec_type != ESP_VIDEO_CODEC_TYPE_MJPEG || cfg->out_fmt != ESP_VIDEO_CODEC_PIXEL_FMT_RGB565_LE)
        return ESP_VC_ERR_NOT_SUPPORTED;
    decoder_t *d = heap_caps_aligned_alloc(16, sizeof(*d), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) return ESP_VC_ERR_NO_MEMORY;
    memset(d, 0, sizeof(*d));
    *handle = d;
    ESP_LOGI(TAG, "Open JDEC: S3 SIMD, context=%u bytes in PSRAM", (unsigned)sizeof(*d));
    return ESP_VC_ERR_OK;
}

static esp_vc_err_t decode(esp_video_dec_handle_t handle, esp_video_dec_in_frame_t *in,
                           esp_video_dec_out_frame_t *out)
{
    decoder_t *d = handle;
    if (!d || !in || !out) return ESP_VC_ERR_INVALID_ARG;
    in->consumed = 0;
    out->decoded_size = 0;
    if (in->size == 0) return ESP_VC_ERR_NO_MORE_DATA;
    if (!in->data || in->size > INT_MAX) return ESP_VC_ERR_INVALID_ARG;
    d->frame_info_valid = false;
    uint32_t width, height;
    esp_vc_err_t result = baseline_size(in->data, in->size, &width, &height);
    if (result != ESP_VC_ERR_OK) return result;
    /* GMF announces geometry and allocates its pool after the first probe.
     * Do not publish a changed stride/dimension inside that same stream. */
    if (d->width && (d->width != width || d->height != height)) return ESP_VC_ERR_NOT_SUPPORTED;
    if (!JPEG_openRAM(&d->jpeg, in->data, (int)in->size, copy_block)) return ESP_VC_ERR_WRONG_DATA;
    if (JPEG_getWidth(&d->jpeg) != (int)width || JPEG_getHeight(&d->jpeg) != (int)height) {
        JPEG_close(&d->jpeg);
        return ESP_VC_ERR_WRONG_DATA;
    }
    d->width = width;
    d->height = height;
    d->frame_info_valid = true;
    const uint32_t visible_bytes = width * height * 2u;
    if (!out->data || out->size < visible_bytes) {
        JPEG_close(&d->jpeg);
        return ESP_VC_ERR_BUF_NOT_ENOUGH;
    }
    JPEG_setPixelType(&d->jpeg, RGB565_LITTLE_ENDIAN);
    d->jpeg.pUser = d;
    d->output = out->data;
    d->copy_failed = false;
    /* GMF 1.1.1 allocates ALIGN16(width)*ALIGN16(height)*2. With an
     * aligned width the decoder writes packed rows, including padded bottom
     * MCUs, without a second frame allocation or copy. Never assume capacity. */
    const bool direct = (width % 16u) == 0 && ((uintptr_t)out->data % 16u) == 0 &&
                        out->size >= width * ALIGN16(height) * 2u;
    JPEG_setFramebuffer(&d->jpeg, direct ? out->data : NULL);
    if (!d->logged_path) {
        ESP_LOGI(TAG, "JDEC %ux%u output=%u visible=%u mode=%s", (unsigned)width,
                 (unsigned)height, (unsigned)out->size, (unsigned)visible_bytes,
                 direct ? "padded-direct" : "clipped-copy");
        d->logged_path = true;
    }
    const int ok = JPEG_decode(&d->jpeg, 0, 0, 0);
    JPEG_close(&d->jpeg);
    d->output = NULL;
    if (!ok || d->copy_failed) return ESP_VC_ERR_WRONG_DATA;
    in->consumed = in->size;
    out->pts = in->pts;
    out->dts = in->dts;
    out->frame_type = ESP_VIDEO_CODEC_FRAME_TYPE_I;
    out->decoded_size = visible_bytes;
    return ESP_VC_ERR_OK;
}

static esp_vc_err_t frame_info(esp_video_dec_handle_t handle, esp_video_codec_frame_info_t *info)
{
    decoder_t *d = handle;
    if (!d || !info) return ESP_VC_ERR_INVALID_ARG;
    if (!d->frame_info_valid) return ESP_VC_ERR_INVALID_STATE;
    memset(info, 0, sizeof(*info));
    info->res = (esp_video_codec_resolution_t){d->width, d->height};
    info->origin_res = info->res;
    return ESP_VC_ERR_OK;
}

static esp_vc_err_t set_config(esp_video_dec_handle_t handle, esp_video_dec_set_type_t type,
                              void *data, uint32_t size)
{
    (void)type; (void)data; (void)size;
    return handle ? ESP_VC_ERR_NOT_SUPPORTED : ESP_VC_ERR_INVALID_ARG;
}

static esp_vc_err_t close_decoder(esp_video_dec_handle_t handle)
{
    if (!handle) return ESP_VC_ERR_INVALID_ARG;
    heap_caps_free(handle);
    return ESP_VC_ERR_OK;
}

const esp_video_dec_ops_t *hosino_jpegdec_get_ops(void)
{
    static const esp_video_dec_ops_t ops = {
        .get_caps = get_caps, .open = open_decoder, .decode = decode,
        .get_frame_info = frame_info, .set = set_config, .close = close_decoder,
    };
    return &ops;
}
