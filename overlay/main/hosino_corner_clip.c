#include "hosino_corner_clip.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

/* CORNER_GEOMETRY_BEGIN
 * Keep this block plain integer C. The host test compiles these exact function
 * bodies as C# with type spelling/accessibility substitutions only. */
static uint32_t corner_ceil_sqrt(uint32_t value)
{
    uint32_t low = 0;
    uint32_t high = 240;
    while (low < high) {
        uint32_t middle = (low + high) / 2;
        if (middle * middle < value) low = middle + 1;
        else high = middle;
    }
    return low;
}

int hosino_corner_clip_row_inset(int y, int radius)
{
    if (y < 0 || y >= 480 || radius <= 0) return 0;
    if (radius > 240) radius = 240;
    int edge_distance = y < 240 ? y : 479 - y;
    if (edge_distance >= radius) return 0;
    /* Use the row edge closest to the circle centre, and ceil the square
     * root: partially intersecting boundary pixels are always retained. */
    int dy = radius - edge_distance - 1;
    int extent = (int)corner_ceil_sqrt((uint32_t)(radius * radius - dy * dy));
    /* Also retain a pixel whose edge merely touches the circle. */
    int inset = radius - extent - 1;
    return inset > 0 ? inset & ~1 : 0;
}

hosino_corner_clip_box_t hosino_corner_clip_bounds(int x1, int y1, int x2, int y2,
                                                uint16_t radius)
{
    hosino_corner_clip_box_t result = {0};
    if (x1 < 0 || y1 < 0 || x2 > 480 || y2 > 480 || x1 >= x2 || y1 >= y2 ||
            ((x1 | y1 | x2 | y2) & 1) != 0) return result;
    int left = x2;
    int right = x1;
    int top = y2;
    int bottom = y1;
    for (int y = y1; y < y2; ++y) {
        int inset = hosino_corner_clip_row_inset(y, radius);
        int row_left = x1 > inset ? x1 : inset;
        int row_right = x2 < 480 - inset ? x2 : 480 - inset;
        if (row_left >= row_right) continue;
        if (row_left < left) left = row_left;
        if (row_right > right) right = row_right;
        if (y < top) top = y;
        bottom = y + 1;
    }
    if (left >= right) return result;
    result.x1 = left & ~1;
    result.x2 = (right + 1) & ~1;
    result.y1 = top & ~1;
    result.y2 = (bottom + 1) & ~1;
    result.visible = true;
    return result;
}
/* CORNER_GEOMETRY_END */

typedef struct {
    lv_display_t *display;
    esp_lcd_panel_handle_t panel;
    uint8_t *scratch;
    size_t scratch_bytes;
    uint16_t radius;
    bool enabled;
    bool installed;
    hosino_corner_clip_stats_t stats;
} corner_context_t;

static corner_context_t s_corner;
static const char *TAG = "corner_clip";

static esp_err_t corner_submit(corner_context_t *ctx, esp_lcd_panel_handle_t panel,
                               int x1, int y1, int x2, int y2, const void *pixels)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, pixels);
    if (err == ESP_OK) {
        ctx->stats.sent_bytes += (uint64_t)(x2 - x1) * (y2 - y1) * 2;
    } else {
        ++ctx->stats.failures;
    }
    return err;
}

static esp_err_t corner_draw(lv_display_t *disp, esp_lcd_panel_handle_t panel,
                             int x1, int y1, int x2, int y2,
                             const void *color_map, void *user_ctx)
{
    corner_context_t *ctx = user_ctx;
    if (ctx == NULL || disp != ctx->display || panel != ctx->panel ||
            color_map == NULL || x1 >= x2 || y1 >= y2) {
        if (ctx != NULL) ++ctx->stats.failures;
        return ESP_ERR_INVALID_ARG;
    }
    ++ctx->stats.requested_flushes;
    const size_t original_bytes = (size_t)(x2 - x1) * (size_t)(y2 - y1) * 2;
    ctx->stats.requested_bytes += original_bytes;
    if (!ctx->enabled || ctx->radius == 0) {
        return corner_submit(ctx, panel, x1, y1, x2, y2, color_map);
    }
    /* Preserve the original path for unexpected geometry instead of silently
     * dropping visible data. The BSP normally rounds all boundaries to 2 px. */
    if (x1 < 0 || y1 < 0 || x2 > 480 || y2 > 480 ||
            ((x1 | y1 | x2 | y2) & 1) != 0 ||
            lv_display_get_color_format(disp) != LV_COLOR_FORMAT_RGB565 ||
            original_bytes > ctx->scratch_bytes) {
        ++ctx->stats.fallback_flushes;
        return corner_submit(ctx, panel, x1, y1, x2, y2, color_map);
    }

    const hosino_corner_clip_box_t box = hosino_corner_clip_bounds(x1, y1, x2, y2, ctx->radius);
    if (!box.visible) {
        ++ctx->stats.skipped_flushes;
        /* No transfer was initiated. The adapter handles NOT_ALLOWED by
         * releasing this flush once. Never return OK without a done ISR. */
        return ESP_ERR_NOT_ALLOWED;
    }
    if (box.x1 == x1 && box.x2 == x2 && box.y1 == y1 && box.y2 == y2) {
        return corner_submit(ctx, panel, x1, y1, x2, y2, color_map);
    }
    ++ctx->stats.clipped_flushes;
    const size_t source_stride = (size_t)(x2 - x1) * 2;
    const size_t row_bytes = (size_t)(box.x2 - box.x1) * 2;
    const uint8_t *src = (const uint8_t *)color_map +
                         (size_t)(box.y1 - y1) * source_stride + (size_t)(box.x1 - x1) * 2;
    uint8_t *dst = ctx->scratch;
    for (int y = box.y1; y < box.y2; ++y) {
        memcpy(dst, src, row_bytes);
        dst += row_bytes;
        src += source_stride;
    }
    /* The adapter already packed and byte-swapped RGB565. One submission
     * means exactly one final done ISR. LVGL waits before invoking us again,
     * so the persistent scratch cannot be overwritten while DMA owns it. */
    return corner_submit(ctx, panel, box.x1, box.y1, box.x2, box.y2, ctx->scratch);
}

