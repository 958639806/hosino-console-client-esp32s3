/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 * App-local digital/analog adaptation of the official release/v0.6 Timer.
 * That upstream application is a clock, not a countdown timer. See README.md.
 */
#include <algorithm>
#include <cmath>
#include <ctime>
#include "esp_brookesia.hpp"
#include "hosino_ui_fonts.h"

LV_IMG_DECLARE(img_app_timer);
LV_FONT_DECLARE(ui_font_Number_big);

namespace esp_brookesia::apps {
namespace {
constexpr char kName[] = "时钟";
constexpr double kPi = 3.14159265358979323846;
void plain(lv_obj_t *obj, bool opaque = false)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x0a1420), 0);
    lv_obj_set_style_bg_opa(obj, opaque ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}
lv_obj_t *text(lv_obj_t *parent, const char *value, const lv_font_t *font)
{
    auto label = lv_label_create(parent);
    lv_label_set_text(label, value);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xe9f1fb), 0);
    return label;
}
} // namespace

class Timer : public systems::phone::App {
public:
    Timer() : App(kName, &img_app_timer, true, true, false) {}
    bool init() override { return true; }
    bool deinit() override { return true; }

    bool run() override
    {
        const int width = lv_area_get_width(&getVisualArea());
        const int usable = lv_area_get_height(&getVisualArea()) - 68;
        if (width < 320 || usable < 300) return false;
        auto screen = lv_screen_active();
        plain(screen, true);
        auto title = text(screen, kName, &hosino_font_20);
        lv_obj_set_pos(title, 44, 10);
        date_ = text(screen, "系统未校时", &hosino_font_16);
        lv_obj_align(date_, LV_ALIGN_TOP_MID, 0, 44);
        const int body_height = usable - 134;
        digital_ = lv_obj_create(screen);
        plain(digital_);
        lv_obj_set_size(digital_, width - 88, body_height);
        lv_obj_set_pos(digital_, 44, 78);
        digits_ = text(digital_, "--:--", &hosino_font_20);
        lv_obj_align(digits_, LV_ALIGN_CENTER, 0, -22);
        seconds_ = text(digital_, "等待系统时间", &hosino_font_18);
        lv_obj_align(seconds_, LV_ALIGN_CENTER, 0, 62);

        analog_ = lv_obj_create(screen);
        plain(analog_);
        dial_size_ = std::min(body_height, width - 100);
        lv_obj_set_size(analog_, dial_size_, dial_size_);
        lv_obj_set_pos(analog_, (width - dial_size_) / 2, 78);
        lv_obj_set_style_border_width(analog_, 2, 0);
        lv_obj_set_style_border_color(analog_, lv_color_hex(0x395774), 0);
        lv_obj_set_style_radius(analog_, LV_RADIUS_CIRCLE, 0);
        // Point arrays are members: LVGL retains their addresses.
        const int center = dial_size_ / 2;
        for (int i = 0; i < 12; ++i) {
            const double angle = i * kPi / 6;
            const int tick_size = i % 3 == 0 ? 6 : 3;
            auto tick = lv_obj_create(analog_);
            plain(tick);
            lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(tick, lv_color_hex(0x8aa6c4), 0);
            lv_obj_set_size(tick, tick_size, tick_size);
            lv_obj_set_pos(tick,
                center + static_cast<int>(std::lround(std::sin(angle) * (center - 14))) - tick_size / 2,
                center - static_cast<int>(std::lround(std::cos(angle) * (center - 14))) - tick_size / 2);
        }
        for (int i = 0; i < 3; ++i) {
            hands_[i] = lv_line_create(analog_);
            lv_obj_set_style_line_width(hands_[i], i == 0 ? 6 : (i == 1 ? 4 : 2), 0);
            lv_obj_set_style_line_color(hands_[i], lv_color_hex(i == 2 ? 0xf4a860 : 0xdbeafe), 0);
            lv_obj_set_style_line_rounded(hands_[i], true, 0);
        }
        unsynced_ = text(analog_, "未校时", &hosino_font_20);
        lv_obj_center(unsynced_);
        lv_obj_add_flag(analog_, LV_OBJ_FLAG_HIDDEN);
        auto button = lv_button_create(screen);
        lv_obj_set_size(button, 136, 36);
        lv_obj_set_pos(button, (width - 136) / 2, usable - 38);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x264560), 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        auto button_label = text(button, "切换表盘", &hosino_font_16);
        lv_obj_center(button_label);
        lv_obj_add_event_cb(button, switch_cb, LV_EVENT_CLICKED, this);
        foreground_ = true;
        analog_selected_ = false;
        updateTimeDisplay();
        // Created during run(), so Brookesia collects and deletes this timer.
        // All UI updates execute in the LVGL task, never an esp_timer callback.
        timer_ = lv_timer_create(tick_cb, 1000, this);
        return timer_ != nullptr;
    }

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
        if (digits_) updateTimeDisplay();
        if (timer_) { lv_timer_reset(timer_); lv_timer_resume(timer_); }
        return true;
    }
    bool close() override { return pause(); }
    bool cleanResource() override
    {
        pause();
        timer_ = nullptr;
        digital_ = analog_ = date_ = digits_ = seconds_ = unsynced_ = nullptr;
        for (auto &hand : hands_) hand = nullptr;
        return true;
    }

