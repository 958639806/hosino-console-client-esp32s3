/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lvgl.h"
#include "hosino_ui_fonts.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:Drawpanel"
#include "esp_lib_utils.h"
#include "Drawpanel.hpp"
#include "esp_heap_caps.h"

#define APP_NAME "画板"

using namespace std;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

LV_IMG_DECLARE(img_app_drawpanel);

namespace esp_brookesia::apps {

static constexpr int DRAW_DOT_SIZE = 10;

// HOST_DRAW_CANVAS_BEGIN
static void breakDrawStroke(DrawpanelStroke *stroke)
{
    stroke->active = false;
}

static void clearDrawCanvas(DrawpanelStroke *stroke)
{
    breakDrawStroke(stroke);
    if (stroke->canvas && lv_obj_is_valid(stroke->canvas))
        lv_canvas_fill_bg(stroke->canvas, lv_color_white(), LV_OPA_COVER);
}

static void drawCanvasPoint(DrawpanelStroke *stroke, const lv_point_t *point)
{
    if (!stroke->canvas || !lv_obj_is_valid(stroke->canvas)) {
        breakDrawStroke(stroke);
        return;
    }
    const int32_t width = lv_obj_get_width(stroke->canvas);
    const int32_t height = lv_obj_get_height(stroke->canvas);
    if (point->x < 0 || point->x >= width || point->y < 0 || point->y >= height) {
        breakDrawStroke(stroke);
        return;
    }
    if (stroke->active && point->x == stroke->previous.x && point->y == stroke->previous.y)
        return;

    lv_layer_t layer;
    lv_canvas_init_layer(stroke->canvas, &layer);
    if (stroke->active) {
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = lv_color_make(255, 0, 0);
        line.width = DRAW_DOT_SIZE;
        line.opa = LV_OPA_COVER;
        line.round_start = line.round_end = 1;
        line.p1.x = stroke->previous.x; line.p1.y = stroke->previous.y;
        line.p2.x = point->x; line.p2.y = point->y;
        lv_draw_line(&layer, &line);
    } else {
        /* LVGL intentionally ignores a zero-length line; use a rounded rect
         * so a single tap leaves a dot as well as the start of a drag. */
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color = lv_color_make(255, 0, 0);
        dot.bg_opa = LV_OPA_COVER;
        dot.radius = LV_RADIUS_CIRCLE;
        lv_area_t area = {point->x - DRAW_DOT_SIZE / 2, point->y - DRAW_DOT_SIZE / 2,
                         point->x + (DRAW_DOT_SIZE - 1) / 2, point->y + (DRAW_DOT_SIZE - 1) / 2};
        lv_draw_rect(&layer, &dot, &area);
    }
    /* The layer is synchronous: no drawing task retains a stack descriptor or
     * the canvas after this returns. Canvas bounds clip wide strokes. */
    lv_canvas_finish_layer(stroke->canvas, &layer);
    stroke->previous = *point;
    stroke->active = true;
}

static void handleDrawEvent(DrawpanelStroke *stroke, lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST ||
            code == LV_EVENT_INDEV_RESET || code == LV_EVENT_DELETE) {
        breakDrawStroke(stroke);
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    if (code == LV_EVENT_PRESSED) breakDrawStroke(stroke);
    lv_indev_t *input = lv_indev_active();
    if (!input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER || !stroke->canvas) {
        breakDrawStroke(stroke);
        return;
    }
    lv_point_t point;
    lv_indev_get_point(input, &point);
    lv_area_t area;
    lv_obj_get_coords(stroke->canvas, &area);
    point.x -= area.x1;
    point.y -= area.y1;
    drawCanvasPoint(stroke, &point);
}
// HOST_DRAW_CANVAS_END

static void configureFixedDrawObject(lv_obj_t *obj, bool clickable)
{
    if (!obj) {
        return;
    }

    lv_obj_clear_flag(
        obj,
        LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM |
        LV_OBJ_FLAG_SCROLL_ONE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_SCROLL_ON_FOCUS |
        LV_OBJ_FLAG_SCROLL_WITH_ARROW
    );
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);

    if (clickable) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    }
}

Drawpanel *Drawpanel::_instance = nullptr;

