#include "sdkconfig.h"

#if CONFIG_HOSINO_PERF_DEMO

#include "hosino_perf_overlay.h"
#include "hosino_ui_fonts.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#if CONFIG_HOSINO_DISPLAY_BUFFERED
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#endif
#include "freertos/portmacro.h"
#if CONFIG_HOSINO_DISPLAY_CORNERS
#include "hosino_corner_clip.h"
#endif

#if defined(CONFIG_APP_LCD_QSPI_80MHZ)
#define HOSINO_LCD_CLOCK_MHZ 80
#elif defined(CONFIG_APP_LCD_QSPI_40MHZ)
#define HOSINO_LCD_CLOCK_MHZ 40
#else
#error "Select the display QSPI clock before building the FPS overlay"
#endif

/* The full ESP-IDF target does not guarantee lock-free GCC atomics. Use its
 * cross-core spinlock instead; the wrapper may run in an ISR or a task. */
static portMUX_TYPE s_frame_lock = portMUX_INITIALIZER_UNLOCKED;
static lv_display_t *s_target_display;
static uint32_t s_completed_frames;

/* All remaining state is used only by the GUI task with its lock held. */
static lv_obj_t *s_label;
static uint32_t s_previous_frames;
static int64_t s_previous_sample_us;
static char s_previous_text[96];

void hosino_perf_overlay_get_snapshot(uint32_t *completed_frames, int64_t *time_us)
{
    if (completed_frames == NULL || time_us == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_frame_lock);
    *completed_frames = s_completed_frames;
    portEXIT_CRITICAL(&s_frame_lock);
    *time_us = esp_timer_get_time();
}

LV_ATTRIBUTE_FLUSH_READY void __real_lv_display_flush_ready(lv_display_t *disp);

LV_ATTRIBUTE_FLUSH_READY void __wrap_lv_display_flush_ready(lv_display_t *disp)
{
    const bool is_last = disp != NULL && lv_display_flush_is_last(disp);

    /* In the normal SPI path this runs after DMA completes. Count only the last
     * flush of a refresh, not each partial buffer. The ISR does no GUI writes,
     * allocation, logging, division, or time sampling. Driver error-release
     * paths can also notify completion: this is UI FPS, not optical scan Hz. */
    portENTER_CRITICAL_SAFE(&s_frame_lock);
    if (s_target_display != NULL && disp == s_target_display && is_last) {
        ++s_completed_frames;
    }
    portEXIT_CRITICAL_SAFE(&s_frame_lock);

    __real_lv_display_flush_ready(disp);
}

static void update_fps_label(lv_timer_t *timer)
{
    (void)timer;

    const int64_t now_us = esp_timer_get_time();
    const int64_t elapsed_us = now_us - s_previous_sample_us;
    if (elapsed_us <= 0) {
        return;
    }

    portENTER_CRITICAL(&s_frame_lock);
    const uint32_t completed = s_completed_frames;
    portEXIT_CRITICAL(&s_frame_lock);
    /* Unsigned subtraction also handles the monotonic counter wrapping. */
    const uint32_t frames = completed - s_previous_frames;
    const uint32_t tenths = (uint32_t)(((uint64_t)frames * 10000000ULL +
                                      (uint64_t)elapsed_us / 2U) / (uint64_t)elapsed_us);
    s_previous_frames = completed;
    s_previous_sample_us = now_us;

    char text[sizeof(s_previous_text)];
    snprintf(text, sizeof(text), "%dMHz | 界面 %" PRIu32 ".%" PRIu32 " 帧/秒",
             HOSINO_LCD_CLOCK_MHZ, tenths / 10U, tenths % 10U);
#if CONFIG_HOSINO_DISPLAY_BUFFERED
    const size_t used = strlen(text);
#if CONFIG_HOSINO_DISPLAY_CORNERS
    hosino_corner_clip_stats_t stats = {0};
    hosino_corner_clip_get_stats(&stats);
    const uint32_t saved_tenths = stats.requested_bytes && stats.sent_bytes <= stats.requested_bytes
        ? (uint32_t)((stats.requested_bytes - stats.sent_bytes) * 1000ULL / stats.requested_bytes) : 0;
    snprintf(text + used, sizeof(text) - used, "\nB%dx2 | C%u %s | TX -%" PRIu32 ".%" PRIu32 "%%",
             CONFIG_HOSINO_DISPLAY_BUFFER_HEIGHT, (unsigned)hosino_corner_clip_get_radius(),
             hosino_corner_clip_is_enabled() ? "ON" : "OFF", saved_tenths / 10U, saved_tenths % 10U);
    if (stats.failures > 0) {
        snprintf(text + used, sizeof(text) - used, "\nB%dx2 | C%u %s | TX ERROR",
                 CONFIG_HOSINO_DISPLAY_BUFFER_HEIGHT, (unsigned)hosino_corner_clip_get_radius(),
                 hosino_corner_clip_is_enabled() ? "ON" : "OFF");
    }
#else
    const uint32_t free_tenths = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) * 10ULL / (1024ULL * 1024ULL));
    snprintf(text + used, sizeof(text) - used, "\n双缓冲 %d 行 | 内存 %" PRIu32 ".%" PRIu32 "MB",
             CONFIG_HOSINO_DISPLAY_BUFFER_HEIGHT, free_tenths / 10U, free_tenths % 10U);
