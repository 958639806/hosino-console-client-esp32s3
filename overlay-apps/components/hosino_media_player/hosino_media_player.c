/* SPDX-License-Identifier: Apache-2.0 */
#include "hosino_media_player.h"
#include "sdkconfig.h"
#include "media_profile.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_player.h"
#include "esp_player_advance.h"
#include "esp_extractor_defaults.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_video_dec_default.h"
#include "esp_video_dec_reg.h"
#include "esp_video_dec_mjpeg.h"
#include "esp_gmf_video_dec.h"
#include "esp_gmf_video_element.h"
#include "hosino_jpegdec.h"
#include "esp_audio_render.h"
#include "esp_video_render.h"
#include "esp_video_render_backend.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_video_color_convert.h"
#include "esp_gmf_video_crop.h"
#include "bsp_board_extra.h"
#include "storage_service.h"
#include "hosino_mjpeg_metadata.h"
#include "hosino_network_policy.h"

static const char *TAG = "hosino_media";
static portMUX_TYPE s_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;

static void log_media_memory(const char *stage)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG, "MEM %s: internal free=%u largest=%u PSRAM free=%u largest=%u",
             stage, (unsigned)heap_caps_get_free_size(internal),
             (unsigned)heap_caps_get_largest_free_block(internal),
             (unsigned)heap_caps_get_free_size(external),
             (unsigned)heap_caps_get_largest_free_block(external));
}
/* Process-wide registry tables must outlive individual app sessions. */
static unsigned s_registry_state; /* 0 untouched, 1 registering, 2 completed */
static esp_err_t s_registry_result = ESP_ERR_INVALID_STATE;

