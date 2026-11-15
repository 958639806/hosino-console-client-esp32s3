/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 * Modified for the 2.16-inch Chinese desktop; see SOURCE.md.
 */

#include "VideoPlayer.hpp"

#include <dirent.h>
#include <errno.h>
#include <algorithm>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "hosino_ui_fonts.h"
#include "hosino_chat_fonts.h"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:VideoPlayer"
#include "esp_lib_utils.h"

LV_IMG_DECLARE(img_app_vedioplayer);

namespace {

constexpr uint32_t CHROME_VISIBLE_MS = 3500;

// A pinch is an app-content shortcut, never an edge/navigation gesture.
bool video_pinch_inside(int32_t x, int32_t y, int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    constexpr int32_t margin = 24;
    return right - left >= margin * 2 && bottom - top >= margin * 2 &&
           x >= left + margin && x <= right - margin && y >= top + margin && y <= bottom - margin;
}

int video_pinch_mode(float scale)
{
    if (!isfinite(scale) || scale <= 0.0f) return -1;
    if (scale > 1.15f) return HOSINO_VIDEO_FILL;
    if (scale < 0.85f) return HOSINO_VIDEO_FIT;
    return -1;
}

bool is_avi_file(const char *name)
{
    const char *extension = name ? strrchr(name, '.') : nullptr;
    return extension && strcasecmp(extension, ".avi") == 0;
}

portMUX_TYPE browser_lock = portMUX_INITIALIZER_UNLOCKED;

bool safe_sd_path(const char *path)
{
    if (!path || strnlen(path, HOSINO_MEDIA_PATH_MAX) >= HOSINO_MEDIA_PATH_MAX) return false;
    constexpr size_t root_size = sizeof(STORAGE_SERVICE_MOUNT_POINT) - 1;
    if (strncmp(path, STORAGE_SERVICE_MOUNT_POINT, root_size) || (path[root_size] && path[root_size] != '/')) return false;
    if (strchr(path, '\\') || strstr(path, "//")) return false;
    for (const char *segment = path + root_size; *segment;) {
        if (*segment == '/') ++segment;
        const char *end = strchr(segment, '/');
        const size_t length = end ? static_cast<size_t>(end - segment) : strlen(segment);
        if (!length || (length == 1 && segment[0] == '.') || (length == 2 && segment[0] == '.' && segment[1] == '.')) return false;
        segment += length;
    }
    return true;
}

bool join_sd_path(char *out, const char *directory, const char *name)
{
    if (!name || !*name || strchr(name, '/') || strchr(name, '\\') || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    const int size = snprintf(out, HOSINO_MEDIA_PATH_MAX, "%s/%s", directory, name);
    return size > 0 && size < HOSINO_MEDIA_PATH_MAX && safe_sd_path(out);
}

void time_text(char *out, size_t size, uint64_t ms, bool known)
{
    if (!known) { snprintf(out, size, "--:--"); return; }
    const unsigned long long seconds = ms / 1000;
    if (seconds >= 3600) snprintf(out, size, "%llu:%02llu:%02llu", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else snprintf(out, size, "%02llu:%02llu", seconds / 60, seconds % 60);
}

} // namespace

namespace esp_brookesia::apps {

VideoPlayer *VideoPlayer::_instance = nullptr;

VideoPlayer *VideoPlayer::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (!_instance) {
        _instance = new VideoPlayer(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

VideoPlayer::VideoPlayer(bool use_status_bar, bool use_navigation_bar)
    : App("视频播放", &img_app_vedioplayer, true, use_status_bar, use_navigation_bar)
{
}

VideoPlayer::~VideoPlayer()
{
    _ui_accept_frames.store(false);
    // LVGL may redraw the canvas until the screen is actually cleared by the
    // core.  Point it at permanent storage before freeing the PSRAM frames.
    detachCanvas();
    while (!stopWorker(portMAX_DELAY)) {
        // A failed vendor shutdown may still retain video_ctx=this. Never let
        // destruction invalidate it; ordinary app close does not delete us.
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    releaseFrameBuffers();
    heap_caps_free(_browser);
    _browser = nullptr;
    destroyUi();
    _instance = nullptr;
}

bool VideoPlayer::init()
{
    if (!_worker_stopped.load() || _worker_task.load() || !_cleanup_complete.load()) {
        return false;
    }
    _state.store(State::Idle);
    _last_error.store(ESP_OK);
    _command.store(Command::None);
    return true;
}

bool VideoPlayer::run()
{
    _foreground = true;
    _ui_accept_frames.store(false);
    detachCanvas();
    destroyUi();
    if (_worker_stopped.load() && _cleanup_complete.load()) {
        // A naturally completed/failed playback keeps its last frame alive until
        // the UI is detached.  A fresh run no longer references that frame.
        releaseFrameBuffers();
    }
    if (!_browser) {
        _browser = static_cast<BrowserStore *>(heap_caps_calloc(1, sizeof(BrowserStore), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!_browser) return false;
        strlcpy(_browser->directory, STORAGE_SERVICE_MOUNT_POINT, sizeof(_browser->directory));
    }
    _browser_visible.store(true);
    _last_browser_revision = UINT32_MAX;
    _screen = lv_screen_active();
    lv_obj_set_style_bg_color(_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_event_cb_with_user_data(_screen, chromeEvent, this);
    lv_obj_add_event_cb(_screen, chromeEvent, LV_EVENT_CLICKED, this);

    // Keep the system status bar and the bottom 68 px HUD/gesture region free.
    const int width = lv_area_get_width(&getVisualArea());
    const int height = lv_area_get_height(&getVisualArea()) - 68;
    if (width <= 0 || height <= 0) return false;
    _viewport_width.store(width);
    _viewport_height.store(height);
    _viewport = lv_obj_create(_screen);
    lv_obj_remove_style_all(_viewport);
    lv_obj_set_pos(_viewport, 0, 0);
    lv_obj_set_size(_viewport, width, height);
    lv_obj_remove_flag(_viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(_viewport, LV_OBJ_FLAG_OVERFLOW_VISIBLE); // Fill crops only inside the app viewport.
    lv_obj_add_event_cb(_viewport, chromeEvent, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_viewport, pinchEvent, LV_EVENT_ALL, this);

    // Keep the reference app's simple, video-first composition. The canvas is
    // created before every overlay so a 320x200/320x240 clip remains the visual
    // focus within the target board's app visual area.
    _canvas = lv_canvas_create(_viewport);
    lv_canvas_set_buffer(_canvas, &_placeholder_pixel, 1, 1, LV_COLOR_FORMAT_RGB565);
    lv_image_set_antialias(_canvas, false); // Nearest-neighbour scaling avoids interpolation work on this CPU.
    lv_obj_set_size(_canvas, 1, 1);
    lv_obj_align(_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_canvas, chromeEvent, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_canvas, pinchEvent, LV_EVENT_ALL, this);

    _hero_icon = lv_image_create(_viewport);
    lv_image_set_src(_hero_icon, &img_app_vedioplayer);
    lv_obj_align(_hero_icon, LV_ALIGN_CENTER, 0, -42);

    _status_label = lv_label_create(_viewport);
    lv_label_set_text(_status_label, "正在读取存储卡");
    lv_obj_set_width(_status_label, std::min(360, width - 24));
    lv_obj_set_style_text_align(_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(_status_label, &hosino_font_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(_status_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_status_label, lv_color_hex(0x111118), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_status_label, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(_status_label, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(_status_label, 9, LV_PART_MAIN);
    lv_obj_set_style_radius(_status_label, 18, LV_PART_MAIN);
    lv_obj_align(_status_label, LV_ALIGN_CENTER, 0, 48);

    _header = lv_obj_create(_viewport);
    lv_obj_remove_style_all(_header);
    // Blank chrome must pass through to the video. Its child buttons retain
    // their own hit targets, including while a second pinch starts after fill.
    lv_obj_remove_flag(_header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(_header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(_header, std::min(440, width - 32), 42);
    lv_obj_set_style_bg_color(_header, lv_color_hex(0x101016), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_header, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(_header, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(_header, lv_color_hex(0x393444), LV_PART_MAIN);
    lv_obj_set_style_border_opa(_header, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(_header, 21, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(_header, 18, LV_PART_MAIN);
    lv_obj_set_flex_flow(_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        _header,
        LV_FLEX_ALIGN_SPACE_BETWEEN,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );
    lv_obj_align(_header, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t *title = lv_label_create(_header);
    lv_label_set_text(title, "视频播放");
    lv_obj_set_style_text_font(title, &hosino_font_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xC695FF), LV_PART_MAIN);

    _counter_label = lv_label_create(_header);
    lv_label_set_text(_counter_label, "0 / 0");
    lv_obj_set_style_text_font(_counter_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(_counter_label, lv_color_white(), LV_PART_MAIN);

    _files_button = lv_button_create(_header);
    lv_obj_set_size(_files_button, 62, 30);
    lv_obj_set_user_data(_files_button, reinterpret_cast<void *>(static_cast<uintptr_t>(Command::Files)));
    lv_obj_add_event_cb(_files_button, controlEvent, LV_EVENT_CLICKED, this);
    lv_obj_t *files_label = lv_label_create(_files_button);
    lv_label_set_text(files_label, "列表");
    lv_obj_set_style_text_font(files_label, &hosino_font_14, 0);
    lv_obj_center(files_label);

    _controls = lv_obj_create(_viewport);
    lv_obj_remove_style_all(_controls);
    lv_obj_set_size(_controls, 350, 154);
    lv_obj_remove_flag(_controls, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(_controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(_controls, lv_color_hex(0x101016), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_controls, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(_controls, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(_controls, lv_color_hex(0x393444), LV_PART_MAIN);
    lv_obj_set_style_border_opa(_controls, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(_controls, 34, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(_controls, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_top(_controls, 44, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(_controls, 51, LV_PART_MAIN);
    lv_obj_set_flex_flow(_controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        _controls,
        LV_FLEX_ALIGN_SPACE_EVENLY,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );
    lv_obj_align(_controls, LV_ALIGN_BOTTOM_MID, 0, -8);
    _progress = lv_slider_create(_controls);
    lv_obj_add_flag(_progress, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(_progress, 294, 9);
    lv_obj_set_pos(_progress, 16, -34);
    lv_slider_set_range(_progress, 0, 1000);
    lv_obj_add_event_cb(_progress, seekEvent, LV_EVENT_ALL, this);
    _time_label = lv_label_create(_controls);
    lv_obj_add_flag(_time_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_text_font(_time_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_time_label, lv_color_white(), 0);
    lv_label_set_text(_time_label, "--:-- / --:--");
    lv_obj_set_pos(_time_label, 82, -19);

    auto add_control = [this](const char *text, Command command, bool primary) -> lv_obj_t * {
        lv_obj_t *button = lv_button_create(_controls);
        lv_obj_set_size(button, primary ? 56 : 46, primary ? 56 : 46);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(
            button,
            primary ? lv_color_hex(0x8E42FF) : lv_color_hex(0x2B2932),
            LV_PART_MAIN
        );
        lv_obj_set_style_bg_opa(button, primary ? LV_OPA_COVER : LV_OPA_80, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(button, primary ? 18 : 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(button, lv_color_hex(0x8E42FF), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(button, primary ? LV_OPA_30 : LV_OPA_TRANSP, LV_PART_MAIN);
        const lv_style_selector_t disabled_selector =
            static_cast<lv_style_selector_t>(LV_PART_MAIN) |
            static_cast<lv_style_selector_t>(LV_STATE_DISABLED);
        lv_obj_set_style_opa(button, LV_OPA_30, disabled_selector);
        lv_obj_set_user_data(button, reinterpret_cast<void *>(static_cast<uintptr_t>(command)));
        lv_obj_add_event_cb(button, controlEvent, LV_EVENT_CLICKED, this);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, primary ? &lv_font_montserrat_24 : &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(label);
        if (primary) {
            _play_button_label = label;
        }
        return button;
    };
    _previous_button = add_control(LV_SYMBOL_PREV, Command::Previous, false);
    _backward_button = add_control("-10", Command::Backward, false);
    _play_button = add_control(LV_SYMBOL_PAUSE, Command::Pause, true);
    _forward_button = add_control("+10", Command::Forward, false);
    _next_button = add_control(LV_SYMBOL_NEXT, Command::Next, false);

    // Four small widgets reuse the existing control overlay; no new frame/layer.
    _volume_label = lv_label_create(_controls);
    lv_obj_add_flag(_volume_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(_volume_label, 0, 75);
    lv_obj_set_style_text_font(_volume_label, &hosino_chat_font_16, 0);
    lv_obj_set_style_text_color(_volume_label, lv_color_white(), 0);
    lv_label_set_text(_volume_label, "音量 --");
    _volume_slider = lv_slider_create(_controls);
    lv_obj_add_flag(_volume_slider, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(_volume_slider, 83, 82);
    lv_obj_set_size(_volume_slider, 102, 8);
    lv_obj_set_ext_click_area(_volume_slider, 12);
    lv_slider_set_range(_volume_slider, 0, 100);
    lv_slider_set_value(_volume_slider, _volume_applied.load(), LV_ANIM_OFF);
    lv_obj_add_event_cb(_volume_slider, volumeEvent, LV_EVENT_ALL, this);
    _scale_button = lv_button_create(_controls);
    lv_obj_add_flag(_scale_button, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(_scale_button, 208, 65);
    lv_obj_set_size(_scale_button, 112, 34);
    lv_obj_set_style_pad_all(_scale_button, 3, 0);
    lv_obj_set_user_data(_scale_button, reinterpret_cast<void *>(static_cast<uintptr_t>(Command::Scale)));
    lv_obj_add_event_cb(_scale_button, controlEvent, LV_EVENT_CLICKED, this);
    _scale_label = lv_label_create(_scale_button);
    lv_obj_set_style_text_font(_scale_label, &hosino_chat_font_16, 0);
    lv_label_set_text(_scale_label, _scale_mode == HOSINO_VIDEO_FIT ? "画面：适应" :
                     _scale_mode == HOSINO_VIDEO_FILL ? "画面：填充" : "画面：原尺寸");
    lv_obj_center(_scale_label);

    createBrowser();
    _fps_since_ms = 0;
    _fps_frames = 0;
    _video_fps = 0;
    _ui_timer = lv_timer_create(uiTimer, 16, this);
    _last_ui_state = State::Idle;
    _frame_presented = false;
    _chrome_visible = true;
    _chrome_hide_at_ms = 0;
    updateControlState(State::Mounting, 0);
    // An old worker may still be finishing a bounded SD/codec operation. Let
    // the GUI timer start the next session only after ownership is released.
    _restart_pending = true;
    if (_ui_timer) updateUi();
    return _ui_timer != nullptr;
}

bool VideoPlayer::back()
{
    if (!_browser_visible.load()) { showBrowser(); return true; }
    return notifyCoreClosed();
}

bool VideoPlayer::close()
{
    _foreground = false;
    _restart_pending = false;
    requestStop();
    destroyUi();
    return true;
}

bool VideoPlayer::deinit()
{
    return close();
}

bool VideoPlayer::pause()
{
    _foreground = false;
    _restart_pending = false;
    requestStop();
    if (_ui_timer) {
        lv_timer_pause(_ui_timer);
    }
    return true;
}

bool VideoPlayer::resume()
{
    _foreground = true;
    _restart_pending = true;
    if (_ui_timer) {
        lv_timer_resume(_ui_timer);
    }
    updateUi();
    return true;
}

bool VideoPlayer::cleanResource()
{
    _foreground = false;
    _restart_pending = false;
    requestStop();
    destroyUi();
    return true;
}

void VideoPlayer::requestStop()
{
    resetPinch();
    _volume_requested.store(-1);
    _audio_enabled.store(false);
    _ui_accept_frames.store(false);
    detachCanvas();
    _stop_requested.store(true);
    _browser_visible.store(true);
    cancelPendingSeek();
    _state.store(State::Stopping);
    // Never join an SD/JPEG/audio worker from the GUI thread. The singleton
    // app outlives its screens and worker callbacks, as in the official app.
    if (_worker_stopped.load() && _cleanup_complete.load()) releaseFrameBuffers();
}

void VideoPlayer::detachCanvas()
{
    if (_canvas) {
        lv_image_set_scale(_canvas, 256);
        lv_image_set_pivot(_canvas, 0, 0);
        lv_canvas_set_buffer(_canvas, &_placeholder_pixel, 1, 1, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(_canvas, 1, 1);
        lv_obj_invalidate(_canvas);
    }
    _display_buffer_index = -1;
    _pending_buffer_index = -1;
    _canvas_detached.store(true);
}

void VideoPlayer::destroyUi()
{
    if (_ui_timer) {
        // Created inside run(): Brookesia owns deletion of this recorded timer.
        lv_timer_pause(_ui_timer);
        _ui_timer = nullptr;
    }
    if (_screen) {
        lv_obj_remove_event_cb_with_user_data(_screen, chromeEvent, this);
    }
    if (_canvas) {
        lv_obj_remove_event_cb_with_user_data(_canvas, chromeEvent, this);
        lv_obj_remove_event_cb_with_user_data(_canvas, pinchEvent, this);
    }
    if (_viewport) lv_obj_remove_event_cb_with_user_data(_viewport, pinchEvent, this);
    resetPinch();
    _browser_root = _directory_label = _browser_status = nullptr;
    _parent_button = _refresh_button = _page_previous = _page_next = _page_label = nullptr;
    _decoder_button = _decoder_label = nullptr;
    for (unsigned i = 0; i < PAGE_SIZE; ++i) { _entry_buttons[i] = nullptr; _entry_labels[i] = nullptr; }
    _files_button = _backward_button = _forward_button = _progress = _time_label = nullptr;
    _volume_slider = _volume_label = _scale_button = _scale_label = nullptr;
    _volume_dragging = false;
    _seek_dragging = false;
    _screen = nullptr;
    _viewport = nullptr;
    _canvas = nullptr;
    _header = nullptr;
    _controls = nullptr;
    _hero_icon = nullptr;
    _status_label = nullptr;
    _counter_label = nullptr;
    _previous_button = nullptr;
    _play_button = nullptr;
    _next_button = nullptr;
    _play_button_label = nullptr;
    _frame_presented = false;
    _chrome_visible = true;
    _chrome_hide_at_ms = 0;
}

void VideoPlayer::setChromeVisible(bool visible)
{
    auto set_visible = [visible](lv_obj_t *object) {
        if (!object) {
            return;
        }
        if (visible) {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    };
    set_visible(_header);
    set_visible(_controls);
    _chrome_visible = visible;
}

void VideoPlayer::showChrome()
{
    setChromeVisible(true);
    _chrome_hide_at_ms = lv_tick_get() + CHROME_VISIBLE_MS;
}

void VideoPlayer::applyViewScale()
{
    const int index = _display_buffer_index.load();
    if (!_canvas || _canvas_detached.load() || index < 0 || index > 1) return;
    const uint16_t width = _frame_width[index], height = _frame_height[index];
    // The producer has already resized/cropped this opaque RGB565 frame.
    // Keeping LVGL at 1:1 avoids generic transform -> RGB565A8 -> masked blend.
    lv_image_set_pivot(_canvas, width / 2, height / 2);
    lv_image_set_scale(_canvas, 256);
    lv_obj_align(_canvas, LV_ALIGN_CENTER, 0, 0);
}

void VideoPlayer::resetPinch()
{
    _pinch_armed = _pinch_applied = _pinch_seen = false;
}

void VideoPlayer::updateControlState(State state, int file_count)
{
    const bool enabled = file_count > 0 &&
        (state == State::Playing || state == State::Paused);
    lv_obj_t *buttons[] = {_previous_button, _play_button, _next_button};
    for (lv_obj_t *button : buttons) {
        if (!button) {
            continue;
        }
        if (enabled) {
            lv_obj_remove_state(button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(button, LV_STATE_DISABLED);
        }
    }
}

bool VideoPlayer::startWorker()
{
    if (_worker_task.load() || !_worker_stopped.load()) {
        return false;
    }

    // Cleanup of a retained AVI/audio session belongs to the worker as well.
    // Do not clear its ownership flags or wait for codec I/O in this GUI call.
    _stop_requested.store(false);
    _command.store(Command::None);
    cancelPendingSeek();
    _decoded_frames.store(0);
    _dropped_frames.store(0);
    _fps_since_ms = 0;
    _fps_frames = 0;
    _video_fps = 0;
    _state.store(State::Mounting);
    _worker_stopped.store(false);

    TaskHandle_t task = nullptr;
    BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        workerTask,
        "esp_video_control",
        8 * 1024,
        this,
        4,
        &task,
        PRO_CPU_NUM,
        MALLOC_CAP_SPIRAM
    );
    if (result != pdPASS) {
        _worker_stopped.store(true);
        _state.store(State::Error);
        _last_error.store(ESP_ERR_NO_MEM);
        return false;
    }
    _worker_task.store(task);
    // Publish the task handle before allowing the worker to run to completion
    // on the other core; otherwise a fast mount failure can leave a stale
    // handle after the worker has already cleared it.
    xTaskNotifyGive(task);
    return true;
}

bool VideoPlayer::stopWorker(TickType_t timeout)
{
    _stop_requested.store(true);
    _state.store(State::Stopping);
    TaskHandle_t task = _worker_task.load();
    if (task == xTaskGetCurrentTaskHandle()) {
        return false;
    }
    const TickType_t started = xTaskGetTickCount();
    while (!_worker_stopped.load()) {
        if (timeout != portMAX_DELAY && xTaskGetTickCount() - started >= timeout) {
            ESP_UTILS_LOGE("Timed out waiting for AVI worker");
            return false;
        }
        vTaskDelay(1);
    }
    if (!_cleanup_complete.load()) {
        return cleanupPlaybackResources();
    }
    return true;
}

bool VideoPlayer::postCommand(Command command, const char *path, uint64_t seek_ms)
{
    if (!_foreground || !_browser || !_worker_task.load() || _stop_requested.load()) return false;
    if (path && !safe_sd_path(path)) return false;
    const bool playback_command = command == Command::Play || command == Command::Pause ||
        command == Command::Previous || command == Command::Next || command == Command::Seek ||
        command == Command::Forward || command == Command::Backward;
    if (playback_command && _browser_visible.load()) return false;
    portENTER_CRITICAL(&browser_lock);
    // Returning to files is a stop barrier. Stale control events must not
    // overwrite it and leave audio running behind a disabled browser page.
    const Command queued = _command.load();
    const bool queued_file_change = queued == Command::Previous || queued == Command::Next || queued == Command::OpenEntry;
    const bool ordinary_control = command == Command::Play || command == Command::Pause || command == Command::Seek ||
        command == Command::Forward || command == Command::Backward;
    if ((queued == Command::Files && command != Command::Files) || (queued_file_change && ordinary_control)) {
        portEXIT_CRITICAL(&browser_lock);
        return false;
    }
    const bool seek_command = command == Command::Seek || command == Command::Forward || command == Command::Backward;
    if (seek_command) {
        if (!_duration_ms) {
            portEXIT_CRITICAL(&browser_lock);
            return false;
        }
        // A backend seek returns before SEEK_DONE. Relative controls must build
        // on the last user target, not a transient/reset renderer timestamp.
        const uint64_t base = std::min(_seek_intent_active ? _seek_target_ms : _position_ms, _duration_ms);
        if (command == Command::Forward) seek_ms = _duration_ms - base <= 10000 ? _duration_ms : base + 10000;
        else if (command == Command::Backward) seek_ms = base > 10000 ? base - 10000 : 0;
        _seek_target_ms = std::min(seek_ms, _duration_ms);
        _seek_intent_active = true;
        ++_seek_revision;
        command = Command::Seek;
        seek_ms = _seek_target_ms;
    } else if (command == Command::Files || command == Command::Previous || command == Command::Next ||
               command == Command::OpenEntry || command == Command::Parent || command == Command::Refresh ||
               command == Command::PagePrevious || command == Command::PageNext) {
        _seek_intent_active = false;
        _seek_issued_revision = _seek_revision;
    }
    if (path) strlcpy(_browser->command_path, path, sizeof(_browser->command_path));
    else _browser->command_path[0] = 0;
    _browser->command_seek_ms = seek_ms;
    _command.store(command);
    portEXIT_CRITICAL(&browser_lock);
    return true;
}

VideoPlayer::Command VideoPlayer::takeCommand(char *path, uint64_t *seek_ms)
{
    portENTER_CRITICAL(&browser_lock);
    Command command = _command.exchange(Command::None);
    strlcpy(path, _browser->command_path, HOSINO_MEDIA_PATH_MAX);
    *seek_ms = _browser->command_seek_ms;
    portEXIT_CRITICAL(&browser_lock);
    return command;
}

void VideoPlayer::cancelPendingSeek()
{
    portENTER_CRITICAL(&browser_lock);
    _seek_intent_active = false;
    _seek_issued_revision = _seek_revision;
    portEXIT_CRITICAL(&browser_lock);
}

esp_err_t VideoPlayer::dispatchPendingSeek(const hosino_media_player_snapshot_t &snapshot)
{
    if (_stop_requested.load() || _browser_visible.load()) {
        cancelPendingSeek();
        return ESP_OK;
    }
    if (snapshot.seeking) return ESP_OK;
    const bool active = snapshot.state == HOSINO_MEDIA_PLAYING || snapshot.state == HOSINO_MEDIA_PAUSED;
    if (!active || !snapshot.duration_ms) {
        if (snapshot.state == HOSINO_MEDIA_FINISHED || snapshot.state == HOSINO_MEDIA_ERROR) cancelPendingSeek();
        return ESP_OK;
    }
    uint64_t target = 0;
    uint32_t revision = 0;
    bool dispatch = false;
    portENTER_CRITICAL(&browser_lock);
    const Command queued = _command.load();
    const bool replacing_media = queued == Command::Files || queued == Command::Previous ||
        queued == Command::Next || queued == Command::OpenEntry;
    if (_seek_intent_active && !replacing_media) {
        if (_seek_issued_revision != _seek_revision) {
            target = std::min(_seek_target_ms, snapshot.duration_ms);
            revision = _seek_revision;
            dispatch = true;
        } else {
            // The accepted request has now completed, so future relative
            // controls can again use the real playback position.
            _seek_intent_active = false;
        }
    }
    portEXIT_CRITICAL(&browser_lock);
    if (!dispatch || _stop_requested.load() || _browser_visible.load()) return ESP_OK;
    esp_err_t result = hosino_media_player_seek(_media, target);
    if (result == ESP_OK) {
        portENTER_CRITICAL(&browser_lock);
        _seek_issued_revision = revision;
        portEXIT_CRITICAL(&browser_lock);
    } else if (result == ESP_ERR_NOT_SUPPORTED) {
        cancelPendingSeek();
        result = ESP_OK;
    } else if (result == ESP_ERR_INVALID_STATE) {
        // Keep only the latest intent until the next real snapshot says the
        // current seek/state transition has completed. Never queue a second.
        result = ESP_OK;
    }
    return result;
}

esp_err_t VideoPlayer::scanDirectory()
{
    _state.store(State::Scanning);
    auto &page = _browser->working;
    memset(&page, 0, sizeof(page));
    strlcpy(page.directory, _browser->directory, sizeof(page.directory));
    page.page = _browser->page;
    esp_err_t result = safe_sd_path(page.directory) ? storage_service_acquire(&_storage_lease) : ESP_ERR_INVALID_ARG;
    DIR *directory = result == ESP_OK ? opendir(page.directory) : nullptr;
    if (result == ESP_OK && !directory) result = ESP_FAIL;
    if (directory) {
        dirent *entry;
        const uint64_t begin = static_cast<uint64_t>(page.page) * PAGE_SIZE;
        unsigned examined = 0;
        while (!_stop_requested.load()) {
            errno = 0;
            entry = readdir(directory);
            if (!entry) { if (errno) result = ESP_FAIL; break; }
            if (!(++examined % 32)) vTaskDelay(1);
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char path[HOSINO_MEDIA_PATH_MAX];
            if (!join_sd_path(path, page.directory, entry->d_name)) continue;
            struct stat info = {};
            if (stat(path, &info)) continue;
            const bool is_dir = S_ISDIR(info.st_mode);
            if (!is_dir && (!S_ISREG(info.st_mode) || !is_avi_file(entry->d_name))) continue;
            if (page.total >= begin && page.count < PAGE_SIZE) {
                auto &item = page.entries[page.count++];
                strlcpy(item.name, entry->d_name, sizeof(item.name));
                item.directory = is_dir;
                item.video_index = page.videos;
            }
            if (!is_dir) ++page.videos;
            if (page.total == UINT32_MAX) { result = ESP_ERR_INVALID_SIZE; break; }
            ++page.total;
        }
        closedir(directory);
        if (_stop_requested.load()) result = ESP_ERR_INVALID_STATE;
    }
    storage_service_release(&_storage_lease);
    page.error = result;
    portENTER_CRITICAL(&browser_lock);
    _browser->published = page;
    _browser_revision.fetch_add(1);
    portEXIT_CRITICAL(&browser_lock);
    _file_count.store(page.videos);
    _last_error.store(result);
    _state.store(result == ESP_OK ? State::Browsing : State::Error);
    return result;
}

esp_err_t VideoPlayer::adjacentVideo(int direction)
{
    esp_err_t result = storage_service_acquire(&_storage_lease);
    if (result != ESP_OK) return result;
    DIR *directory = opendir(_browser->directory);
    if (!directory) { storage_service_release(&_storage_lease); return ESP_FAIL; }
    // Constant space irrespective of directory size; order matches readdir's
    // browser order. Paths are complete UTF-8, not clipped display labels.
    char first[HOSINO_MEDIA_PATH_MAX] = {}, previous[HOSINO_MEDIA_PATH_MAX] = {};
    char chosen[HOSINO_MEDIA_PATH_MAX] = {}, last[HOSINO_MEDIA_PATH_MAX] = {};
    unsigned count = 0, chosen_index = 0, examined = 0;
    bool found = false;
    dirent *entry;
    while (!_stop_requested.load()) {
        errno = 0;
        entry = readdir(directory);
        if (!entry) { if (errno) result = ESP_FAIL; break; }
        if (!(++examined % 32)) vTaskDelay(1);
        if (!is_avi_file(entry->d_name)) continue;
        char path[HOSINO_MEDIA_PATH_MAX];
        if (!join_sd_path(path, _browser->directory, entry->d_name)) continue;
        struct stat info = {};
        if (stat(path, &info) || !S_ISREG(info.st_mode)) continue;
        if (!count) strlcpy(first, path, sizeof(first));
        if (found && direction > 0 && !chosen[0]) { strlcpy(chosen, path, sizeof(chosen)); chosen_index = count; }
        if (!strcmp(path, _browser->selected)) {
            found = true;
            if (direction < 0 && previous[0]) { strlcpy(chosen, previous, sizeof(chosen)); chosen_index = count - 1; }
        }
        strlcpy(previous, path, sizeof(previous));
        strlcpy(last, path, sizeof(last));
        ++count;
    }
    closedir(directory);
    storage_service_release(&_storage_lease);
    if (result != ESP_OK) return result;
    if (_stop_requested.load() || _browser_visible.load()) return ESP_ERR_INVALID_STATE;
    if (!count) return ESP_ERR_NOT_FOUND;
    if (!chosen[0]) {
        strlcpy(chosen, direction < 0 ? last : first, sizeof(chosen));
        chosen_index = direction < 0 ? count - 1 : 0;
    }
    strlcpy(_browser->selected, chosen, sizeof(_browser->selected));
    _file_count.store(count); _current_index.store(chosen_index);
    return ESP_OK;
}

esp_err_t VideoPlayer::startSelected()
{
    if (!safe_sd_path(_browser->selected) || !is_avi_file(_browser->selected)) return ESP_ERR_INVALID_ARG;
    if (!cleanupPlaybackResources()) return _last_error.load();
    if (_stop_requested.load() || _browser_visible.load()) return ESP_ERR_INVALID_STATE;
    _cleanup_complete.store(false);
    _state.store(State::Preparing); _last_error.store(ESP_OK);
    portENTER_CRITICAL(&browser_lock); _position_ms = _duration_ms = 0; portEXIT_CRITICAL(&browser_lock);
    hosino_media_player_config_t config = {};
    config.owner = HOSINO_MEDIA_OWNER_VIDEO; config.enable_video = true;
    config.max_width = _viewport_width.load(); config.max_height = _viewport_height.load();
    config.render_fps = 30; config.video_cb = videoCallback; config.video_ctx = this;
    config.video_decoder = _decoder.load();
    esp_err_t result = hosino_media_player_create(&config, &_media);
    if (result != ESP_OK) return result;
    ESP_LOGI("VideoPlayer", "Opening selected clip: PSRAM free=%u largest=%u buffers=%u/%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(_frame_buffer_capacity[0]), static_cast<unsigned>(_frame_buffer_capacity[1]));
    result = hosino_media_player_open(_media, _browser->selected);
    if (result == ESP_OK) {
        _volume_requested.store(-1);
        _volume_applied.store(bsp_extra_codec_volume_get()); // Worker owns the initialized board audio session.
        _volume_error.store(ESP_OK);
        if (_stop_requested.load() || _browser_visible.load()) return ESP_ERR_INVALID_STATE;
        result = hosino_media_player_start(_media);
    }
    return result;
}

void VideoPlayer::releaseFrameBuffers()
{
    // Only after the producer stops and LVGL no longer references these pixels.
    for (auto &buffer : _frame_buffers) {
        heap_caps_free(buffer);
        buffer = nullptr;
    }
    _frame_buffer_capacity[0] = _frame_buffer_capacity[1] = 0;
    _display_buffer_index = -1;
    _pending_buffer_index = -1;
}

bool VideoPlayer::cleanupPlaybackResources()
{
    _audio_enabled.store(false);
    _volume_requested.store(-1);
    cancelPendingSeek();
    if (_media) {
        esp_err_t result = hosino_media_player_stop(_media);
        if (result == ESP_OK) result = hosino_media_player_destroy(_media);
        if (result != ESP_OK) {
            _last_error.store(result);
            _cleanup_complete.store(false);
            return false;
        }
        _media = nullptr;
    }
    // The render callback is stopped before releasing its small SRAM row cache.
    heap_caps_free(_scale_scratch);
    _scale_scratch = nullptr;
    storage_service_release(&_storage_lease);
    _playing.store(false);
    _cleanup_complete.store(true);
    return true;
}

esp_err_t VideoPlayer::videoCallback(const hosino_media_video_frame_t *frame, void *arg)
{
    auto *self = static_cast<VideoPlayer *>(arg);
    if (!self || !frame || !frame->data) return ESP_ERR_INVALID_ARG;
    if (!self->_ui_accept_frames.load() || self->_stop_requested.load()) return ESP_OK;
    const size_t source_bytes = static_cast<size_t>(frame->width) * frame->height * 2;
    if (!frame->width || !frame->height || frame->width > self->_viewport_width.load() ||
        frame->height > self->_viewport_height.load() || frame->stride_bytes != frame->width * 2U ||
        frame->bytes < source_bytes) return ESP_ERR_INVALID_SIZE;
    // The display buffer remains immutable until LVGL switches to the pending
    // buffer. There is never a write to LVGL or LCD from the render task.
    if (self->_pending_buffer_index.load() >= 0) {
        self->_dropped_frames.fetch_add(1);
        return ESP_OK;
    }
    const int target = self->_display_buffer_index.load() == 0 ? 1 : 0;
    const hosino_video_scale_mode_t mode = self->_scale_mode.load();
    hosino_video_rgb565_plan_t plan;
    if (!hosino_video_rgb565_plan(mode, frame->width, frame->height,
            self->_viewport_width.load(), self->_viewport_height.load(), &plan)) return ESP_ERR_INVALID_SIZE;
    if (mode != HOSINO_VIDEO_ORIGINAL && !self->_scale_scratch) {
        self->_scale_scratch = static_cast<hosino_video_rgb565_scratch_t *>(
            heap_caps_malloc(sizeof(hosino_video_rgb565_scratch_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!self->_scale_scratch) {
            // Preserve both frames and audio; an explicit retry is possible by
            // cycling the mode after memory becomes available. Never allocate
            // a full internal frame or silently use slow PSRAM row scratch.
            auto expected = mode;
            if (self->_scale_mode.compare_exchange_strong(expected, HOSINO_VIDEO_ORIGINAL))
                self->_scale_failed.store(true);
            self->_dropped_frames.fetch_add(1);
            ESP_LOGW("VideoPlayer", "Scale row cache unavailable: need=%u; reverting to original",
                     static_cast<unsigned>(sizeof(hosino_video_rgb565_scratch_t)));
            return ESP_OK;
        }
        ESP_LOGI("VideoPlayer", "Scale row cache: %u bytes internal SRAM",
                 static_cast<unsigned>(sizeof(hosino_video_rgb565_scratch_t)));
    }
    const size_t stride = (static_cast<size_t>(plan.width) * 2 + LV_DRAW_BUF_STRIDE_ALIGN - 1) /
        LV_DRAW_BUF_STRIDE_ALIGN * LV_DRAW_BUF_STRIDE_ALIGN;
    const size_t bytes = stride * plan.height;
    // Only this producer writes buffer pointers/capacities. With no pending
    // frame, the GUI cannot adopt target until we publish its index below.
    // Grow that non-displayed buffer first, retaining both existing buffers
    // on allocation failure. This also permits differently-sized next clips.
    if (self->_frame_buffer_capacity[target] < bytes) {
        const size_t capacity = (bytes + 15U) & ~static_cast<size_t>(15U);
        auto *replacement = static_cast<uint8_t *>(heap_caps_aligned_alloc(
            16, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!replacement) {
            ESP_LOGE("VideoPlayer", "Frame buffer %d allocation failed: need=%u PSRAM free=%u largest=%u",
                     target, static_cast<unsigned>(capacity),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
            if (mode != HOSINO_VIDEO_ORIGINAL) {
                // Retain the displayed frame and both allocations. Do not stop
                // audio because a larger presentation buffer was unavailable.
                // If the first enlarged frame already displayed, fall back to
                // 1:1 instead of retrying the second enlargement indefinitely.
                const auto applied = self->_scale_applied.load();
                auto expected = mode;
                if (self->_scale_mode.compare_exchange_strong(expected,
                        applied != mode ? applied : HOSINO_VIDEO_ORIGINAL)) self->_scale_failed.store(true);
                self->_dropped_frames.fetch_add(1);
                return ESP_OK;
            }
            return ESP_ERR_NO_MEM;
        }
        heap_caps_free(self->_frame_buffers[target]);
        self->_frame_buffers[target] = replacement;
        self->_frame_buffer_capacity[target] = capacity;
        ESP_LOGI("VideoPlayer", "Frame buffer %d: %ux%u capacity=%u PSRAM free=%u largest=%u",
                 target, plan.width, plan.height, static_cast<unsigned>(capacity),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    }
    const int64_t copy_started = esp_timer_get_time();
    if (!hosino_video_rgb565_copy(&plan, frame->data, frame->bytes, frame->stride_bytes,
            self->_frame_buffers[target], self->_frame_buffer_capacity[target], stride, self->_scale_scratch)) return ESP_ERR_INVALID_SIZE;
    self->_frame_copy_us[target] = static_cast<uint32_t>(esp_timer_get_time() - copy_started);
    self->_frame_width[target] = plan.width;
    self->_frame_height[target] = plan.height;
    self->_frame_source_width[target] = frame->width;
    self->_frame_source_height[target] = frame->height;
    self->_frame_scale_mode[target] = mode;
    self->_pending_buffer_index.store(target);
    return ESP_OK;
}

void VideoPlayer::workerTask(void *arg)
{
    auto *self = static_cast<VideoPlayer *>(arg);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!self->cleanupPlaybackResources()) {
        self->_state.store(State::Error);
    } else {
        self->scanDirectory();
    }
    bool advance = false;
    while (!self->_stop_requested.load()) {
        char path[HOSINO_MEDIA_PATH_MAX] = {};
        uint64_t seek_ms = 0;
        Command command = self->takeCommand(path, &seek_ms);
        if (command == Command::None && advance) command = Command::Next;
        advance = false;
        esp_err_t result = ESP_OK;
        const bool browse_command = command == Command::Files || command == Command::Parent ||
            command == Command::Refresh || command == Command::PagePrevious || command == Command::PageNext;
        if (browse_command || command == Command::OpenEntry || command == Command::Previous || command == Command::Next) {
            if (!self->cleanupPlaybackResources()) result = self->_last_error.load();
            if (result == ESP_OK && self->_canvas_detached.load() && !self->_ui_accept_frames.load()) self->releaseFrameBuffers();
            if (result == ESP_OK && browse_command) {
                if (command == Command::Parent && strcmp(self->_browser->directory, STORAGE_SERVICE_MOUNT_POINT)) {
                    char *slash = strrchr(self->_browser->directory, '/');
                    if (slash && slash > self->_browser->directory + strlen(STORAGE_SERVICE_MOUNT_POINT)) *slash = 0;
                    else strlcpy(self->_browser->directory, STORAGE_SERVICE_MOUNT_POINT, sizeof(self->_browser->directory));
                    self->_browser->page = 0;
                } else if (command == Command::PagePrevious && self->_browser->page) --self->_browser->page;
                else if (command == Command::PageNext && self->_browser->page < UINT32_MAX / PAGE_SIZE) ++self->_browser->page;
                self->_browser_visible.store(true);
                result = self->scanDirectory();
            } else if (result == ESP_OK && command == Command::OpenEntry) {
                struct stat info = {};
                // Metadata IO also holds a short lease; facade acquires its own
                // lease only after this one has been released.
                result = safe_sd_path(path) ? storage_service_acquire(&self->_storage_lease) : ESP_ERR_INVALID_ARG;
                if (result == ESP_OK && stat(path, &info)) result = ESP_FAIL;
                storage_service_release(&self->_storage_lease);
                if (result == ESP_OK && S_ISDIR(info.st_mode)) {
                    strlcpy(self->_browser->directory, path, sizeof(self->_browser->directory)); self->_browser->page = 0;
                    self->_browser_visible.store(true); result = self->scanDirectory();
                } else if (result == ESP_OK && S_ISREG(info.st_mode) && is_avi_file(path)) {
                    strlcpy(self->_browser->selected, path, sizeof(self->_browser->selected));
                    // A queued selection carries a complete immutable path.
                    // Keep its playlist directory correct even if navigation
                    // finished between the original row click and this work.
                    char selected_directory[HOSINO_MEDIA_PATH_MAX];
                    strlcpy(selected_directory, path, sizeof(selected_directory));
                    *strrchr(selected_directory, '/') = 0;
                    if (strcmp(self->_browser->directory, selected_directory)) {
                        strlcpy(self->_browser->directory, selected_directory, sizeof(self->_browser->directory));
                        self->_browser->page = 0;
                    }
                    if (!self->_browser_visible.load()) result = self->startSelected();
                } else if (result == ESP_OK) result = ESP_ERR_NOT_SUPPORTED;
            } else if (result == ESP_OK) {
                result = self->adjacentVideo(command == Command::Previous ? -1 : 1);
                if (result == ESP_OK) result = self->startSelected();
            }
        } else if (self->_media && command != Command::None) {
            hosino_media_player_snapshot_t snapshot = {};
            hosino_media_player_get_snapshot(self->_media, &snapshot);
            if (command == Command::Pause && snapshot.state == HOSINO_MEDIA_PLAYING) result = hosino_media_player_pause(self->_media);
            else if (command == Command::Play && snapshot.state == HOSINO_MEDIA_PAUSED) result = hosino_media_player_resume(self->_media);
            // A queued command can race EOF/preparation or a previous seek.
            if (result == ESP_ERR_INVALID_STATE || result == ESP_ERR_NOT_SUPPORTED) result = ESP_OK;
        }
        if (result == ESP_ERR_INVALID_STATE && (self->_stop_requested.load() || self->_browser_visible.load())) {
            // Back-to-list / home superseded an in-progress file open.
            if (self->cleanupPlaybackResources()) result = ESP_OK;
        }
        if (result == ESP_OK && self->_media) {
            result = hosino_media_player_poll(self->_media);
            hosino_media_player_snapshot_t snapshot = {};
            hosino_media_player_get_snapshot(self->_media, &snapshot);
            self->_audio_enabled.store(snapshot.audio_enabled);
            portENTER_CRITICAL(&browser_lock);
            self->_position_ms = snapshot.position_ms; self->_duration_ms = snapshot.duration_ms;
            portEXIT_CRITICAL(&browser_lock);
            if (result == ESP_OK) result = self->dispatchPendingSeek(snapshot);
            if (snapshot.state == HOSINO_MEDIA_ERROR) result = snapshot.last_error ? snapshot.last_error : ESP_FAIL;
            else if (snapshot.state == HOSINO_MEDIA_PLAYING) { self->_playing.store(true); self->_state.store(State::Playing); }
            else if (snapshot.state == HOSINO_MEDIA_PAUSED) self->_state.store(State::Paused);
            else if (snapshot.state == HOSINO_MEDIA_FINISHED) advance = true;
            if (result == ESP_OK && !self->_browser_visible.load() && !self->_stop_requested.load() &&
                snapshot.audio_enabled && (snapshot.state == HOSINO_MEDIA_PLAYING || snapshot.state == HOSINO_MEDIA_PAUSED)) {
                // A separate latest-value mailbox cannot overwrite Files/Seek/Next.
                // At most one I2C update per existing 80ms control-worker iteration.
                const int volume = self->_volume_requested.exchange(-1);
                if (volume >= 0) {
                    const esp_err_t volume_result = hosino_media_player_set_volume(self->_media, volume);
                    self->_volume_error.store(volume_result);
                    if (volume_result == ESP_OK) self->_volume_applied.store(volume);
                    else ESP_LOGW("VideoPlayer", "Volume update failed: %s", esp_err_to_name(volume_result));
                }
            }
        }
        if (result != ESP_OK && !self->_stop_requested.load()) {
            self->_last_error.store(result);
            self->cleanupPlaybackResources();
            self->_state.store(State::Error);
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    const bool cleaned = self->cleanupPlaybackResources();
    self->_state.store(cleaned ? State::Idle : State::Error);
    self->_worker_task.store(nullptr); self->_worker_stopped.store(true);
    vTaskDeleteWithCaps(nullptr);
}

void VideoPlayer::createBrowser()
{
    _browser_root = lv_obj_create(_viewport);
    lv_obj_remove_style_all(_browser_root);
    lv_obj_set_size(_browser_root, _viewport_width.load(), _viewport_height.load());
    lv_obj_set_style_bg_color(_browser_root, lv_color_hex(0x111118), 0);
    lv_obj_set_style_bg_opa(_browser_root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(_browser_root, &hosino_font_16, 0);
    lv_obj_set_style_text_color(_browser_root, lv_color_white(), 0);
    lv_obj_remove_flag(_browser_root, LV_OBJ_FLAG_SCROLLABLE);
    _directory_label = lv_label_create(_browser_root);
    lv_obj_set_pos(_directory_label, 24, 8); lv_obj_set_width(_directory_label, _viewport_width.load() - 48);
    lv_obj_set_style_text_font(_directory_label, &hosino_font_16, 0);
    lv_label_set_long_mode(_directory_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(_directory_label, STORAGE_SERVICE_MOUNT_POINT);
    auto button = [this](const char *text, Command command, int x, int y, int width) {
        lv_obj_t *object = lv_button_create(_browser_root);
        lv_obj_set_pos(object, x, y); lv_obj_set_size(object, width, 34);
        lv_obj_set_style_bg_color(object, lv_color_hex(0x34303F), 0);
        lv_obj_set_user_data(object, reinterpret_cast<void *>(static_cast<uintptr_t>(command)));
        lv_obj_add_event_cb(object, controlEvent, LV_EVENT_CLICKED, this);
        lv_obj_t *label = lv_label_create(object); lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &hosino_font_16, 0); lv_obj_center(label);
        return object;
    };
    _parent_button = button("上级目录", Command::Parent, 24, 36, 112);
    _refresh_button = button("刷新", Command::Refresh, _viewport_width.load() - 104, 36, 80);
    _decoder_button = button(_decoder.load() == HOSINO_MEDIA_DECODER_JPEGDEC ?
        "解码：B JPEGDEC" : "解码：A 乐鑫", Command::Decoder, 144, 36, _viewport_width.load() - 256);
    _decoder_label = lv_obj_get_child(_decoder_button, 0);
    lv_obj_set_style_text_font(_decoder_label, &hosino_font_14, 0);
    for (unsigned i = 0; i < PAGE_SIZE; ++i) {
        _entry_buttons[i] = lv_button_create(_browser_root);
        lv_obj_set_pos(_entry_buttons[i], 24, 82 + i * 52);
        lv_obj_set_size(_entry_buttons[i], _viewport_width.load() - 48, 46);
        lv_obj_set_style_bg_color(_entry_buttons[i], lv_color_hex(0x25242E), 0);
        lv_obj_set_style_text_align(_entry_buttons[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_user_data(_entry_buttons[i], reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
        lv_obj_add_event_cb(_entry_buttons[i], browserEvent, LV_EVENT_CLICKED, this);
        _entry_labels[i] = lv_label_create(_entry_buttons[i]);
        lv_obj_set_style_text_font(_entry_labels[i], &hosino_font_16, 0);
        lv_obj_set_width(_entry_labels[i], _viewport_width.load() - 78);
        lv_label_set_long_mode(_entry_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_align(_entry_labels[i], LV_ALIGN_LEFT_MID, 0, 0);
        lv_label_set_text(_entry_labels[i], "");
    }
    _browser_status = lv_label_create(_browser_root);
    lv_obj_set_width(_browser_status, _viewport_width.load() - 48);
    lv_obj_set_style_text_font(_browser_status, &hosino_font_14, 0);
    lv_obj_set_style_text_align(_browser_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_browser_status, LV_ALIGN_BOTTOM_MID, 0, -49);
    lv_label_set_text(_browser_status, "正在读取存储卡");
    const int footer = _viewport_height.load() - 42;
    _page_previous = button("上一页", Command::PagePrevious, 24, footer, 90);
    _page_next = button("下一页", Command::PageNext, _viewport_width.load() - 114, footer, 90);
    _page_label = lv_label_create(_browser_root);
    lv_obj_set_style_text_font(_page_label, &hosino_font_14, 0);
    lv_obj_align(_page_label, LV_ALIGN_BOTTOM_MID, 0, -17);
    lv_label_set_text(_page_label, "1 / 1");
}

void VideoPlayer::updateBrowser()
{
    if (!_browser || !_browser_root) return;
    unsigned revision, count, page, total;
    char directory[HOSINO_MEDIA_PATH_MAX];
    portENTER_CRITICAL(&browser_lock);
    revision = _browser_revision.load();
    count = _browser->published.count; page = _browser->published.page; total = _browser->published.total;
    strlcpy(directory, _browser->published.directory, sizeof(directory));
    portEXIT_CRITICAL(&browser_lock);
    if (revision != _last_browser_revision) {
        _last_browser_revision = revision;
        lv_label_set_text(_directory_label, directory[0] ? directory : STORAGE_SERVICE_MOUNT_POINT);
        for (unsigned i = 0; i < PAGE_SIZE; ++i) {
            if (i < count) {
                char name[HOSINO_MEDIA_PATH_MAX]; bool is_dir;
                portENTER_CRITICAL(&browser_lock);
                strlcpy(name, _browser->published.entries[i].name, sizeof(name));
                is_dir = _browser->published.entries[i].directory;
                portEXIT_CRITICAL(&browser_lock);
                // Let LVGL ellipsize by glyph; never byte-truncate UTF-8 names.
                lv_label_set_text_fmt(_entry_labels[i], "%s %s", is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_VIDEO, name);
                lv_obj_remove_flag(_entry_buttons[i], LV_OBJ_FLAG_HIDDEN);
            } else lv_obj_add_flag(_entry_buttons[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text_fmt(_page_label, "%u / %u", page + 1, std::max(1U, (total + PAGE_SIZE - 1) / PAGE_SIZE));
    }
    const State state = _state.load();
    const bool ready = state == State::Browsing;
    auto enable = [](lv_obj_t *object, bool value) {
        if (value) lv_obj_remove_state(object, LV_STATE_DISABLED); else lv_obj_add_state(object, LV_STATE_DISABLED);
    };
    for (unsigned i = 0; i < PAGE_SIZE; ++i) enable(_entry_buttons[i], ready && i < count);
    enable(_parent_button, (ready || state == State::Error) && directory[0] && strcmp(directory, STORAGE_SERVICE_MOUNT_POINT));
    enable(_refresh_button, ready || state == State::Error || state == State::NoCard);
    enable(_decoder_button, ready && _cleanup_complete.load() && _command.load() == Command::None);
    const char *decoder_text = _decoder.load() == HOSINO_MEDIA_DECODER_JPEGDEC ?
        "解码：B JPEGDEC" : "解码：A 乐鑫";
    if (strcmp(lv_label_get_text(_decoder_label), decoder_text)) lv_label_set_text(_decoder_label, decoder_text);
    enable(_page_previous, ready && page > 0);
    enable(_page_next, ready && static_cast<uint64_t>(page + 1) * PAGE_SIZE < total);
    const char *message = "选择文件夹或AVI视频";
    if (state == State::Mounting || state == State::Scanning) message = "正在读取目录";
    else if (state == State::Stopping) message = "正在停止播放";
    else if (state == State::Error || state == State::NoCard) message = "目录读取失败，请检查存储卡后刷新";
    else if (!total) message = "目录中没有文件夹或AVI视频";
    if (strcmp(lv_label_get_text(_browser_status), message)) lv_label_set_text(_browser_status, message);
}

void VideoPlayer::showBrowser()
{
    resetPinch();
    _ui_accept_frames.store(false);
    detachCanvas();
    _browser_visible.store(true);
    cancelPendingSeek();
    _frame_presented = false;
    _seek_dragging = false;
    _state.store(State::Stopping);
    if (_browser_root) {
        lv_obj_remove_flag(_browser_root, LV_OBJ_FLAG_HIDDEN);
        updateBrowser();
    }
    postCommand(Command::Files);
}

void VideoPlayer::browserEvent(lv_event_t *event)
{
    auto *self = static_cast<VideoPlayer *>(lv_event_get_user_data(event));
    if (!self || self->_state.load() != State::Browsing || !self->_browser) return;
    const unsigned index = static_cast<unsigned>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(static_cast<lv_obj_t *>(lv_event_get_target(event)))));
    char directory[HOSINO_MEDIA_PATH_MAX], name[HOSINO_MEDIA_PATH_MAX], path[HOSINO_MEDIA_PATH_MAX];
    bool is_dir = false; unsigned video_index = 0; bool valid;
    portENTER_CRITICAL(&browser_lock);
    valid = index < self->_browser->published.count && self->_last_browser_revision == self->_browser_revision.load();
    if (valid) {
        strlcpy(directory, self->_browser->published.directory, sizeof(directory));
        strlcpy(name, self->_browser->published.entries[index].name, sizeof(name));
        is_dir = self->_browser->published.entries[index].directory;
        video_index = self->_browser->published.entries[index].video_index;
    }
    portEXIT_CRITICAL(&browser_lock);
    if (!valid || !join_sd_path(path, directory, name)) return;
    // Publish GUI intent before handing the command to the other core. After
    // posting, the worker may already have completed the operation.
    self->_state.store(is_dir ? State::Scanning : State::Preparing);
    if (!is_dir) {
        self->_browser_visible.store(false); self->_ui_accept_frames.store(true);
        self->_current_index.store(video_index); self->_frame_presented = false;
        self->_fps_since_ms = 0; self->_video_fps = 0; self->showChrome();
    }
    if (!self->postCommand(Command::OpenEntry, path)) {
        self->_browser_visible.store(true); self->_ui_accept_frames.store(false);
        self->_last_error.store(ESP_ERR_INVALID_STATE); self->_state.store(State::Error);
    }
}

void VideoPlayer::seekEvent(lv_event_t *event)
{
    auto *self = static_cast<VideoPlayer *>(lv_event_get_user_data(event));
    if (!self || !self->_progress) return;
    const State state = self->_state.load();
    if (!self->_foreground || self->_browser_visible.load() ||
        (state != State::Playing && state != State::Paused)) {
        self->_seek_dragging = false;
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) { self->_seek_dragging = true; self->showChrome(); }
    if (code == LV_EVENT_RELEASED) {
        uint64_t duration;
        portENTER_CRITICAL(&browser_lock); duration = self->_duration_ms; portEXIT_CRITICAL(&browser_lock);
        if (self->_seek_dragging && duration) self->postCommand(Command::Seek,
            nullptr, duration * static_cast<unsigned>(lv_slider_get_value(self->_progress)) / 1000);
        self->_seek_dragging = false;
        self->showChrome();
    } else if (code == LV_EVENT_PRESS_LOST) self->_seek_dragging = false;
}

void VideoPlayer::controlEvent(lv_event_t *event)
{
    auto *self = static_cast<VideoPlayer *>(lv_event_get_user_data(event));
    if (!self || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
    Command command = static_cast<Command>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target)));
    const State state = self->_state.load();
    if (command == Command::Scale) {
        if (self->_foreground && !self->_browser_visible.load() && self->_frame_presented) {
            self->_scale_failed.store(false);
            self->_scale_mode.store(static_cast<hosino_video_scale_mode_t>((self->_scale_mode.load() + 1) % 3));
            // Apply from the next decoder frame. A paused image stays intact;
            // retaining another native frame only for this would cost memory.
            self->showChrome();
        }
        return;
    }
    if (command == Command::Decoder) {
        // Switch only after the previous player has stopped. Each selected clip
        // starts at zero, with one active decoder/audio session and identical UI.
        if (self->_browser_visible.load() && self->_cleanup_complete.load() &&
            state == State::Browsing && self->_command.load() == Command::None) {
            const auto next = self->_decoder.load() == HOSINO_MEDIA_DECODER_ESPRESSIF ?
                HOSINO_MEDIA_DECODER_JPEGDEC : HOSINO_MEDIA_DECODER_ESPRESSIF;
            self->_decoder.store(next);
            lv_label_set_text(self->_decoder_label, next == HOSINO_MEDIA_DECODER_JPEGDEC ?
                "解码：B JPEGDEC" : "解码：A 乐鑫");
        }
        return;
    }
    if (command == Command::Files) {
        // The file list must remain reachable during preparation and errors.
        if (!self->_browser_visible.load()) self->showBrowser();
        return;
    }
    const bool browser_command = command == Command::Parent || command == Command::Refresh ||
        command == Command::PagePrevious || command == Command::PageNext;
    if (browser_command) {
        const bool allowed = state == State::Browsing ||
            (state == State::Error && (command == Command::Parent || command == Command::Refresh)) ||
            (state == State::NoCard && command == Command::Refresh);
        if (!self->_browser_visible.load() || !allowed) return;
        self->_state.store(State::Scanning);
        if (!self->postCommand(command)) {
            self->_last_error.store(ESP_ERR_INVALID_STATE); self->_state.store(State::Error);
        }
        return;
    }
    if (state != State::Playing && state != State::Paused) return;
    self->showChrome();
    if (command == Command::Pause && state == State::Paused) {
        command = Command::Play;
    }
    self->postCommand(command);
}

void VideoPlayer::chromeEvent(lv_event_t *event)
{
    auto *self = static_cast<VideoPlayer *>(lv_event_get_user_data(event));
    if (!self || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    // LVGL emits CLICKED after a pinch's final RELEASED too. Keep this latch
    // until the next genuine PRESSED cycle, even if a remaining finger lingers.
    auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
    if (self->_pinch_seen && (target == self->_canvas || target == self->_viewport)) return;
    if (self->_state.load() != State::Playing) {
        self->showChrome();
        return;
    }
    if (self->_chrome_visible) {
        self->setChromeVisible(false);
        self->_chrome_hide_at_ms = 0;
    } else {
        self->showChrome();
    }
}

void VideoPlayer::pinchEvent(lv_event_t *event)
{
#if LV_USE_GESTURE_RECOGNITION
    auto *self = static_cast<VideoPlayer *>(lv_event_get_user_data(event));
    if (!self) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED &&
        code != LV_EVENT_PRESS_LOST && code != LV_EVENT_GESTURE) return;
    auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
    // Ignore a control's bubbled gesture and the duplicate parent delivery.
    if ((target != self->_canvas && target != self->_viewport) ||
        target != lv_event_get_current_target(event)) return;

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        self->_pinch_armed = self->_pinch_applied = false;
        return; // _pinch_seen suppresses this cycle's synthesized CLICKED.
    }
    const State state = self->_state.load();
    const bool ready = self->_foreground && self->_screen == lv_screen_active() &&
        !self->_browser_visible.load() && self->_frame_presented && self->_viewport &&
        !self->_seek_dragging && !self->_volume_dragging &&
        (state == State::Playing || state == State::Paused);
    if (code == LV_EVENT_PRESSED) {
        self->resetPinch();
        if (!ready) return;
        lv_indev_t *indev = lv_event_get_indev(event);
        if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        lv_area_t area;
        lv_obj_get_coords(self->_viewport, &area);
        if (!video_pinch_inside(point.x, point.y, area.x1, area.y1, area.x2, area.y2)) return;
        lv_display_t *display = lv_obj_get_display(self->_screen);
        const int32_t display_width = lv_display_get_horizontal_resolution(display);
        const int32_t display_height = lv_display_get_vertical_resolution(display);
        // Also exclude the physical display's outer 40px. Brookesia's phone
        // navigation uses the main pointer's start position independently of
        // LVGL event bubbling, so stopping a gesture event alone is insufficient.
        if (display_width <= 80 || display_height <= 80 || point.x < 40 || point.y < 40 ||
            point.x >= display_width - 40 || point.y >= display_height - 40) return;
        // Blank header/control backgrounds are input-transparent; the original
        // target check keeps real buttons/sliders exclusive. Do not reject a
        // canvas target merely because visible chrome occupies those pixels.
        self->_pinch_armed = true;
        return;
    }
    if (lv_event_get_gesture_type(event) != LV_INDEV_GESTURE_PINCH) return;
    const auto gesture_state = lv_event_get_gesture_state(event, LV_INDEV_GESTURE_PINCH);
    if (gesture_state == LV_INDEV_GESTURE_STATE_ENDED || gesture_state == LV_INDEV_GESTURE_STATE_CANCELED) {
        self->_pinch_armed = self->_pinch_applied = false;
        return;
    }
    if (!ready || !self->_pinch_armed || gesture_state != LV_INDEV_GESTURE_STATE_RECOGNIZED) return;
    self->_pinch_seen = true;
    lv_event_stop_bubbling(event); // App content only; never captures a screen-edge start.
    if (self->_pinch_applied) return;
    const float scale = lv_event_get_pinch_scale(event);
    const int mode = video_pinch_mode(scale);
    if (mode < 0) return;
    self->_pinch_applied = true;
    self->_scale_failed.store(false);
    self->_scale_mode.store(static_cast<hosino_video_scale_mode_t>(mode));
    // Reuse the existing two-frame producer. Paused mode changes apply on resume.
    self->showChrome();
    ESP_UTILS_LOGI("VIDEO_PINCH mode=%s scale=%.3f", mode == HOSINO_VIDEO_FILL ? "fill" : "fit", scale);
#else
    (void)event;
#endif
}

void VideoPlayer::volumeEvent(lv_event_t *event)
{
    auto *self = static_cast<VideoPlayer *>(lv_event_get_user_data(event));
    if (!self || !self->_foreground || self->_browser_visible.load() || !self->_volume_slider) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) self->_volume_dragging = true;
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) self->_volume_dragging = false;
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESSED) return;
    self->showChrome();
    const State state = self->_state.load();
    if (self->_audio_enabled.load() && (state == State::Playing || state == State::Paused)) {
        const int value = lv_slider_get_value(self->_volume_slider);
        self->_volume_requested.store(value);
        self->_volume_error.store(ESP_OK);
    }
}

void VideoPlayer::uiTimer(lv_timer_t *timer)
{
    auto *self = static_cast<VideoPlayer *>(lv_timer_get_user_data(timer));
    if (self) {
        self->updateUi();
    }
}

void VideoPlayer::updateUi()
{
    if (!_foreground || !_screen) return;
    if (_restart_pending && _worker_stopped.load() && !_worker_task.load()) {
        _restart_pending = false;
        const bool started = startWorker();
        _ui_accept_frames.store(false);
        if (!started) _last_error.store(ESP_ERR_NO_MEM);
    }
    const bool browsing = _browser_visible.load();
    if (browsing) {
        _ui_accept_frames.store(false);
        if (!_canvas_detached.load()) detachCanvas();
        if (_browser_root) lv_obj_remove_flag(_browser_root, LV_OBJ_FLAG_HIDDEN);
        updateBrowser();
        return;
    }
    if (_browser_root) lv_obj_add_flag(_browser_root, LV_OBJ_FLAG_HIDDEN);
    _ui_accept_frames.store(true);
    const int pending_index = _pending_buffer_index.load();
    if (pending_index >= 0 && pending_index < 2 && _canvas && _ui_accept_frames.load()) {
        // Publish the new display owner before reopening the other buffer to
        // the decoder. This ordering prevents the callback from decoding into
        // the frame that the canvas is about to display.
        _display_buffer_index.store(pending_index);
        _canvas_detached.store(false);
        _scale_applied.store(_frame_scale_mode[pending_index]);
        _view_copy_sum_us += _frame_copy_us[pending_index];
        _view_copy_max_us = std::max(_view_copy_max_us, _frame_copy_us[pending_index]);
        ++_view_copy_samples;
        lv_canvas_set_buffer(
            _canvas,
            _frame_buffers[pending_index],
            _frame_width[pending_index],
            _frame_height[pending_index],
            LV_COLOR_FORMAT_RGB565
        );
        lv_obj_set_size(_canvas, _frame_width[pending_index], _frame_height[pending_index]);
        applyViewScale();
        lv_obj_invalidate(_canvas);
        _pending_buffer_index.store(-1);
        _decoded_frames.fetch_add(1);
        _frame_presented = true;
        if (_hero_icon) {
            lv_obj_add_flag(_hero_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const State state = _state.load();
    const int count = _file_count.load();
    const int index = _current_index.load();
    if (state != _last_ui_state) {
        if (state == State::Playing) {
            showChrome();
        } else {
            setChromeVisible(true);
            _chrome_hide_at_ms = 0;
        }
        _last_ui_state = state;
    }
    if (state == State::Playing && !_seek_dragging && !_volume_dragging && !_pinch_armed && _chrome_visible && _chrome_hide_at_ms != 0 &&
            static_cast<int32_t>(lv_tick_get() - _chrome_hide_at_ms) >= 0) {
        setChromeVisible(false);
        _chrome_hide_at_ms = 0;
    }
    updateControlState(state, count);
    const bool audio_controls = _audio_enabled.load() && (state == State::Playing || state == State::Paused);
    if (_volume_slider) {
        if (audio_controls) lv_obj_remove_state(_volume_slider, LV_STATE_DISABLED);
        else lv_obj_add_state(_volume_slider, LV_STATE_DISABLED);
        if (!_volume_dragging && _volume_requested.load() < 0)
            lv_slider_set_value(_volume_slider, _volume_applied.load(), LV_ANIM_OFF);
    }
    if (_volume_label) {
        char text[40];
        if (_volume_error.load() != ESP_OK) snprintf(text, sizeof(text), "音量失败");
        else snprintf(text, sizeof(text), "音量 %d", static_cast<int>(_volume_slider ? lv_slider_get_value(_volume_slider) : _volume_applied.load()));
        if (strcmp(lv_label_get_text(_volume_label), text)) lv_label_set_text(_volume_label, text);
    }
    if (_scale_button) {
        if (_frame_presented) lv_obj_remove_state(_scale_button, LV_STATE_DISABLED);
        else lv_obj_add_state(_scale_button, LV_STATE_DISABLED);
    }
    if (_scale_label) {
        const auto mode = _scale_mode.load();
        const char *label = _scale_failed.load() ? "缩放内存不足" :
            (state == State::Paused && mode != _scale_applied.load()) ? "续播后生效" :
            mode == HOSINO_VIDEO_FIT ? "画面：适应" :
            mode == HOSINO_VIDEO_FILL ? "画面：填充" : "画面：原尺寸";
        if (strcmp(lv_label_get_text(_scale_label), label)) {
            lv_label_set_text(_scale_label, label);
            lv_obj_center(_scale_label);
        }
    }
    uint64_t position, duration;
    portENTER_CRITICAL(&browser_lock); position = _position_ms; duration = _duration_ms; portEXIT_CRITICAL(&browser_lock);
    const bool can_seek = duration && (state == State::Playing || state == State::Paused);
    lv_obj_t *seek_controls[] = {_progress, _backward_button, _forward_button};
    for (lv_obj_t *control : seek_controls) {
        if (!control) continue;
        if (can_seek) lv_obj_remove_state(control, LV_STATE_DISABLED); else lv_obj_add_state(control, LV_STATE_DISABLED);
    }
    if (_progress && !_seek_dragging) lv_slider_set_value(_progress,
        duration ? static_cast<int>(std::min(position, duration) * 1000 / duration) : 0, LV_ANIM_OFF);
    if (_time_label) {
        char current[24], total[24], text[56];
        time_text(current, sizeof(current), position, duration != 0); time_text(total, sizeof(total), duration, duration != 0);
        snprintf(text, sizeof(text), "%s / %s", current, total);
        if (strcmp(lv_label_get_text(_time_label), text)) lv_label_set_text(_time_label, text);
    }

    const uint32_t now = lv_tick_get();
    const uint32_t frames = _decoded_frames.load();
    if (!_fps_since_ms) { _fps_since_ms = now; _fps_frames = frames; }
    if (now - _fps_since_ms >= 2000) {
        _video_fps = 1000.0f * (frames - _fps_frames) / (now - _fps_since_ms);
        ESP_UTILS_LOGI("VIDEO_STATS ui_submit_fps=%.1f frames=%lu dropped=%lu state=%u decoder=%s",
            static_cast<double>(_video_fps), static_cast<unsigned long>(frames),
            static_cast<unsigned long>(_dropped_frames.load()), static_cast<unsigned>(state),
            _decoder.load() == HOSINO_MEDIA_DECODER_JPEGDEC ? "B-JPEGDEC" : "A-ESPRESSIF");
        const int displayed = _display_buffer_index.load();
        if (displayed >= 0 && displayed < 2 && !_canvas_detached.load()) {
            ESP_UTILS_LOGI("VIDEO_VIEW mode=%u src=%ux%u dst=%ux%u samples=%lu copy_avg_us=%lu copy_max_us=%lu",
                static_cast<unsigned>(_frame_scale_mode[displayed]), _frame_source_width[displayed],
                _frame_source_height[displayed], _frame_width[displayed], _frame_height[displayed],
                static_cast<unsigned long>(_view_copy_samples),
                static_cast<unsigned long>(_view_copy_samples ? _view_copy_sum_us / _view_copy_samples : 0),
                static_cast<unsigned long>(_view_copy_max_us));
        }
        _view_copy_sum_us = _view_copy_max_us = _view_copy_samples = 0;
        _fps_since_ms = now;
        _fps_frames = frames;
    }
    if (_counter_label) {
        char counter[48];
        snprintf(counter, sizeof(counter), "%c %d/%d %.1f FPS",
                 _decoder.load() == HOSINO_MEDIA_DECODER_JPEGDEC ? 'B' : 'A',
                 count ? index + 1 : 0, count, static_cast<double>(_video_fps));
        if (strcmp(lv_label_get_text(_counter_label), counter) != 0) {
            lv_label_set_text(_counter_label, counter);
        }
    }
    if (_play_button_label) {
        const char *symbol = state == State::Paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE;
        if (strcmp(lv_label_get_text(_play_button_label), symbol) != 0) {
            lv_label_set_text(_play_button_label, symbol);
        }
    }
    if (_status_label) {
        const char *text = nullptr;
        switch (state) {
        case State::Mounting: text = "正在读取存储卡"; break;
        case State::Scanning: text = "正在查找视频"; break;
        case State::Preparing: text = "正在准备播放"; break;
        case State::Paused: text = "已暂停 / 继续播放"; break;
        case State::Empty:
            text = "未找到视频\n请选择目录中的MJPEG AVI";
            break;
        case State::NoCard:
            text = "存储卡未就绪\n请插入卡后关闭应用再重开";
            break;
        case State::Error:
            text = _last_error.load() == ESP_ERR_NO_MEM
                ? "播放内存不足\n请关闭其他应用后重试"
                : (_decoder.load() == HOSINO_MEDIA_DECODER_JPEGDEC
                    ? "B解码失败\n请返回列表切换A后重试"
                    : "视频无法播放\n请使用MJPEG AVI和PCM音频\n最大480宽，或关闭占用音频的应用");
            break;
        case State::Stopping: text = "正在结束上次播放"; break;
        case State::Idle: text = "播放已停止"; break;
        case State::Browsing: case State::Playing: default: break;
        }
        if (text) {
            if (strcmp(lv_label_get_text(_status_label), text) != 0) lv_label_set_text(_status_label, text);
            lv_obj_remove_flag(_status_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_status_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

} // namespace esp_brookesia::apps