#endif
    static int64_t last_health_log_us;
    if (now_us - last_health_log_us >= 5000000) {
        last_health_log_us = now_us;
        ESP_LOGI("ui_health", "frames=%" PRIu32 " lvgl_stack_min_free=%u bytes internal_free=%u largest=%u",
                 completed, (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
#endif

    /* Avoid invalidating the HUD when its displayed value is unchanged. The
     * HUD's own changed text still contributes a small amount of idle traffic. */
    if (strcmp(text, s_previous_text) != 0) {
        lv_label_set_text(s_label, text);
        memcpy(s_previous_text, text, strlen(text) + 1U);
    }
}

void hosino_perf_overlay_init(lv_display_t *disp)
{
    if (disp == NULL) {
        disp = lv_display_get_default();
    }
    ESP_ERROR_CHECK(disp != NULL ? ESP_OK : ESP_ERR_INVALID_ARG);

    portENTER_CRITICAL(&s_frame_lock);
    lv_display_t *target = s_target_display;
    portEXIT_CRITICAL(&s_frame_lock);
    if (target != NULL) {
        ESP_ERROR_CHECK(target == disp ? ESP_OK : ESP_ERR_INVALID_STATE);
        return;
    }

    lv_obj_t *layer = lv_display_get_layer_sys(disp);
    ESP_ERROR_CHECK(layer != NULL ? ESP_OK : ESP_ERR_INVALID_STATE);
    s_label = lv_label_create(layer);
    ESP_ERROR_CHECK(s_label != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    lv_obj_remove_flag(s_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_label, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_text_font(s_label, &hosino_font_16, 0);
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_label, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_label, 6, 0);
    lv_obj_set_style_pad_hor(s_label, 8, 0);
    lv_obj_set_style_pad_ver(s_label, 4, 0);
    lv_obj_set_style_border_width(s_label, 0, 0);
    snprintf(s_previous_text, sizeof(s_previous_text), "%dMHz | 界面 --.- 帧/秒",
             HOSINO_LCD_CLOCK_MHZ);
#if CONFIG_HOSINO_DISPLAY_BUFFERED
    const size_t used = strlen(s_previous_text);
#if CONFIG_HOSINO_DISPLAY_CORNERS
    snprintf(s_previous_text + used, sizeof(s_previous_text) - used,
             "\nB%dx2 | C0 OFF | TX -0.0%%", CONFIG_HOSINO_DISPLAY_BUFFER_HEIGHT);
#else
    snprintf(s_previous_text + used, sizeof(s_previous_text) - used,
             "\n双缓冲 %d 行 | 内存 --.-MB", CONFIG_HOSINO_DISPLAY_BUFFER_HEIGHT);
#endif
#endif
    lv_label_set_text(s_label, s_previous_text);
    lv_obj_align(s_label, LV_ALIGN_BOTTOM_MID, 0, -32);

    lv_timer_t *timer = lv_timer_create(update_fps_label, 1000, NULL);
    ESP_ERROR_CHECK(timer != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    s_previous_sample_us = esp_timer_get_time();
    /* Publish only after all state is initialized. This display is not deleted
     * during normal application switching; its system layer keeps the HUD alive. */
    portENTER_CRITICAL(&s_frame_lock);
    s_previous_frames = s_completed_frames;
    s_target_display = disp;
    portEXIT_CRITICAL(&s_frame_lock);
}

#endif /* CONFIG_HOSINO_PERF_DEMO */
