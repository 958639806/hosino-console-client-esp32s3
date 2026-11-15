// SPDX-License-Identifier: MIT
#include "sdkconfig.h"
#if defined(CONFIG_HOSINO_DISPLAY_BUFFERED) && CONFIG_HOSINO_DISPLAY_BUFFERED

#include <array>
#include <cstdint>
#include <memory>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_brookesia.hpp"
#include "portengine.h"

// Supplied by main; a link-time dependency avoids a component cycle with main.
extern "C" void hosino_perf_overlay_get_snapshot(uint32_t *completed_frames, int64_t *time_us);

namespace esp_brookesia::apps {
namespace {
constexpr int kIconSize = 64;
constexpr uint16_t color565(uint32_t c)
{
    return static_cast<uint16_t>(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
}
constexpr std::array<uint16_t, kIconSize * kIconSize> make_icon()
{
    std::array<uint16_t, kIconSize * kIconSize> pixels{};
    for (int y = 0; y < kIconSize; ++y) {
        for (int x = 0; x < kIconSize; ++x) {
            uint32_t c = 0x6937ba;
            if ((x >= 11 && x < 20 && y >= 36 && y < 50) ||
                (x >= 27 && x < 36 && y >= 25 && y < 50) ||
                (x >= 43 && x < 52 && y >= 13 && y < 50)) c = 0xf1eaff;
            if (y >= 53 && y < 56 && x >= 10 && x < 54) c = 0xffc664;
            pixels[y * kIconSize + x] = color565(c);
        }
    }
    return pixels;
}
alignas(4) constexpr auto kIconPixels = make_icon();
const lv_image_dsc_t kIcon = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
               .flags = 0, .w = kIconSize, .h = kIconSize, .stride = kIconSize * 2},
    .data_size = sizeof(kIconPixels),
    .data = reinterpret_cast<const uint8_t *>(kIconPixels.data()),
};

void box(lv_obj_t *obj, uint32_t color)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *label(lv_obj_t *parent, const char *value, int x, int y, const lv_font_t *font = &lv_font_montserrat_14)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, value);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xeaf0fa), 0);
    lv_obj_set_pos(obj, x, y);
    return obj;
}

struct SceneResult {
    uint32_t frames = 0;
    int64_t elapsed_us = 0;
};

class LvglBench : public systems::phone::App {
public:
    LvglBench() : App("LVGL Bench", &kIcon, true, true, false) {}

private:
    bool run() override
    {
        results_ = {};
        scene_ = 0;
        foreground_ = true;
        playing_ = true;
        finished_ = false;
        measuring_ = false;
        screen_ = lv_screen_active();
        const int width = lv_area_get_width(&getVisualArea());
        const int height = lv_area_get_height(&getVisualArea());
        box(screen_, 0x111725);
        label(screen_, "LVGL Bench", 16, 10, &lv_font_montserrat_20);
        scene_label_ = label(screen_, "", 16, 43);
        lv_obj_set_width(scene_label_, width - 32);
        lv_label_set_long_mode(scene_label_, LV_LABEL_LONG_MODE_CLIP);

        lv_obj_t *button = lv_button_create(screen_);
        lv_obj_set_pos(button, width - 110, 8);
        lv_obj_set_size(button, 94, 32);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x603caa), 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        button_label_ = lv_label_create(button);
        lv_label_set_text(button_label_, "Pause");
        lv_obj_center(button_label_);
        lv_obj_add_event_cb(button, button_cb, LV_EVENT_CLICKED, this);

        viewport_ = lv_obj_create(screen_);
        box(viewport_, 0xe5e7eb);
        lv_obj_set_pos(viewport_, 8, 66);
        lv_obj_set_size(viewport_, width - 16, height > 170 ? height - 132 : 80);
        label(screen_, "15 official scenes | completed UI FPS", 16, height - 60);
        lv_obj_update_layout(viewport_);