Drawpanel *Drawpanel::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new Drawpanel(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

Drawpanel::Drawpanel(bool use_status_bar, bool use_navigation_bar) :
    App(APP_NAME, &img_app_drawpanel, true, use_status_bar, use_navigation_bar)
{
}

Drawpanel::~Drawpanel()
{
    close();
}

bool Drawpanel::run(void)
{
    ESP_UTILS_LOGD("Run");
    close();

    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    // The app screen already matches Brookesia's visual area. Using it directly
    // prevents children near an edge from expanding a separate scrollable panel.
    _panel_obj = screen;
    configureFixedDrawObject(_panel_obj, true);
    lv_obj_update_layout(_panel_obj);
    const int32_t width = lv_obj_get_width(_panel_obj);
    const int32_t height = lv_obj_get_height(_panel_obj);
    if (width <= 0 || width > 480 || height <= 0 || height > 480) {
        _panel_obj = nullptr;
        return false;
    }
    const size_t bytes = (size_t)lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565) * height;
    const size_t alignment = LV_DRAW_BUF_ALIGN < sizeof(void *) ? sizeof(void *) : LV_DRAW_BUF_ALIGN;
    _pixels = heap_caps_aligned_alloc(alignment, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_pixels) {
        ESP_UTILS_LOGE("Draw canvas PSRAM allocation failed");
        _panel_obj = nullptr;
        return false;
    }
    _stroke.canvas = lv_canvas_create(_panel_obj);
    if (!_stroke.canvas) {
        close();
        return false;
    }
    configureFixedDrawObject(_stroke.canvas, false);
    lv_obj_set_pos(_stroke.canvas, 0, 0);
    lv_canvas_set_buffer(_stroke.canvas, _pixels, width, height, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(_stroke.canvas, width, height);
    clearAllPoints();
    lv_obj_remove_event_cb_with_user_data(_panel_obj, touch_event_cb, this);
    lv_obj_add_event_cb(_panel_obj, touch_event_cb, LV_EVENT_ALL, this);

    // Local controls stay inside the app's visual area; the system edge gestures remain free.
    lv_obj_t *clear_button = _clear_button = lv_button_create(screen);
    lv_obj_set_size(clear_button, 76, 40);
    lv_obj_align(clear_button, LV_ALIGN_TOP_RIGHT, -40, 12);
    lv_obj_add_event_cb(clear_button, [](lv_event_t *event) {
        auto *app = static_cast<Drawpanel *>(lv_event_get_user_data(event));
        if (app != nullptr) {
            app->clearAllPoints();
        }
    }, LV_EVENT_CLICKED, this);
    lv_obj_t *clear_label = lv_label_create(clear_button);
    lv_label_set_text(clear_label, "清空");
    lv_obj_set_style_text_font(clear_label, &hosino_font_18, LV_PART_MAIN);
    lv_obj_center(clear_label);

    return true;
}

bool Drawpanel::back(void)
{
    ESP_UTILS_LOGD("Back");

    // If the app needs to exit, call notifyCoreClosed() to notify the core to close the app
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");

    return true;
}

bool Drawpanel::close(void)
{
    ESP_UTILS_LOGD("Close");
    breakDrawStroke(&_stroke);
    if (_panel_obj && lv_obj_is_valid(_panel_obj)) {
        lv_obj_remove_event_cb_with_user_data(_panel_obj, touch_event_cb, this);
    }
    if (_clear_button && lv_obj_is_valid(_clear_button)) lv_obj_delete(_clear_button);
    _clear_button = nullptr;
    if (_stroke.canvas && lv_obj_is_valid(_stroke.canvas)) lv_obj_delete(_stroke.canvas);
    _stroke.canvas = nullptr;
    heap_caps_free(_pixels);
    _pixels = nullptr;
    _panel_obj = nullptr;

    return true;
}

bool Drawpanel::init()
{
    ESP_UTILS_LOGD("Init");

    return true;
}

bool Drawpanel::deinit()
{
    ESP_UTILS_LOGD("Deinit");

    return close();
}

bool Drawpanel::pause()
{
    ESP_UTILS_LOGD("Pause");
    breakDrawStroke(&_stroke);
    return true;
}

bool Drawpanel::resume()
{
    ESP_UTILS_LOGD("Resume");
    breakDrawStroke(&_stroke);
    return true;
}

void Drawpanel::clearAllPoints()
{
    clearDrawCanvas(&_stroke);
}

void Drawpanel::touch_event_cb(lv_event_t *e)
{
    Drawpanel *app = (Drawpanel *)lv_event_get_user_data(e);
    if (!app || !app->_panel_obj || lv_event_get_target(e) != app->_panel_obj) return;
    handleDrawEvent(&app->_stroke, e);
}

} // namespace esp_brookesia::apps