static esp_err_t ensure_media_registries(void)
{
    for (;;) {
        portENTER_CRITICAL(&s_snapshot_mux);
        const unsigned state = s_registry_state;
        const esp_err_t previous = s_registry_result;
        if (state == 0) s_registry_state = 1;
        portEXIT_CRITICAL(&s_snapshot_mux);
        if (state == 2) return previous;
        if (state == 0) break;
        /* Control workers only. Registration must not hold a spinlock. */
        vTaskDelay(1);
    }
    esp_err_t result = ESP_OK;
    const char *stage = "extractor";
    int native = esp_extractor_register_default();
    if (native != ESP_EXTRACTOR_ERR_OK) {
        result = native == ESP_EXTRACTOR_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    } else {
        stage = "audio decoder";
        native = esp_audio_dec_register_default();
        if (native != ESP_AUDIO_ERR_OK) result = native == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    if (result == ESP_OK) {
        stage = "audio simple decoder";
        native = esp_audio_simple_dec_register_default();
        if (native != ESP_AUDIO_ERR_OK) result = native == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    if (result == ESP_OK) {
        stage = "video decoder";
        native = esp_video_dec_register_default();
        if (native != ESP_VC_ERR_OK) result = native == ESP_VC_ERR_NO_MEMORY ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    if (result == ESP_OK) {
        stage = "JPEGDEC";
        esp_video_codec_desc_t desc = {
            .codec_type = ESP_VIDEO_CODEC_TYPE_MJPEG, .codec_cc = HOSINO_JPEGDEC_CODEC_CC,
        };
        native = esp_video_dec_register(&desc, hosino_jpegdec_get_ops());
        if (native != ESP_VC_ERR_OK) result = native == ESP_VC_ERR_NO_MEMORY ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Media registries ready: extractors/audio/simple-audio/video");
    } else {
        /* Partial registration is retained. Do not retry or unregister shared
         * providers while another app might use the process-wide tables. */
        ESP_LOGE(TAG, "Media registry %s failed: %d (%s); restart required",
                 stage, native, esp_err_to_name(result));
    }
    portENTER_CRITICAL(&s_snapshot_mux);
    s_registry_result = result;
    s_registry_state = 2;
    portEXIT_CRITICAL(&s_snapshot_mux);
    return result;
}
struct hosino_media_player {
    hosino_media_player_config_t cfg;

    hosino_media_player_snapshot_t snapshot;
    esp_player_handle_t player;
    esp_audio_render_handle_t audio;
    esp_video_render_handle_t video;
    esp_gmf_pool_handle_t audio_pool, video_pool;
    storage_service_lease_t lease;
    bsp_extra_audio_owner_t owner;
    bool audio_owned, board_open, sink_error, stopping;
    uint32_t callbacks;
    uint32_t position_epoch;
    uint64_t seek_target_ms;
    int64_t seek_started_us, profile_started_us, audio_previous_end_us;
    uint16_t backend_width, backend_height;
    uint8_t *fallback_fb;
    char path[HOSINO_MEDIA_PATH_MAX];
    bool network_source;
    char network_base[HOSINO_MEDIA_PATH_MAX];
    char network_authorization[200];
};

static esp_err_t player_error(int e)
{
    switch (e) {
    case ESP_PLAYER_ERR_OK: return ESP_OK;
    case ESP_PLAYER_ERR_INVALID_ARG: return ESP_ERR_INVALID_ARG;
    case ESP_PLAYER_ERR_NO_MEM: return ESP_ERR_NO_MEM;
    case ESP_PLAYER_ERR_TIMEOUT: return ESP_ERR_TIMEOUT;
    case ESP_PLAYER_ERR_NOT_SUPPORT: return ESP_ERR_NOT_SUPPORTED;
    case ESP_PLAYER_ERR_INVALID_STATE: return ESP_ERR_INVALID_STATE;
    default: return ESP_FAIL;
    }
}
static esp_err_t fail(struct hosino_media_player *p, esp_err_t err, int native)
{
    portENTER_CRITICAL(&s_snapshot_mux);
    p->snapshot.last_error = err;
    p->snapshot.backend_error = native;
    p->snapshot.state = HOSINO_MEDIA_ERROR;
    p->snapshot.seeking = false;
    ++p->position_epoch;
    portEXIT_CRITICAL(&s_snapshot_mux);
    ESP_LOGE(TAG, "Media failure: %s (backend=%d)", esp_err_to_name(err), native);
    return err;
}
static void mark_sink_error(struct hosino_media_player *p, esp_err_t err)
{
    bool first;
    portENTER_CRITICAL(&s_snapshot_mux);
    first = !p->sink_error;
    p->sink_error = true;
    if (first) {
        p->snapshot.last_error = err;
        p->snapshot.backend_error = 0;
        p->snapshot.state = HOSINO_MEDIA_ERROR;
        p->snapshot.seeking = false;
        ++p->position_epoch;
    }
    portEXIT_CRITICAL(&s_snapshot_mux);
    if (first) ESP_LOGE(TAG, "Output rejected: %s; control worker will stop playback", esp_err_to_name(err));
}
static void callback_enter(struct hosino_media_player *p)
{
    portENTER_CRITICAL(&s_snapshot_mux); ++p->callbacks; portEXIT_CRITICAL(&s_snapshot_mux);
}
static void callback_leave(struct hosino_media_player *p)
{
    portENTER_CRITICAL(&s_snapshot_mux); --p->callbacks; portEXIT_CRITICAL(&s_snapshot_mux);
}
static bool output_disabled(struct hosino_media_player *p)
{
    portENTER_CRITICAL(&s_snapshot_mux);
    bool disabled = p->stopping || p->sink_error;
    portEXIT_CRITICAL(&s_snapshot_mux);
    return disabled;
}
static int audio_write(uint8_t *data, uint32_t bytes, void *ctx)
{
    struct hosino_media_player *p = ctx;
    if (!bytes) return 0;
    callback_enter(p);
    if (output_disabled(p)) { callback_leave(p); return 0; }
    size_t written = 0;
    const int64_t started_us = esp_timer_get_time();
    esp_err_t err = bsp_extra_i2s_write(data, bytes, &written, 1000);
    const int64_t ended_us = esp_timer_get_time();
    if (err == ESP_OK && written != bytes) err = ESP_FAIL;
    if (p->cfg.enable_video) {
        const uint32_t gap_us = p->audio_previous_end_us ? (uint32_t)(started_us - p->audio_previous_end_us) : UINT32_MAX;
        hosino_media_profile_record_audio_write((uint32_t)(ended_us - started_us), gap_us,
                                                (uint32_t)written, bytes, err != ESP_OK);
        p->audio_previous_end_us = ended_us;
    }
    if (err != ESP_OK) mark_sink_error(p, err);
    callback_leave(p);
    return err == ESP_OK ? 0 : -1;
}

/* This backend deliberately claims GRAM semantics without owning the LCD.
 * One uncached stream is rendered synchronously by ESP Player; write_fb copies
 * through the application callback before the borrowed decoder frame expires.
 * Never add asynchronous composition / overlays without revisiting ownership. */
static esp_video_render_err_t video_init(void *cfg, int size, esp_video_render_backend_handle_t *out)
{
    if (!cfg || size != sizeof(struct hosino_media_player *) || !out) return ESP_VIDEO_RENDER_ERR_INVALID_ARG;
    *out = *(struct hosino_media_player **)cfg;
    return *out ? ESP_VIDEO_RENDER_ERR_OK : ESP_VIDEO_RENDER_ERR_INVALID_ARG;
}
static bool video_has_gram(esp_video_render_backend_handle_t h) { (void)h; return true; }
static esp_video_render_err_t video_info(esp_video_render_backend_handle_t h, esp_video_render_disp_info_t *info)
{
    struct hosino_media_player *p = h;
    if (!p || !info) return ESP_VIDEO_RENDER_ERR_INVALID_ARG;
    *info = (esp_video_render_disp_info_t){ .format = ESP_VIDEO_RENDER_FORMAT_RGB565,
        .width = p->backend_width, .height = p->backend_height };
    return ESP_VIDEO_RENDER_ERR_OK;
}
static esp_video_render_err_t video_get_fb(esp_video_render_backend_handle_t h, esp_video_render_fb_t *fb)
{
    struct hosino_media_player *p = h;
    if (!p || !fb || !p->backend_width || !p->backend_height) return ESP_VIDEO_RENDER_ERR_INVALID_ARG;
    size_t bytes = (size_t)p->backend_width * p->backend_height * 2;
    if (!p->fallback_fb) {
        p->fallback_fb = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p->fallback_fb) { mark_sink_error(p, ESP_ERR_NO_MEM); return ESP_VIDEO_RENDER_ERR_NO_MEM; }
    }
    *fb = (esp_video_render_fb_t){ .info = { .format = ESP_VIDEO_RENDER_FORMAT_RGB565,
        .width = p->backend_width, .height = p->backend_height }, .data = p->fallback_fb, .size = bytes };
    return ESP_VIDEO_RENDER_ERR_OK;
}
static esp_video_render_err_t video_lock(esp_video_render_backend_handle_t h, esp_video_render_fb_t *fb, bool lock)
{ (void)h; (void)fb; (void)lock; return ESP_VIDEO_RENDER_ERR_OK; }
static esp_video_render_err_t video_write(esp_video_render_backend_handle_t h, esp_video_render_fb_t *fb,
                                        const esp_video_render_rect_t *dirty, const esp_video_render_pos_t *pos)
{
    struct hosino_media_player *p = h;
    if (!p || !fb) return ESP_VIDEO_RENDER_ERR_INVALID_ARG;
    (void)dirty;
    callback_enter(p);
    if (output_disabled(p)) { callback_leave(p); return ESP_VIDEO_RENDER_ERR_OK; }
    const size_t bytes = (size_t)fb->info.width * fb->info.height * 2;
    if (!fb->data || fb->info.format != ESP_VIDEO_RENDER_FORMAT_RGB565 || !bytes || fb->size < bytes ||
        fb->info.width != p->backend_width || fb->info.height != p->backend_height ||
        (pos && (pos->x || pos->y))) {
        mark_sink_error(p, ESP_ERR_NOT_SUPPORTED);
        callback_leave(p); return ESP_VIDEO_RENDER_ERR_NOT_SUPPORTED;
    }
    hosino_media_video_frame_t frame = { .data = fb->data, .bytes = bytes,
        .width = fb->info.width, .height = fb->info.height, .stride_bytes = (uint32_t)fb->info.width * 2 };
    const int64_t started_us = esp_timer_get_time();
    esp_err_t err = p->cfg.video_cb(&frame, p->cfg.video_ctx);
    hosino_media_profile_record_video_sink((uint32_t)(esp_timer_get_time() - started_us));
    portENTER_CRITICAL(&s_snapshot_mux); ++p->snapshot.video_frames; portEXIT_CRITICAL(&s_snapshot_mux);
    if (err != ESP_OK) mark_sink_error(p, err);
    callback_leave(p);
    return err == ESP_OK ? ESP_VIDEO_RENDER_ERR_OK : ESP_VIDEO_RENDER_ERR_FAIL;
}
static esp_video_render_err_t video_deinit(esp_video_render_backend_handle_t h)
{
    struct hosino_media_player *p = h;
    if (p) { heap_caps_free(p->fallback_fb); p->fallback_fb = NULL; }
    return ESP_VIDEO_RENDER_ERR_OK;
}
static const esp_video_render_backend_ops_t video_ops = {
    .init = video_init, .with_gram = video_has_gram, .get_display_info = video_info,
    .get_fb = video_get_fb, .lock_fb = video_lock, .write_fb = video_write, .deinit = video_deinit
};
/* Geometry validation and an explicit per-instance decoder selection.
 * This runs before the first decoder/render pipeline is built, so no vendor
 * player API is called recursively and the video renderer has no live stream. */
static esp_player_err_t video_factory(void *ctx, uint32_t codec, const esp_player_video_stream_info_t *info,
                                     esp_gmf_element_handle_t *out)
{
    struct hosino_media_player *p = ctx;
    if (!out || codec != (uint32_t)ESP_VIDEO_CODEC_TYPE_MJPEG || !info || !info->width || !info->height || info->width > p->cfg.max_width || info->height > p->cfg.max_height) {
        ESP_LOGE(TAG,"Video factory rejected codec=%08" PRIx32 " raw=%ux%u bounds=%ux%u managed=%d",codec,
                 info?info->width:0,info?info->height:0,p->cfg.max_width,p->cfg.max_height,p->network_source);
        mark_sink_error(p, ESP_ERR_NOT_SUPPORTED); return ESP_PLAYER_ERR_INVALID_ARG;
    }
    *out = NULL;
    portENTER_CRITICAL(&s_snapshot_mux);
    p->snapshot.source_width = info->width; p->snapshot.source_height = info->height; p->snapshot.source_fps = info->fps;
    portEXIT_CRITICAL(&s_snapshot_mux);
    if (p->backend_width) {
        if (p->backend_width != info->width || p->backend_height != info->height) {
            mark_sink_error(p, ESP_ERR_NOT_SUPPORTED); return ESP_PLAYER_ERR_INVALID_ARG;
        }
    } else {
        p->backend_width = info->width; p->backend_height = info->height;
        struct hosino_media_player *self = p;
        esp_video_render_backend_cfg_t cfg = { .ops = &video_ops, .cfg = &self, .cfg_size = sizeof(self) };
        int ret = esp_video_render_set_display(p->video, &cfg);
        if (ret != ESP_VIDEO_RENDER_ERR_OK) {
            mark_sink_error(p, ret == ESP_VIDEO_RENDER_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL);
            return ESP_PLAYER_ERR_FAIL;
        }
    }
    esp_gmf_video_dec_cfg_t decoder_cfg = {
        .codec_cc = p->cfg.video_decoder == HOSINO_MEDIA_DECODER_JPEGDEC ?
                    HOSINO_JPEGDEC_CODEC_CC : ESP_VIDEO_DEC_SW_MJPEG_TAG,
    };
    const int ret = esp_gmf_video_dec_init(&decoder_cfg, out);
    if (ret != ESP_GMF_ERR_OK) {
        mark_sink_error(p, ret == ESP_GMF_ERR_MEMORY_LACK ? ESP_ERR_NO_MEM : ESP_FAIL);
        return ret == ESP_GMF_ERR_MEMORY_LACK ? ESP_PLAYER_ERR_NO_MEM : ESP_PLAYER_ERR_FAIL;
    }
    if (esp_gmf_video_dec_set_dst_format(*out, ESP_VIDEO_CODEC_PIXEL_FMT_RGB565_LE) != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(*out);
        *out = NULL;
        mark_sink_error(p, ESP_ERR_NOT_SUPPORTED);
        return ESP_PLAYER_ERR_FAIL;
    }
    ((esp_gmf_video_element_t *)*out)->src_info.format_id = codec;
    ESP_LOGI(TAG, "Video source %ux%u @ %u FPS; RGB565 borrowed sink; decoder=%s",
             info->width, info->height, info->fps,
             p->cfg.video_decoder == HOSINO_MEDIA_DECODER_JPEGDEC ? "B-JPEGDEC" : "A-ESPRESSIF");
    return ESP_PLAYER_ERR_OK;
}
static esp_player_err_t player_event(esp_player_event_msg_t *event, void *ctx)
{
    struct hosino_media_player *p = ctx;
    if (!event) return ESP_PLAYER_ERR_OK;
    int error_source = 0;
    uint64_t completed_seek_target = 0;
    int64_t completed_seek_start = 0;
    if (event->event_type == ESP_PLAYER_EVENT_ERROR && event->data && event->data_len >= sizeof(esp_player_error_source_t))
        error_source = *(esp_player_error_source_t *)event->data;
    portENTER_CRITICAL(&s_snapshot_mux);
    // A failed video track can be followed by PLAYED from surviving audio.
    // Keep the failure visible until the control worker stops/reopens it.
    if (!p->stopping && !p->sink_error &&
        (p->snapshot.last_error == ESP_OK || event->event_type == ESP_PLAYER_EVENT_ERROR)) {
        switch (event->event_type) {
        case ESP_PLAYER_EVENT_PLAYED: p->snapshot.state = HOSINO_MEDIA_PLAYING; break;
        case ESP_PLAYER_EVENT_PAUSED: p->snapshot.state = HOSINO_MEDIA_PAUSED; break;
        case ESP_PLAYER_EVENT_SEEK_DONE:
            completed_seek_target = p->seek_target_ms;
            completed_seek_start = p->seek_started_us;
            p->snapshot.position_ms = p->seek_target_ms;
            p->snapshot.seeking = false;
            ++p->position_epoch;
            break;
        case ESP_PLAYER_EVENT_FINISHED:
            p->snapshot.state = HOSINO_MEDIA_FINISHED;
            p->snapshot.seeking = false;
            ++p->position_epoch;
            break;
        case ESP_PLAYER_EVENT_ERROR:
            p->snapshot.state = HOSINO_MEDIA_ERROR; p->snapshot.last_error = ESP_FAIL;
            p->snapshot.seeking = false;
            ++p->position_epoch;
            p->snapshot.backend_error = error_source; break;
        default: break;
        }
    }
    portEXIT_CRITICAL(&s_snapshot_mux);
    // VIDEO_INFO_PARSED is emitted immediately before the vendor creates its
    // video-render task. Its external stack still needs an internal TCB.
    // These are ordinary worker events, never allocator-failure callbacks.
    if (event->event_type == ESP_PLAYER_EVENT_VIDEO_INFO_PARSED) {
        log_media_memory("before video-render task");
    } else if (event->event_type == ESP_PLAYER_EVENT_SEEK_DONE) {
        ESP_LOGI(TAG, "SEEK_DONE target=%" PRIu64 " ms elapsed=%" PRIi64 " us",
                 completed_seek_target, esp_timer_get_time() - completed_seek_start);
    } else if (event->event_type == ESP_PLAYER_EVENT_ERROR) {
        ESP_LOGE(TAG, "ESP Player runtime error source=%d", error_source);
        log_media_memory("player error");
    }
    return ESP_PLAYER_ERR_OK;
}

static esp_err_t register_element(esp_gmf_pool_handle_t pool, esp_gmf_element_handle_t el, int init_result)
{
    if (init_result != ESP_GMF_ERR_OK || !el) return init_result == ESP_GMF_ERR_MEMORY_LACK ? ESP_ERR_NO_MEM : ESP_FAIL;
    if (esp_gmf_pool_register_element(pool, el, NULL) != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(el); return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t configure_player_tasks(struct hosino_media_player *p)
{
    /* Keep the vendor's stack sizes, memory placement and core assignments.
     * Both PCM production and hand-off must outrank JPEG work; raising only
     * the final I2S writer (the mixer is already priority 20) cannot feed it.
     * The shared demux also supplies audio, so give it a smaller boost.
     * Blocking reads/writes still yield; this does not bypass AV sync waits.
     */
    const esp_player_task_config_t tasks = {
        .extractor     = { .stack = 5120, .prio = p->audio_owned ? 7 : 5, .core = 0, .stack_in_ext = 1 },
        .audio_decoder = { .stack = 5120, .prio = 9,  .core = 1, .stack_in_ext = 1 },
        .audio_render  = { .stack = 5120, .prio = 10, .core = 0, .stack_in_ext = 1 },
        .video_decoder = { .stack = 5120, .prio = 5,  .core = 0, .stack_in_ext = 1 },
        .video_render  = { .stack = 5120, .prio = 5,  .core = 1, .stack_in_ext = 1 },
    };
    const int native = esp_player_set_task_config(p->player, &tasks);
    if (native != ESP_PLAYER_ERR_OK) return player_error(native);
    ESP_LOGI(TAG, "MEDIA_TASKS extractor=%u/core0 adec=9/core1 arender=10/core0 vdec=5/core0 vrender=5/core1 mixer=%d/core%d stack=5120/ext",
             (unsigned)tasks.extractor.prio, CONFIG_ESP_AUDIO_RENDER_MIXER_THREAD_PRIORITY,
             CONFIG_ESP_AUDIO_RENDER_MIXER_THREAD_CORE_ID);
    return ESP_OK;
}

static esp_err_t create_audio(struct hosino_media_player *p, esp_audio_render_stream_handle_t *stream)
{
    if (esp_gmf_pool_init(&p->audio_pool) != ESP_GMF_ERR_OK) return ESP_ERR_NO_MEM;
    esp_gmf_element_handle_t el = NULL;
    esp_ae_ch_cvt_cfg_t ch = DEFAULT_ESP_GMF_CH_CVT_CONFIG();
    int native = esp_gmf_ch_cvt_init(&ch, &el);
    esp_err_t err = register_element(p->audio_pool, el, native); if (err != ESP_OK) return err;
    esp_ae_bit_cvt_cfg_t bit = DEFAULT_ESP_GMF_BIT_CVT_CONFIG(); el = NULL;
    native = esp_gmf_bit_cvt_init(&bit, &el);
    err = register_element(p->audio_pool, el, native); if (err != ESP_OK) return err;
    esp_ae_rate_cvt_cfg_t rate = DEFAULT_ESP_GMF_RATE_CVT_CONFIG(); el = NULL;
    native = esp_gmf_rate_cvt_init(&rate, &el);
    err = register_element(p->audio_pool, el, native); if (err != ESP_OK) return err;
    esp_audio_render_cfg_t cfg = { .max_stream_num = 1, .out_writer = audio_write, .out_ctx = p,
        .out_sample_info = { .sample_rate = 48000, .bits_per_sample = 16, .channel = 2 }, .pool = p->audio_pool };
    native = esp_audio_render_create(&cfg, &p->audio);
    if (native != ESP_AUDIO_RENDER_ERR_OK) return native == ESP_AUDIO_RENDER_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    native = esp_audio_render_stream_get(p->audio, ESP_AUDIO_RENDER_FIRST_STREAM, stream);
    return native == ESP_AUDIO_RENDER_ERR_OK ? ESP_OK : ESP_FAIL;
}
static esp_err_t create_video(struct hosino_media_player *p)
{
    if (esp_gmf_pool_init(&p->video_pool) != ESP_GMF_ERR_OK) return ESP_ERR_NO_MEM;
    esp_gmf_element_handle_t el = NULL;
    esp_imgfx_color_convert_cfg_t color = { .color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601 };
    int native = esp_gmf_video_color_convert_init(&color, &el);
    esp_err_t err = register_element(p->video_pool, el, native); if (err != ESP_OK) return err;
    esp_imgfx_crop_cfg_t crop = {0}; el = NULL;
    native = esp_gmf_video_crop_init(&crop, &el);
    err = register_element(p->video_pool, el, native); if (err != ESP_OK) return err;
    esp_video_render_cfg_t cfg = { .pool = p->video_pool, .fps = p->cfg.render_fps ? p->cfg.render_fps : 30 };
    native = esp_video_render_create(&cfg, &p->video);
    return native == ESP_VIDEO_RENDER_ERR_OK ? ESP_OK : (native == ESP_VIDEO_RENDER_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL);
}

esp_err_t hosino_media_player_create(const hosino_media_player_config_t *cfg, hosino_media_player_handle_t *out)
{
    if (!cfg || !out || (cfg->owner != HOSINO_MEDIA_OWNER_MUSIC && cfg->owner != HOSINO_MEDIA_OWNER_VIDEO) ||
        (cfg->enable_video && (!cfg->video_cb || !cfg->max_width || !cfg->max_height || cfg->max_width > 480 || cfg->max_height > 480)) ||
        (cfg->allow_video_without_audio && !cfg->enable_video) ||
        (cfg->video_decoder != HOSINO_MEDIA_DECODER_ESPRESSIF && cfg->video_decoder != HOSINO_MEDIA_DECODER_JPEGDEC)) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    esp_err_t registry_result = ensure_media_registries();
    if (registry_result != ESP_OK) return registry_result;
    struct hosino_media_player *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return ESP_ERR_NO_MEM;

    p->cfg = *cfg;
    p->owner = cfg->owner == HOSINO_MEDIA_OWNER_MUSIC ? BSP_EXTRA_AUDIO_OWNER_MUSIC : BSP_EXTRA_AUDIO_OWNER_VIDEO;
    p->snapshot.state = HOSINO_MEDIA_IDLE;
    *out = p;
    return ESP_OK;
}

esp_err_t hosino_media_player_stop(hosino_media_player_handle_t p)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_snapshot_mux);
    p->stopping = true; p->snapshot.state = HOSINO_MEDIA_STOPPING;
    p->snapshot.seeking = false;
    ++p->position_epoch;
    portEXIT_CRITICAL(&s_snapshot_mux);
    int native;
    if (p->player) {
        native = esp_player_stop(p->player);
        if (native != ESP_PLAYER_ERR_OK) return fail(p, player_error(native), native);
        native = esp_player_deinit(p->player);
        if (native != ESP_PLAYER_ERR_OK) return fail(p, player_error(native), native);
        p->player = NULL;
    }
    if (p->cfg.enable_video) hosino_media_profile_stop();
    /* No player pipeline can call either render after successful deinit. */
    if (p->video) {
        native = esp_video_render_destroy(p->video);
        if (native != ESP_VIDEO_RENDER_ERR_OK) return fail(p, ESP_FAIL, native);
        p->video = NULL;
    }
    if (p->audio) {
        native = esp_audio_render_destroy(p->audio);
        if (native != ESP_AUDIO_RENDER_ERR_OK) return fail(p, ESP_FAIL, native);
        p->audio = NULL;
    }
    portENTER_CRITICAL(&s_snapshot_mux); bool busy = p->callbacks != 0; portEXIT_CRITICAL(&s_snapshot_mux);
    if (busy) return fail(p, ESP_ERR_TIMEOUT, 0);
    if (p->audio_pool) { esp_gmf_pool_deinit(p->audio_pool); p->audio_pool = NULL; }
    if (p->video_pool) { esp_gmf_pool_deinit(p->video_pool); p->video_pool = NULL; }
    if (p->board_open) {
        esp_err_t err = bsp_extra_codec_dev_stop();
        if (err != ESP_OK) return fail(p, err, 0);
        p->board_open = false;
    }
    if (p->audio_owned) {
        esp_err_t err = bsp_extra_audio_session_release(p->owner);
        if (err != ESP_OK) return fail(p, err, 0);
        p->audio_owned = false;
    }
    storage_service_release(&p->lease);
    p->network_source = false;
    memset(p->network_authorization, 0, sizeof(p->network_authorization));
    p->network_base[0] = 0;
    p->backend_width = p->backend_height = 0;
    portENTER_CRITICAL(&s_snapshot_mux);
    p->snapshot.resources_active = false; p->snapshot.audio_enabled = false;
    p->snapshot.state = HOSINO_MEDIA_STOPPED; p->stopping = false;
    portEXIT_CRITICAL(&s_snapshot_mux);
    return ESP_OK;
}
static esp_err_t media_open_source(hosino_media_player_handle_t p, const char *path,
                                  const char *base, const char *token)
{
    esp_err_t err = hosino_media_player_stop(p);
    if (err != ESP_OK) return err;
    portENTER_CRITICAL(&s_snapshot_mux);
    p->snapshot = (hosino_media_player_snapshot_t){ .state = HOSINO_MEDIA_OPENING };
    p->sink_error = false;
    portEXIT_CRITICAL(&s_snapshot_mux);
    if (p->cfg.enable_video) hosino_media_profile_reset(path);
    p->profile_started_us = esp_timer_get_time();
    p->audio_previous_end_us = 0;
    strlcpy(p->path, path, sizeof(p->path));
    p->network_source = base != NULL;
    if (p->network_source) {
        strlcpy(p->network_base, base, sizeof(p->network_base));
        snprintf(p->network_authorization, sizeof(p->network_authorization), "Bearer %s", token);
    } else {
        err = storage_service_acquire(&p->lease);
        if (err != ESP_OK) return fail(p, err, 0);
    }
    portENTER_CRITICAL(&s_snapshot_mux); p->snapshot.resources_active = true; portEXIT_CRITICAL(&s_snapshot_mux);
    err = bsp_extra_audio_session_acquire(p->owner);
    if (err == ESP_OK) {
        p->audio_owned = true;
        /* Track possible partial board setup too; cleanup must close it. */
        p->board_open = true;
        err = bsp_extra_codec_init();
        if (err != ESP_OK) goto failed;
        err = bsp_extra_codec_set_fs(48000, 16, I2S_SLOT_MODE_STEREO);
        if (err != ESP_OK) goto failed;
        err = bsp_extra_codec_mute_set(false);
        if (err != ESP_OK) goto failed;
    } else if (p->cfg.enable_video && p->cfg.allow_video_without_audio) {
        ESP_LOGW(TAG, "Audio session busy; explicitly configured video-only playback");
    } else goto failed;
    esp_audio_render_stream_handle_t audio_stream = NULL;
    if (p->audio_owned) { err = create_audio(p, &audio_stream); if (err != ESP_OK) goto failed; }
    if (p->cfg.enable_video) { err = create_video(p); if (err != ESP_OK) goto failed; }
    esp_player_config_t cfg = ESP_PLAYER_CONFIG_DEFAULT();
    cfg.audio_render_hd = audio_stream; cfg.video_render_hd = p->video;
    cfg.hosino_managed_mjpeg_http = p->network_source;
    cfg.hosino_http_base_url = p->network_source ? p->network_base : NULL;
    cfg.hosino_http_authorization = p->network_source ? p->network_authorization : NULL;
    int native = esp_player_init(&cfg, &p->player);
    if (native != ESP_PLAYER_ERR_OK) { err = player_error(native); goto failed; }
    err = configure_player_tasks(p);
    if (err != ESP_OK) goto failed;
    if (p->cfg.enable_video && p->audio_owned) {
        /* Bounded PSRAM headroom for compressed local A/V packets. The demux
         * admission policy sheds independent JPEG frames when audio is dry;
         * leave room for the next audio packet instead of filling the old
         * 100KiB shared pool with a few JPEGs. PCM/I2S buffers are unchanged. */
        const esp_player_buffer_config_t buffers = {
            .extractor_pool_size = 256U * 1024U,
            .http_read_buf_size = p->network_source ? 256U * 1024U : 0,
        };
        native = esp_player_set_buffer_config(p->player, &buffers);
        if (native != ESP_PLAYER_ERR_OK) { err = player_error(native); goto failed; }
        ESP_LOGI(TAG, "MEDIA_BUFFERS extractor_pool_bytes=%u http_ring_bytes=%u managed=%d",
                 (unsigned)buffers.extractor_pool_size, (unsigned)buffers.http_read_buf_size, p->network_source);
    }
    native = esp_player_set_event_cb(p->player, player_event, p);
    if (native != ESP_PLAYER_ERR_OK) { err = player_error(native); goto failed; }
    if (p->cfg.enable_video) {
        esp_player_custom_elements_t hooks = { .vdec_factory = video_factory, .user_ctx = p,
            .hosino_mjpeg_width=p->network_source?p->cfg.network_video_width:0,
            .hosino_mjpeg_height=p->network_source?p->cfg.network_video_height:0,
            .hosino_mjpeg_fps=p->network_source?p->cfg.network_video_fps:0 };
        native = esp_player_set_custom_elements(p->player, &hooks);
        if (native != ESP_PLAYER_ERR_OK) { err = player_error(native); goto failed; }
    }
    const uint8_t mask = (p->audio_owned ? ESP_PLAYER_MASK_AUDIO : 0) | (p->cfg.enable_video ? ESP_PLAYER_MASK_VIDEO : 0);
    esp_player_data_src_t src = ESP_PLAYER_DATA_SRC(p->path, mask);
    src.sync_mode = p->audio_owned ? ESP_PLAYER_SYNC_MODE_AUDIO : ESP_PLAYER_SYNC_MODE_SYSTEM;
    native = esp_player_set_data_src(p->player, &src);
    if (native != ESP_PLAYER_ERR_OK) { err = player_error(native); goto failed; }
    portENTER_CRITICAL(&s_snapshot_mux); p->snapshot.audio_enabled = p->audio_owned; portEXIT_CRITICAL(&s_snapshot_mux);
    ESP_LOGI(TAG, "Opened %s %s media; exclusive audio=%d", p->network_source ? "managed network" : "local",
             p->cfg.enable_video ? "video" : "audio", p->audio_owned);
    return ESP_OK;
failed:
    {
        esp_err_t cleanup = hosino_media_player_stop(p);
        return fail(p, cleanup == ESP_OK ? err : cleanup, 0);
    }
}
esp_err_t hosino_media_player_open(hosino_media_player_handle_t p, const char *path)
{
    if (!p || !path || strnlen(path, HOSINO_MEDIA_PATH_MAX) >= HOSINO_MEDIA_PATH_MAX ||
        strncmp(path, STORAGE_SERVICE_MOUNT_POINT "/", sizeof(STORAGE_SERVICE_MOUNT_POINT)) != 0 ||
        strstr(path, "/../") || strchr(path, '?') || strchr(path, '#')) return ESP_ERR_INVALID_ARG;
    return media_open_source(p, path, NULL, NULL);
}
esp_err_t hosino_media_player_open_network(hosino_media_player_handle_t p, const char *url,
                                         const char *base, const char *token)
{
    if (!p || !p->cfg.enable_video || !hosino_network_url_allowed(url, base) ||
        !hosino_network_token_valid(token)||!hosino_mjpeg_profile_valid(p->cfg.network_video_width,
            p->cfg.network_video_height,p->cfg.network_video_fps)) return ESP_ERR_INVALID_ARG;
    return media_open_source(p, url, base, token);
}
static esp_err_t control_result(struct hosino_media_player *p, int native)
{ return native == ESP_PLAYER_ERR_OK ? ESP_OK : fail(p, player_error(native), native); }
static esp_err_t pause_resume_result(struct hosino_media_player *p, int native)
{
    // EOF / preparation may race a queued UI command. Let the caller refresh
    // state on its next poll without poisoning an otherwise healthy session.
    if (native == ESP_PLAYER_ERR_INVALID_STATE) return ESP_ERR_INVALID_STATE;
    return control_result(p, native);
}
esp_err_t hosino_media_player_start(hosino_media_player_handle_t p)
{ return (!p || !p->player) ? ESP_ERR_INVALID_STATE : control_result(p, esp_player_run(p->player)); }
esp_err_t hosino_media_player_pause(hosino_media_player_handle_t p)
{ return (!p || !p->player) ? ESP_ERR_INVALID_STATE : pause_resume_result(p, esp_player_pause(p->player)); }
esp_err_t hosino_media_player_resume(hosino_media_player_handle_t p)
{ return (!p || !p->player) ? ESP_ERR_INVALID_STATE : pause_resume_result(p, esp_player_resume(p->player)); }
esp_err_t hosino_media_player_set_volume(hosino_media_player_handle_t p, int volume)
{
    if (!p || volume < 0 || volume > 100) return ESP_ERR_INVALID_ARG;
    if (!p->player || !p->audio_owned || !p->board_open || p->stopping) return ESP_ERR_INVALID_STATE;
    return bsp_extra_codec_volume_set(volume, NULL);
}
esp_err_t hosino_media_player_seek(hosino_media_player_handle_t p, uint64_t ms)
{
    if (!p || !p->player) return ESP_ERR_INVALID_STATE;
    if (p->network_source) return ESP_ERR_NOT_SUPPORTED;
    portENTER_CRITICAL(&s_snapshot_mux);
    if (p->snapshot.seeking || p->stopping || p->snapshot.last_error != ESP_OK) {
        portEXIT_CRITICAL(&s_snapshot_mux);
        return ESP_ERR_INVALID_STATE;
    }
    // The API returns when seek begins, and SEEK_DONE may even arrive before
    // that return. Publish first; never set seeking again after the call.
    p->snapshot.seeking = true;
    p->seek_target_ms = ms;
    p->seek_started_us = esp_timer_get_time();
    ++p->position_epoch;
    portEXIT_CRITICAL(&s_snapshot_mux);
    ESP_LOGI(TAG, "SEEK_REQUEST target=%" PRIu64 " ms", ms);
    int native = esp_player_seek(p->player, ms);
    if (native != ESP_PLAYER_ERR_OK) {
        portENTER_CRITICAL(&s_snapshot_mux);
        p->snapshot.seeking = false;
        ++p->position_epoch;
        portEXIT_CRITICAL(&s_snapshot_mux);
    }
    // Controls can race EOF or a previous asynchronous seek. An unsupported
    // seek is a control limitation, not a decoder/output failure.
    if (native == ESP_PLAYER_ERR_INVALID_STATE) return ESP_ERR_INVALID_STATE;
    if (native == ESP_PLAYER_ERR_NOT_SUPPORT) return ESP_ERR_NOT_SUPPORTED;
    return control_result(p, native);
}
esp_err_t hosino_media_player_poll(hosino_media_player_handle_t p)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_snapshot_mux);
    bool sink_error = p->sink_error;
    esp_err_t saved = p->snapshot.last_error;
    const uint32_t position_epoch = p->position_epoch;
    const bool seek_expired = p->snapshot.seeking && esp_timer_get_time() - p->seek_started_us > 15000000;
    if (seek_expired) {
        // Vendor decoder-flush timeout may not emit ERROR or SEEK_DONE. Keep
        // uncertain pipelines owned until normal stop; do not permit another seek.
        p->snapshot.seeking = false;
        p->snapshot.last_error = ESP_ERR_TIMEOUT;
        p->snapshot.state = HOSINO_MEDIA_ERROR;
        ++p->position_epoch;
    }
    portEXIT_CRITICAL(&s_snapshot_mux);
    if (seek_expired) {
        ESP_LOGE(TAG, "Seek did not complete within 15 s; stopping playback");
        esp_err_t err = hosino_media_player_stop(p);
        return fail(p, err == ESP_OK ? ESP_ERR_TIMEOUT : err, 0);
    }
    if (sink_error && p->player) {
        esp_err_t err = hosino_media_player_stop(p);
        return fail(p, err == ESP_OK ? saved : err, 0);
    }
    if (!p->player) return ESP_OK;
    uint64_t position = 0, duration = 0;
    bool got_position = esp_player_get_play_time(p->player, &position) == ESP_PLAYER_ERR_OK;
    bool got_duration = esp_player_get_duration(p->player, &duration) == ESP_PLAYER_ERR_OK;
    esp_player_state_t state;
    bool got_state = esp_player_get_state(p->player, &state) == ESP_PLAYER_ERR_OK;
    portENTER_CRITICAL(&s_snapshot_mux);
    // A getter racing seek completion must not overwrite the completed target
    // with an intermediate zero returned while render clocks were being reset.
    if (got_position && !p->snapshot.seeking && position_epoch == p->position_epoch)
        p->snapshot.position_ms = position;
    if (got_duration) p->snapshot.duration_ms = duration;
    if (got_state && !p->sink_error && !p->stopping && p->snapshot.last_error == ESP_OK) {
        switch (state) {
        case ESP_PLAYER_STATE_PLAYING: p->snapshot.state = HOSINO_MEDIA_PLAYING; break;
        case ESP_PLAYER_STATE_PAUSED: p->snapshot.state = HOSINO_MEDIA_PAUSED; break;
        case ESP_PLAYER_STATE_FINISHED:
            p->snapshot.state = HOSINO_MEDIA_FINISHED;
            if (p->snapshot.seeking) { p->snapshot.seeking = false; ++p->position_epoch; }
            break;
        case ESP_PLAYER_STATE_ERROR:
            p->snapshot.state = HOSINO_MEDIA_ERROR; p->snapshot.last_error = ESP_FAIL;
            if (p->snapshot.seeking) { p->snapshot.seeking = false; ++p->position_epoch; }
            break;
        default: break;
        }
    }
    portEXIT_CRITICAL(&s_snapshot_mux);
    const int64_t profile_now = esp_timer_get_time();
    if (p->cfg.enable_video && profile_now - p->profile_started_us >= 2000000) {
        hosino_media_profile_snapshot_t m;
        hosino_media_profile_take(&m);
        hosino_media_player_snapshot_t current;
        hosino_media_player_get_snapshot(p, &current);
        // These are elapsed wall times, including scheduling/preemption, not
        // CPU utilization. Normal I2S writes are expected to block in real time.
        ESP_LOGI(TAG, "MEDIA_PROFILE window_ms=%" PRIi64 " width=%u height=%u source_fps=%u decoder=%s"
                 " decode_ok=%" PRIu32 " decode_non_ok=%" PRIu32 " decode_avg_us=%" PRIu64 " decode_max_us=%" PRIu32
                 " sink_n=%" PRIu32 " sink_avg_us=%" PRIu64 " sink_max_us=%" PRIu32
                 " audio_n=%" PRIu32 " audio_write_avg_us=%" PRIu64 " audio_gap_avg_us=%" PRIu64 " audio_gap_max_us=%" PRIu32,
                 (profile_now - p->profile_started_us) / 1000,
                 (unsigned)current.source_width, (unsigned)current.source_height, (unsigned)current.source_fps,
                 p->cfg.video_decoder == HOSINO_MEDIA_DECODER_JPEGDEC ? "B-JPEGDEC" : "A-ESPRESSIF",
                 m.decode_ok, m.decode_error, m.decode_ok ? m.decode_total_us / m.decode_ok : 0, m.decode_max_us,
                 m.video_sink_count, m.video_sink_count ? m.video_sink_total_us / m.video_sink_count : 0, m.video_sink_max_us,
                 m.audio_write_count, m.audio_write_count ? m.audio_write_total_us / m.audio_write_count : 0,
                 m.audio_gap_count ? m.audio_gap_total_us / m.audio_gap_count : 0, m.audio_gap_max_us);
        const hosino_media_wait_sample_t *skip = &m.pipeline[HOSINO_PLAYER_DIAG_AV_YIELD_SKIPPED];
        const hosino_media_wait_sample_t *video_yield = &m.pipeline[HOSINO_PLAYER_DIAG_VIDEO_ONLY_YIELD];
        const hosino_media_wait_sample_t *net_yield = &m.pipeline[HOSINO_PLAYER_DIAG_AV_YIELD_NETWORK];
        const hosino_media_wait_sample_t *hold = &m.pipeline[HOSINO_PLAYER_DIAG_DEMUX_AUDIO_HOLD];
        const hosino_media_wait_sample_t *pool = &m.pipeline[HOSINO_PLAYER_DIAG_DEMUX_POOL_WAIT];
        const hosino_media_wait_sample_t *push = &m.pipeline[HOSINO_PLAYER_DIAG_VIDEO_QUEUE_PUSH];
        const hosino_media_wait_sample_t *input = &m.pipeline[HOSINO_PLAYER_DIAG_AUDIO_INPUT_ACQUIRE];
        // pcm_ms is accepted 48kHz/stereo/16bit PCM, not proof of audible output.
        // skipped_request_ms is a counterfactual request amount, not measured savings.
        ESP_LOGI(TAG, "MEDIA_PIPE window_ms=%" PRIi64 " state=%d seeking=%d"
                 " pcm_bytes=%" PRIu64 " pcm_ms=%" PRIu64 " short=%" PRIu32 " write_errors=%" PRIu32
                 " av_skip_n=%" PRIu32 " skipped_request_ms=%" PRIu64
                 " vo_yield_n=%" PRIu32 " vo_yield_ms=%" PRIu64 " net_yield_n=%" PRIu32 " net_yield_ms=%" PRIu64
                 " hold_n=%" PRIu32 " hold_ms=%" PRIu64 " hold_max_us=%" PRIu32
                 " pool_n=%" PRIu32 " pool_ms=%" PRIu64 " pool_max_us=%" PRIu32
                 " push_n=%" PRIu32 " push_ms=%" PRIu64 " push_max_us=%" PRIu32 " push_non_ok=%" PRIu32
                 " ain_n=%" PRIu32 " ain_ms=%" PRIu64 " ain_max_us=%" PRIu32 " ain_bytes=%" PRIu64 " ain_non_ok=%" PRIu32,
                 (profile_now - p->profile_started_us) / 1000, (int)current.state, current.seeking,
                 m.audio_written_bytes, m.audio_written_bytes * 1000 / 192000, m.audio_short_writes, m.audio_write_errors,
                 skip->count, skip->amount / 1000,
                 video_yield->count, video_yield->total_us / 1000, net_yield->count, net_yield->total_us / 1000,
                 hold->count, hold->total_us / 1000, hold->max_us,
                 pool->count, pool->total_us / 1000, pool->max_us,
                 push->count, push->total_us / 1000, push->max_us, push->non_ok,
                 input->count, input->total_us / 1000, input->max_us, input->amount, input->non_ok);
        const hosino_media_file_sample_t *read = &m.file[HOSINO_FILE_DIAG_READ];
        const hosino_media_file_sample_t *seek = &m.file[HOSINO_FILE_DIAG_SEEK];
        const hosino_media_wait_sample_t *drop_audio = &m.pipeline[HOSINO_PLAYER_DIAG_DROP_LATE_AUDIO];
        const hosino_media_wait_sample_t *drop_wall = &m.pipeline[HOSINO_PLAYER_DIAG_DROP_LATE_WALL];
        const hosino_media_wait_sample_t *anchors = &m.pipeline[HOSINO_PLAYER_DIAG_CLOCK_REANCHOR];
        // Lateness is a timestamp delta, not CPU execution or a blocked wait.
        const hosino_player_clock_sample_t *clock = &m.last_clock;
        ESP_LOGI(TAG, "MEDIA_CLOCK samples=%" PRIu32 " video_pts_ms=%" PRIu64 " audio_pts_ms=%" PRIu64
                 " anchor_pts_ms=%" PRIu64 " anchor_elapsed_ms=%" PRIu64 " source_fps=%" PRIu32
                 " threshold_ms=%" PRIu32 " flags=%" PRIu32 " decision=%" PRIu32 " reanchor_reason=%" PRIu32,
                 m.clock_samples, clock->video_pts_ms, clock->audio_pts_ms, clock->anchor_pts_ms,
                 clock->anchor_elapsed_ms, clock->source_fps, clock->threshold_ms, clock->flags, clock->decision, clock->reanchor_reason);
        ESP_LOGI(TAG, "MEDIA_DROP window_ms=%" PRIi64 " audio_n=%" PRIu32 " audio_late_max_us=%" PRIu32
                 " wall_n=%" PRIu32 " wall_late_max_us=%" PRIu32 " reanchor_n=%" PRIu32
                 " pcm_gap_accept_n=%" PRIu32 " pcm_sync_drop_n=%" PRIu32
                 " demux_video_drop_n=%" PRIu32 " demux_video_drop_fail_n=%" PRIu32
                 " video_packet_max_bytes=%" PRIu32,
                 (profile_now - p->profile_started_us) / 1000,
                 drop_audio->count, drop_audio->max_us, drop_wall->count, drop_wall->max_us, anchors->count,
                 m.pipeline[HOSINO_PLAYER_DIAG_AUDIO_SYNC_GAP_ACCEPTED].count,
                 m.pipeline[HOSINO_PLAYER_DIAG_AUDIO_SYNC_DROP].count,
                 m.pipeline[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY].count -
                     m.pipeline[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY].non_ok,
                 m.pipeline[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY].non_ok,
                 m.pipeline[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_PACKET].amount_max);
        ESP_LOGI(TAG, "MEDIA_FILE window_ms=%" PRIi64 " read_n=%" PRIu32 " bytes=%" PRIu64
                 " read_us=%" PRIu64 " read_max_us=%" PRIu32 " over20=%" PRIu32 " over40=%" PRIu32 " over80=%" PRIu32
                 " read_errors=%" PRIu32 " eof=%" PRIu32 " seek_n=%" PRIu32 " seek_us=%" PRIu64
                 " seek_max_us=%" PRIu32 " seek_errors=%" PRIu32,
                 (profile_now - p->profile_started_us) / 1000, read->count, read->bytes,
                 read->total_us, read->max_us, read->over_20ms, read->over_40ms, read->over_80ms,
                 read->errors, read->eof, seek->count, seek->total_us, seek->max_us, seek->errors);
        p->profile_started_us = profile_now;
    }
    return ESP_OK;
}
esp_err_t hosino_media_player_get_snapshot(hosino_media_player_handle_t p, hosino_media_player_snapshot_t *snapshot)
{
    if (!p || !snapshot) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_snapshot_mux); *snapshot = p->snapshot; portEXIT_CRITICAL(&s_snapshot_mux);
    return ESP_OK;
}
esp_err_t hosino_media_player_join(hosino_media_player_handle_t p, uint32_t timeout_ms)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    do {
        portENTER_CRITICAL(&s_snapshot_mux);
        bool done = !p->snapshot.resources_active && p->callbacks == 0;
        portEXIT_CRITICAL(&s_snapshot_mux);
        if (done) return ESP_OK;
        if (!timeout_ms || esp_timer_get_time() >= deadline) break;
        vTaskDelay(pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1);
    } while (true);
    return ESP_ERR_TIMEOUT;
}
esp_err_t hosino_media_player_destroy(hosino_media_player_handle_t p)
{
    if (!p) return ESP_OK;
    esp_err_t err = hosino_media_player_stop(p);
    if (err != ESP_OK) return err;
    err = hosino_media_player_join(p, 0);
    if (err != ESP_OK) return err;
    heap_caps_free(p);
    return ESP_OK;
}

