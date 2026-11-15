// SPDX-License-Identifier: Apache-2.0
// Two deterministic, self-contained workloads for the display FPS overlay.
#include "sdkconfig.h"
#if defined(CONFIG_HOSINO_PERF_DEMO) && CONFIG_HOSINO_PERF_DEMO

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

#include "lvgl.h"
#include "hosino_ui_fonts.h"
#include "esp_brookesia.hpp"

namespace esp_brookesia::apps {
namespace {

constexpr int kIconSize = 64;
constexpr uint16_t rgb565(uint32_t color)
{
    return static_cast<uint16_t>(((color >> 8) & 0xf800) | ((color >> 5) & 0x07e0) |
                                 ((color >> 3) & 0x001f));
}

constexpr std::array<uint16_t, kIconSize * kIconSize> make_icon(bool list)
{
    std::array<uint16_t, kIconSize * kIconSize> pixels{};
    for (int y = 0; y < kIconSize; ++y) {
        for (int x = 0; x < kIconSize; ++x) {
            uint32_t color = list ? 0x087f83 : 0x3155cd;
            if (list) {
                const bool line = x >= 12 && x < 47 &&
                                  ((y >= 14 && y < 21) || (y >= 29 && y < 36) ||
                                   (y >= 44 && y < 51));
                if (line) color = 0xeaffff;
                if (x >= 52 && x < 56 && y >= 17 && y < 39) color = 0xffc45c;
            } else {
                if ((x >= 11 && x < 21 && y >= 36 && y < 53) ||
                    (x >= 27 && x < 37 && y >= 25 && y < 53) ||
                    (x >= 43 && x < 53 && y >= 12 && y < 53)) color = 0xeef4ff;
            }
            pixels[y * kIconSize + x] = rgb565(color);
        }
    }
    return pixels;
}

alignas(4) constexpr auto kRenderPixels = make_icon(false);
alignas(4) constexpr auto kScrollPixels = make_icon(true);

const lv_image_dsc_t kRenderIcon = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
               .flags = 0, .w = kIconSize, .h = kIconSize, .stride = kIconSize * 2},
    .data_size = sizeof(kRenderPixels),
    .data = reinterpret_cast<const uint8_t *>(kRenderPixels.data()),
};
const lv_image_dsc_t kScrollIcon = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
               .flags = 0, .w = kIconSize, .h = kIconSize, .stride = kIconSize * 2},
    .data_size = sizeof(kScrollPixels),
    .data = reinterpret_cast<const uint8_t *>(kScrollPixels.data()),
};

constexpr std::array<uint32_t, 6> kColors = {
    0x285dd9, 0x286fae, 0x138b8c, 0x6a4fba, 0xbd567c, 0xc2823c,
};

void plain_box(lv_obj_t *obj, uint32_t color, int radius = 0)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *text(lv_obj_t *parent, const char *value, int x, int y,
               const lv_font_t *font = &hosino_font_14, uint32_t color = 0xe7edfa)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, value);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

int bounce(uint32_t value, int extent)
{
    if (extent <= 0) return 0;
    const uint32_t phase = value % (static_cast<uint32_t>(extent) * 2);
    return static_cast<int>(phase <= static_cast<uint32_t>(extent) ? phase : extent * 2 - phase);
}

// Timers are created inside run(), so Brookesia records and deletes them together
// with the default screen. pause()/close() stop work before the screen is hidden.
class BenchmarkApp : public systems::phone::App {
public:
    BenchmarkApp(const char *name, const lv_image_dsc_t *icon)
        : App(name, icon, true, true, false) {}

protected:
    bool back() override { return notifyCoreClosed(); }

    bool pause() override
    {
        foreground_ = false;
        if (timer_) lv_timer_pause(timer_);
        return true;
    }

    bool resume() override
    {
        foreground_ = true;
        if (timer_ && playing_) {
            lv_timer_reset(timer_);
            lv_timer_resume(timer_);
        }
        return true;
    }

    bool close() override { return pause(); }

    bool cleanResource() override
    {
        // The core owns deletion; do not double-delete recorded timers/screens.
        if (timer_) lv_timer_pause(timer_);
        foreground_ = false;
        timer_ = nullptr;
        screen_ = nullptr;
        button_label_ = nullptr;
        return true;
    }