esp_err_t hosino_corner_clip_attach(const esp_lv_adapter_display_config_t *cfg,
                                  lv_display_t *disp)
{
    if (cfg == NULL || disp == NULL || cfg->panel == NULL || cfg->panel_io == NULL ||
            cfg->profile.interface != ESP_LV_ADAPTER_PANEL_IF_OTHER ||
            cfg->profile.rotation != ESP_LV_ADAPTER_ROTATE_0 ||
            cfg->profile.hor_res != 480 || cfg->profile.ver_res != 480 ||
            cfg->profile.buffer_height == 0 || cfg->profile.buffer_height > 480 ||
            cfg->tear_avoid_mode != ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE ||
            lv_display_get_color_format(disp) != LV_COLOR_FORMAT_RGB565) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_corner.display != NULL &&
            (s_corner.display != disp || s_corner.panel != cfg->panel)) return ESP_ERR_INVALID_STATE;
    const size_t scratch_bytes = (size_t)480 * cfg->profile.buffer_height * 2;
    if (s_corner.installed && s_corner.scratch_bytes != scratch_bytes) return ESP_ERR_INVALID_STATE;
    s_corner.display = disp;
    s_corner.panel = cfg->panel;
    s_corner.scratch_bytes = scratch_bytes;
    return hosino_corner_clip_init(disp);
}

esp_err_t hosino_corner_clip_init(lv_display_t *disp)
{
    if (disp == NULL || s_corner.display != disp || s_corner.panel == NULL) return ESP_ERR_INVALID_STATE;
    if (s_corner.installed) return ESP_OK;
    if (s_corner.scratch == NULL) {
        s_corner.scratch = heap_caps_malloc(s_corner.scratch_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_corner.scratch == NULL) return ESP_ERR_NO_MEM;
    }
    const esp_lv_adapter_draw_bitmap_callbacks_t cbs = {.custom_draw_bitmap = corner_draw};
    esp_err_t err = esp_lv_adapter_set_draw_bitmap_callbacks(disp, &cbs, &s_corner);
    if (err != ESP_OK) {
        free(s_corner.scratch);
        s_corner.scratch = NULL;
        return err;
    }
    s_corner.installed = true;
    ESP_LOGI(TAG, "Corner output mask ready, scratch=%u bytes; disabled until calibrated",
             (unsigned)s_corner.scratch_bytes);
    return ESP_OK;
}

static void corner_invalidate(void)
{
    if (s_corner.display == NULL) return;
    lv_obj_invalidate(lv_display_get_screen_active(s_corner.display));
    lv_obj_invalidate(lv_display_get_layer_top(s_corner.display));
    lv_obj_invalidate(lv_display_get_layer_sys(s_corner.display));
}

void hosino_corner_clip_set_radius(uint16_t radius)
{
    if (radius > 240) radius = 240;
    if (s_corner.radius == radius) return;
    s_corner.radius = radius;
    if (radius == 0) s_corner.enabled = false;
    corner_invalidate();
}

uint16_t hosino_corner_clip_get_radius(void) { return s_corner.radius; }

void hosino_corner_clip_set_enabled(bool enabled)
{
    static bool warned_unavailable;
    if (enabled && !s_corner.installed && !warned_unavailable) {
        ESP_LOGW(TAG, "Cannot enable corner clipping: callback is not installed");
        warned_unavailable = true;
    }
    enabled = enabled && s_corner.installed && s_corner.radius > 0;
    if (s_corner.enabled == enabled) return;
    s_corner.enabled = enabled;
    corner_invalidate();
}

bool hosino_corner_clip_is_enabled(void)
{
    return s_corner.enabled && s_corner.installed && s_corner.radius > 0;
}

void hosino_corner_clip_get_stats(hosino_corner_clip_stats_t *stats)
{
    if (stats != NULL) *stats = s_corner.stats;
}

void hosino_corner_clip_reset_stats(void) { memset(&s_corner.stats, 0, sizeof(s_corner.stats)); }