private:
    bool getSystemTime(std::tm &value)
    {
        const std::time_t now = std::time(nullptr);
        // IDF boots at the Unix epoch without RTC/SNTP initialization. Do not
        // present that value, compile time, or a fabricated date as real time.
        if (!localtime_r(&now, &value)) return false;
        return value.tm_year >= 124 && value.tm_year < 300;
    }
    void updateTimeDisplay()
    {
        if (!foreground_ || !digits_) return;
        std::tm now{};
        const bool valid = getSystemTime(now);
        if (!valid) {
            lv_label_set_text(date_, "系统未校时");
            lv_obj_set_style_text_font(digits_, &hosino_font_20, 0);
            lv_label_set_text(digits_, "--:--");
            lv_label_set_text(seconds_, "等待系统时间");
            lv_obj_remove_flag(unsynced_, LV_OBJ_FLAG_HIDDEN);
            for (auto hand : hands_) lv_obj_add_flag(hand, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        static const char *const weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        lv_label_set_text_fmt(date_, "%04d-%02d-%02d  周%s", now.tm_year + 1900,
                              now.tm_mon + 1, now.tm_mday, weekdays[now.tm_wday]);
        lv_obj_set_style_text_font(digits_, &ui_font_Number_big, 0);
        lv_label_set_text_fmt(digits_, "%02d:%02d", now.tm_hour, now.tm_min);
        lv_label_set_text_fmt(seconds_, "%02d 秒", now.tm_sec);
        lv_obj_add_flag(unsynced_, LV_OBJ_FLAG_HIDDEN);
        for (auto hand : hands_) lv_obj_remove_flag(hand, LV_OBJ_FLAG_HIDDEN);
        updateAnalogClock(now);
    }
    void updateAnalogClock(const std::tm &now)
    {
        // The official 30/6/6 degree conversion, with fractional hour/minute
        // progress so the hands track the same system time as the digital face.
        const double degrees[] = {(now.tm_hour % 12) * 30.0 + now.tm_min * 0.5,
                                   now.tm_min * 6.0 + now.tm_sec * 0.1,
                                   now.tm_sec * 6.0};
        const int center = dial_size_ / 2;
        const int lengths[] = {center * 50 / 100, center * 72 / 100, center * 80 / 100};
        for (int i = 0; i < 3; ++i) {
            const double angle = degrees[i] * kPi / 180;
            hand_points_[i][0].x = center;
            hand_points_[i][0].y = center;
            hand_points_[i][1].x = center + static_cast<int>(std::lround(std::sin(angle) * lengths[i]));
            hand_points_[i][1].y = center - static_cast<int>(std::lround(std::cos(angle) * lengths[i]));
            lv_line_set_points(hands_[i], hand_points_[i], 2);
        }
    }
    static void tick_cb(lv_timer_t *timer)
    {
        auto app = static_cast<Timer *>(lv_timer_get_user_data(timer));
        if (app) app->updateTimeDisplay();
    }
    static void switch_cb(lv_event_t *event)
    {
        auto app = static_cast<Timer *>(lv_event_get_user_data(event));
        if (!app || !app->foreground_) return;
        app->analog_selected_ = !app->analog_selected_;
        lv_obj_set_flag(app->digital_, LV_OBJ_FLAG_HIDDEN, app->analog_selected_);
        lv_obj_set_flag(app->analog_, LV_OBJ_FLAG_HIDDEN, !app->analog_selected_);
        app->updateTimeDisplay();
    }
    bool foreground_ = false, analog_selected_ = false;
    int dial_size_ = 0;
    lv_timer_t *timer_ = nullptr;
    lv_obj_t *digital_ = nullptr, *analog_ = nullptr, *date_ = nullptr;
    lv_obj_t *digits_ = nullptr, *seconds_ = nullptr, *unsynced_ = nullptr;
    lv_obj_t *hands_[3] = {};
    lv_point_precise_t hand_points_[3][2] = {};
};

ESP_UTILS_REGISTER_PLUGIN(systems::base::App, Timer, kName)
} // namespace esp_brookesia::apps