    void create_header(const char *title, const char *subtitle)
    {
        screen_ = lv_screen_active();
        plain_box(screen_, 0x091222);
        width_ = lv_area_get_width(&getVisualArea());
        height_ = lv_area_get_height(&getVisualArea());
        foreground_ = true;
        playing_ = true;
        text(screen_, title, 16, 12, &hosino_font_20);
        text(screen_, subtitle, 16, 42, &hosino_font_14, 0x9db0d2);

        lv_obj_t *button = lv_button_create(screen_);
        lv_obj_set_size(button, 102, 40);
        lv_obj_set_pos(button, width_ - 118, 10);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x253a60), 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        button_label_ = lv_label_create(button);
        lv_obj_set_style_text_font(button_label_, &hosino_font_14, 0);
        lv_label_set_text(button_label_, "暂停");
        lv_obj_center(button_label_);
        lv_obj_add_event_cb(button, toggle_cb, LV_EVENT_CLICKED, this);

        // The bottom 68 px of the visual area stay free for the central FPS HUD
        // and the system's existing bottom-edge navigation gesture.
    }

    bool start_timer()
    {
        timer_ = lv_timer_create(timer_cb, 15, this);
        return timer_ != nullptr;
    }

    void set_playing(bool value)
    {
        playing_ = value;
        if (button_label_) lv_label_set_text(button_label_, value ? "暂停" : "运行");
        if (!timer_) return;
        if (playing_ && foreground_) {
            lv_timer_reset(timer_);
            lv_timer_resume(timer_);
        } else {
            lv_timer_pause(timer_);
        }
    }

    virtual void tick() = 0;

    lv_obj_t *screen_ = nullptr;
    lv_obj_t *button_label_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    int width_ = 480;
    int height_ = 440;
    bool foreground_ = false;
    bool playing_ = true;

private:
    static void toggle_cb(lv_event_t *event)
    {
        auto *app = static_cast<BenchmarkApp *>(lv_event_get_user_data(event));
        app->set_playing(!app->playing_);
    }

    static void timer_cb(lv_timer_t *timer)
    {
        auto *app = static_cast<BenchmarkApp *>(lv_timer_get_user_data(timer));
        if (app->foreground_ && app->playing_ && app->screen_) app->tick();
    }
};

} // namespace

class RenderTest : public BenchmarkApp {
public:
    RenderTest() : BenchmarkApp("渲染测试", &kRenderIcon) {}

protected:
    bool run() override
    {
        phase_ = 0;
        create_header("渲染测试", "全屏重绘 / 目标每15毫秒更新");
        canvas_ = lv_obj_create(screen_);
        plain_box(canvas_, 0x132442);
        canvas_width_ = width_ - 24;
        canvas_height_ = std::max(120, height_ - 140);
        lv_obj_set_pos(canvas_, 12, 72);
        lv_obj_set_size(canvas_, canvas_width_, canvas_height_);
        lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
        band_height_ = (canvas_height_ + 9) / 10;
        for (size_t i = 0; i < bands_.size(); ++i) {
            bands_[i] = lv_obj_create(canvas_);
            plain_box(bands_[i], kColors[i % kColors.size()]);
            lv_obj_set_size(bands_[i], canvas_width_, band_height_ + 1);
            lv_obj_remove_flag(bands_[i], LV_OBJ_FLAG_CLICKABLE);
        }
        for (size_t i = 0; i < shapes_.size(); ++i) {
            shapes_[i] = lv_obj_create(canvas_);
            plain_box(shapes_[i], 0xe8f1ff, (i % 2) ? LV_RADIUS_CIRCLE : 8);
            lv_obj_set_size(shapes_[i], 28 + i * 6, 28 + i * 6);
            lv_obj_set_style_bg_opa(shapes_[i], LV_OPA_80, 0);
            lv_obj_remove_flag(shapes_[i], LV_OBJ_FLAG_CLICKABLE);
        }
        tick();
        return start_timer();
    }