        // This one timer is recorded by Brookesia. Reuse it for every scene/run;
        // never delete it here or create replacement timers outside run().
        timer_ = lv_timer_create(timer_cb, 100, this);
        if (!timer_) return false;
        start_scene();
        return true;
    }

    bool back() override { return notifyCoreClosed(); }

    bool pause() override
    {
        foreground_ = false;
        if (timer_) lv_timer_pause(timer_);
        interrupt_scene();
        return true;
    }

    bool resume() override
    {
        foreground_ = true;
        if (playing_ && !finished_) start_scene();
        return true;
    }

    bool close() override
    {
        ESP_LOGI("lvgl_bench", "Closing app; stack_min_free=%u bytes",
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        return pause();
    }

    bool cleanResource() override
    {
        if (timer_) lv_timer_pause(timer_);
        interrupt_scene();
        // Brookesia deletes the recorded timer/default screen after this hook.
        timer_ = nullptr;
        viewport_ = nullptr;
        screen_ = nullptr;
        scene_label_ = nullptr;
        button_label_ = nullptr;
        foreground_ = false;
        playing_ = false;
        finished_ = false;
        results_ = {};
        return true;
    }

    void interrupt_scene()
    {
        measuring_ = false;
        hosino_lvgl_benchmark_scene_stop();
    }

    void start_scene()
    {
        if (!foreground_ || !playing_ || finished_ || !viewport_ || !timer_) return;
        hosino_lvgl_benchmark_scene_stop();
        lv_obj_clean(viewport_);
        if (!hosino_lvgl_benchmark_scene_start(viewport_, scene_)) {
            playing_ = false;
            lv_timer_pause(timer_);
            lv_label_set_text(scene_label_, "Scene could not start");
            lv_label_set_text(button_label_, "Run");
            return;
        }
        lv_label_set_text_fmt(scene_label_, "%02u/15  %s", scene_ + 1,
                              hosino_lvgl_benchmark_scene_name(scene_));
        lv_label_set_text(button_label_, "Pause");
        hosino_perf_overlay_get_snapshot(&start_frames_, &start_time_us_);
        measuring_ = true;
        lv_timer_reset(timer_);
        lv_timer_resume(timer_);
        ESP_LOGI("lvgl_bench", "Starting scene %u: %s; stack_min_free=%u bytes", scene_ + 1,
                 hosino_lvgl_benchmark_scene_name(scene_),
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }

    void tick()
    {
        if (!foreground_ || !playing_ || !measuring_ || finished_) return;
        uint32_t frames;
        int64_t now;
        hosino_perf_overlay_get_snapshot(&frames, &now);
        const int64_t elapsed = now - start_time_us_;
        if (elapsed < static_cast<int64_t>(hosino_lvgl_benchmark_scene_duration_ms(scene_)) * 1000) return;
        results_[scene_] = {frames - start_frames_, elapsed};
        ESP_LOGI("lvgl_bench", "Scene %u complete: frames=%lu elapsed_us=%lld stack_min_free=%u bytes",
                 scene_ + 1, (unsigned long)results_[scene_].frames,
                 (long long)elapsed, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        interrupt_scene();
        ++scene_;
        if (scene_ == HOSINO_LVGL_BENCHMARK_SCENE_COUNT) show_results();
        else start_scene();
    }

    void show_results()
    {
        ESP_LOGI("lvgl_bench", "All 15 scenes completed; creating results page");
        finished_ = true;
        playing_ = false;
        measuring_ = false;
        lv_timer_pause(timer_);
        lv_label_set_text(button_label_, "Again");
        lv_label_set_text(scene_label_, "Complete | scroll to see all 15 results");
        lv_obj_clean(viewport_);
        lv_obj_add_flag(viewport_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(viewport_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(viewport_, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_bg_color(viewport_, lv_color_hex(0x111725), 0);

        uint64_t total_frames = 0;
        int64_t total_time = 0;
        for (const auto &result : results_) {
            total_frames += result.frames;
            total_time += result.elapsed_us;
        }
        const uint64_t avg_tenths = total_time > 0 ? total_frames * 10000000ULL / total_time : 0;
        lv_obj_t *overall = label(viewport_, "", 8, 5, &lv_font_montserrat_16);
        lv_label_set_text_fmt(overall, "Overall: %lu.%lu UI FPS", static_cast<unsigned long>(avg_tenths / 10),
                              static_cast<unsigned long>(avg_tenths % 10));
        label(viewport_, "Brookesia port; Widgets scene omitted", 8, 28);
        const int width = lv_obj_get_width(viewport_);
        lv_obj_t *dimensions = label(viewport_, "", 8, 45);
        lv_label_set_text_fmt(dimensions, "Viewport %d x %d | HUD included", width,
                              static_cast<int>(lv_obj_get_height(viewport_)));
        for (unsigned i = 0; i < results_.size(); ++i) {
            const auto &result = results_[i];
            const uint64_t fps_tenths = result.elapsed_us > 0 ?
                static_cast<uint64_t>(result.frames) * 10000000ULL / result.elapsed_us : 0;
            lv_obj_t *row = lv_obj_create(viewport_);
            box(row, i % 2 ? 0x1b2538 : 0x151e2e);
            lv_obj_set_pos(row, 0, 69 + i * 52);
            lv_obj_set_size(row, width - 8, 50);
            lv_obj_t *name = label(row, hosino_lvgl_benchmark_scene_name(i), 8, 7);
            lv_obj_set_width(name, width - 122);
            lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_t *value = label(row, "", width - 102, 14);
            lv_label_set_text_fmt(value, "%lu.%lu FPS", static_cast<unsigned long>(fps_tenths / 10),
                                  static_cast<unsigned long>(fps_tenths % 10));
        }
        lv_obj_scroll_to_y(viewport_, 0, LV_ANIM_OFF);
    }

    void button_pressed()
    {
        if (finished_) {
            results_ = {};
            scene_ = 0;
            finished_ = false;
            playing_ = true;
            lv_obj_remove_flag(viewport_, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_scroll_to_y(viewport_, 0, LV_ANIM_OFF);
            start_scene();
        } else if (playing_) {
            playing_ = false;
            lv_timer_pause(timer_);
            interrupt_scene();
            lv_label_set_text(button_label_, "Resume");
            lv_label_set_text(scene_label_, "Paused | resume repeats current scene");
        } else {
            playing_ = true;
            start_scene();
        }
    }

    static void timer_cb(lv_timer_t *timer)
    {
        static_cast<LvglBench *>(lv_timer_get_user_data(timer))->tick();
    }
    static void button_cb(lv_event_t *event)
    {
        static_cast<LvglBench *>(lv_event_get_user_data(event))->button_pressed();
    }

    lv_obj_t *screen_ = nullptr;
    lv_obj_t *viewport_ = nullptr;
    lv_obj_t *scene_label_ = nullptr;
    lv_obj_t *button_label_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::array<SceneResult, HOSINO_LVGL_BENCHMARK_SCENE_COUNT> results_{};
    unsigned scene_ = 0;
    uint32_t start_frames_ = 0;
    int64_t start_time_us_ = 0;
    bool foreground_ = false;
    bool playing_ = false;
    bool measuring_ = false;
    bool finished_ = false;
};
} // namespace

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, LvglBench, "LVGL Bench", []()
{
    return std::make_shared<LvglBench>();
})
} // namespace esp_brookesia::apps
#endif