    void tick() override
    {
        phase_ += 4;
        const int cycle = band_height_ * static_cast<int>(bands_.size());
        for (size_t i = 0; i < bands_.size(); ++i) {
            const auto offset = (i * band_height_ + phase_ / 2) % static_cast<uint32_t>(cycle);
            lv_obj_set_y(bands_[i], static_cast<int>(offset) - band_height_);
        }
        for (size_t i = 0; i < shapes_.size(); ++i) {
            const int size = 28 + static_cast<int>(i) * 6;
            lv_obj_set_pos(shapes_[i], bounce(phase_ * (i + 1) / 2 + i * 47, canvas_width_ - size),
                           bounce(phase_ + i * 67, canvas_height_ - size));
        }
        // The app's own screen is resized below the status bar. Invalidate the
        // full-size top layer so this workload redraws the physical display,
        // including the unchanged system bars, rather than just the app canvas.
        lv_obj_invalidate(lv_display_get_layer_top(lv_obj_get_display(screen_)));
    }

private:
    lv_obj_t *canvas_ = nullptr;
    std::array<lv_obj_t *, 12> bands_{};
    std::array<lv_obj_t *, 6> shapes_{};
    uint32_t phase_ = 0;
    int canvas_width_ = 456;
    int canvas_height_ = 330;
    int band_height_ = 33;
};

class ScrollTest : public BenchmarkApp {
public:
    ScrollTest() : BenchmarkApp("滚动测试", &kScrollIcon) {}

protected:
    bool run() override
    {
        direction_ = 1;
        create_header("滚动测试", "40张卡片 / 触摸列表暂停");
        list_ = lv_obj_create(screen_);
        plain_box(list_, 0x091222);
        lv_obj_set_pos(list_, 12, 72);
        lv_obj_set_size(list_, width_ - 24, std::max(120, height_ - 140));
        lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(list_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_ACTIVE);
        lv_obj_set_style_pad_all(list_, 2, 0);
        lv_obj_set_style_pad_row(list_, 8, 0);
        lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_event_cb(list_, touch_cb, LV_EVENT_PRESSED, this);
        static const char *details[] = {"语音 / 对话", "角色 / 动画", "任务 / 提醒", "音乐 / 播放"};
        for (int i = 0; i < 40; ++i) {
            lv_obj_t *card = lv_obj_create(list_);
            plain_box(card, (i % 2) ? 0x182741 : 0x132039, 12);
            lv_obj_set_size(card, LV_PCT(100), 88);
            lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_t *tile = lv_obj_create(card);
            plain_box(tile, kColors[i % kColors.size()], 10);
            lv_obj_set_size(tile, 46, 46);
            lv_obj_set_pos(tile, 12, 16);
            lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t *number = text(tile, "", 0, 0, &hosino_font_20);
            lv_label_set_text_fmt(number, "%02d", i + 1);
            lv_obj_center(number);
            lv_obj_t *title = text(card, "", 74, 14, &hosino_font_18);
            lv_label_set_text_fmt(title, "示例卡片 %02d", i + 1);
            text(card, details[i % 4], 74, 40, &hosino_font_14, 0xa9bad7);
            lv_obj_t *bar = lv_obj_create(card);
            plain_box(bar, kColors[i % kColors.size()], 2);
            lv_obj_set_pos(bar, 74, 67);
            lv_obj_set_size(bar, 80 + (i * 31) % 230, 4);
            lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_update_layout(list_);
        return start_timer();
    }

    void tick() override
    {
        const int current = lv_obj_get_scroll_y(list_);
        const int limit = std::max<int32_t>(0, current + lv_obj_get_scroll_bottom(list_));
        if (!limit) return;
        int next = current + direction_ * 6;
        if (next >= limit) { next = limit; direction_ = -1; }
        if (next <= 0) { next = 0; direction_ = 1; }
        lv_obj_scroll_to_y(list_, next, LV_ANIM_OFF);
    }

private:
    static void touch_cb(lv_event_t *event)
    {
        auto *app = static_cast<ScrollTest *>(lv_event_get_user_data(event));
        app->set_playing(false);
    }

    lv_obj_t *list_ = nullptr;
    int direction_ = 1;
};

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, RenderTest, "Render Test", []()
{
    return std::make_shared<RenderTest>();
})

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, ScrollTest, "Scroll Test", []()
{
    return std::make_shared<ScrollTest>();
})

} // namespace esp_brookesia::apps

#endif // CONFIG_HOSINO_PERF_DEMO
