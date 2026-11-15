// SPDX-License-Identifier: Apache-2.0
#include "sdkconfig.h"
#if defined(CONFIG_HOSINO_DISPLAY_CORNERS) && CONFIG_HOSINO_DISPLAY_CORNERS

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include "esp_brookesia.hpp"
#include "../../main/hosino_corner_clip.h"

namespace esp_brookesia::apps {
namespace {
constexpr std::array<uint16_t, 64 * 64> corner_icon()
{
    std::array<uint16_t, 64 * 64> pixels{};
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            bool edge = ((x >= 10 && x < 15) || (x >= 49 && x < 54)) && y >= 10 && y < 54;
            edge |= ((y >= 10 && y < 15) || (y >= 49 && y < 54)) && x >= 10 && x < 54;
            bool corner = (x < 22 || x >= 42) && (y < 22 || y >= 42);
            pixels[y * 64 + x] = edge ? 0xffff : corner ? 0xf940 : 0x1949;
        }
    }
    return pixels;
}
alignas(4) constexpr auto kPixels = corner_icon();
const lv_image_dsc_t kIcon = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
               .flags = 0, .w = 64, .h = 64, .stride = 128},
    .data_size = sizeof(kPixels),
    .data = reinterpret_cast<const uint8_t *>(kPixels.data()),
};
}

// The candidate mask is only an optical calibration aid. While it is visible,
// the transmission crop MUST be disabled so it cannot hide its own errors.
class CornerCal : public systems::phone::App {
public:
    CornerCal() : App("Corner Cal", &kIcon, true, true, false) {}

protected:
    bool run() override
    {
        screen_ = lv_screen_active();
        display_ = lv_obj_get_display(screen_);
        candidate_ = hosino_corner_clip_get_radius();
        saved_enabled_ = hosino_corner_clip_is_enabled();
        preview_ = true;
        active_ = true;
        lv_obj_set_style_bg_color(screen_, lv_color_hex(0x091222), 0);
        lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
        label("Corner Cal", 24, 10, &lv_font_montserrat_20);
        label("Keep UI size / hide invisible corners only", 24, 39);
        state_ = label("", 24, 68, &lv_font_montserrat_18);
        label("If any RED is visible, reduce the radius.\n"
              "Enable only when ALL red is hidden.\n"
              "Leave a few pixels of safety margin.", 24, 101);
        button("-10", 28, 169, 92, 40, minus10);
        button("-2", 136, 169, 92, 40, minus2);
        button("+2", 244, 169, 92, 40, plus2);
        button("+10", 352, 169, 92, 40, plus10);
        button("Preview", 28, 223, 124, 42, preview_cb);
        button("Enable crop", 174, 223, 134, 42, apply_cb);
        button("Crop OFF", 330, 223, 122, 42, off_cb);
        status_ = label("", 24, 278);
        label("Settings last until reboot. Home gesture still works.", 24, 322);
        hosino_corner_clip_set_enabled(false);
        create_mask();
        update_labels();
        timer_ = lv_timer_create(timer_cb, 1000, this);
        return timer_ != nullptr;
    }

    bool back() override { return notifyCoreClosed(); }

    bool pause() override
    {
        if (!active_) return true;
        active_ = false;
        if (timer_) lv_timer_pause(timer_);
        destroy_mask();
        if (preview_) hosino_corner_clip_set_enabled(saved_enabled_);
        return true;
    }

    bool resume() override
    {
        if (active_) return true;
        active_ = true;
        if (preview_) {
            saved_enabled_ = hosino_corner_clip_is_enabled();
            hosino_corner_clip_set_enabled(false);
            create_mask();
        }
        if (timer_) lv_timer_resume(timer_);
        update_labels();
        return true;
    }

    bool close() override { return pause(); }

    bool cleanResource() override
    {
        pause();
        destroy_mask();
        // Brookesia owns the recorded app screen and timer. The system-layer
        // child belongs to this class and must be removed explicitly.
        timer_ = nullptr;
        screen_ = state_ = status_ = nullptr;
        display_ = nullptr;
        return true;
    }

private:
    lv_obj_t *label(const char *text, int x, int y,
                    const lv_font_t *font = &lv_font_montserrat_14)
    {
        lv_obj_t *obj = lv_label_create(screen_);
        lv_label_set_text(obj, text);
        lv_obj_set_style_text_color(obj, lv_color_hex(0xeaf0fa), 0);
        lv_obj_set_style_text_font(obj, font, 0);
        lv_obj_set_pos(obj, x, y);
        return obj;
    }

    void button(const char *text, int x, int y, int w, int h, lv_event_cb_t cb)
    {
        lv_obj_t *obj = lv_button_create(screen_);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_size(obj, w, h);
        lv_obj_set_style_bg_color(obj, lv_color_hex(0x253a60), 0);
        lv_obj_set_style_shadow_width(obj, 0, 0);
        lv_obj_t *text_obj = lv_label_create(obj);
        lv_label_set_text(text_obj, text);
        lv_obj_center(text_obj);
        lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, this);
    }

    void create_mask()
    {
        if (mask_ || !display_) return;
        mask_ = lv_obj_create(lv_display_get_layer_sys(display_));
        lv_obj_remove_style_all(mask_);
        lv_obj_set_pos(mask_, 0, 0);
        lv_obj_set_size(mask_, 480, 480);
        lv_obj_remove_flag(mask_, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                                  LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_flag(mask_, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
        lv_obj_add_event_cb(mask_, draw_mask_cb, LV_EVENT_DRAW_MAIN, this);
        lv_obj_move_foreground(mask_);
    }

    void destroy_mask()
    {
        if (mask_) {
            lv_obj_delete(mask_);
            mask_ = nullptr;
        }
    }

    void start_preview()
    {
        if (!preview_) saved_enabled_ = hosino_corner_clip_is_enabled();
        preview_ = true;
        hosino_corner_clip_set_enabled(false);
        create_mask();
        if (mask_) lv_obj_invalidate(mask_);
        update_labels();
    }

    void adjust(int delta)
    {
        start_preview();
        candidate_ = std::clamp(candidate_ + delta, 0, 240);
        if (mask_) lv_obj_invalidate(mask_);
        update_labels();
    }

    void apply()
    {
        destroy_mask();
        preview_ = false;
        hosino_corner_clip_set_radius(candidate_);
        hosino_corner_clip_reset_stats();
        hosino_corner_clip_set_enabled(candidate_ > 0);
        saved_enabled_ = hosino_corner_clip_is_enabled();
        update_labels();
    }

    void turn_off()
    {
        destroy_mask();
        preview_ = false;
        saved_enabled_ = false;
        hosino_corner_clip_set_enabled(false);
        hosino_corner_clip_reset_stats();
        update_labels();
    }

    void update_labels()
    {
        if (!state_ || !status_) return;
        char buf[180];
        lv_snprintf(buf, sizeof(buf), "Radius %d px | %s", candidate_,
                    preview_ ? "PREVIEW / crop OFF" : hosino_corner_clip_is_enabled() ? "crop ON" : "crop OFF");
        lv_label_set_text(state_, buf);
        hosino_corner_clip_stats_t stats{};
        hosino_corner_clip_get_stats(&stats);
        if (stats.failures > 0) {
            lv_label_set_text(status_, "Display transfer error: turn crop OFF.\n"
                                       "Byte saving is unavailable after an error.");
            return;
        }
        const uint32_t saved = stats.requested_bytes > 0 && stats.sent_bytes <= stats.requested_bytes
            ? static_cast<uint32_t>((stats.requested_bytes - stats.sent_bytes) * 1000 / stats.requested_bytes) : 0;
        lv_snprintf(buf, sizeof(buf), "Submitted pixel bytes saved: %u.%u%%\n"
                    "Compare Scroll Test with crop ON / OFF.", saved / 10, saved % 10);
        lv_label_set_text(status_, buf);
    }

    static CornerCal *self(lv_event_t *e) { return static_cast<CornerCal *>(lv_event_get_user_data(e)); }
    static void minus10(lv_event_t *e) { self(e)->adjust(-10); }
    static void minus2(lv_event_t *e) { self(e)->adjust(-2); }
    static void plus2(lv_event_t *e) { self(e)->adjust(2); }
    static void plus10(lv_event_t *e) { self(e)->adjust(10); }
    static void preview_cb(lv_event_t *e) { self(e)->start_preview(); }
    static void apply_cb(lv_event_t *e) { self(e)->apply(); }
    static void off_cb(lv_event_t *e) { self(e)->turn_off(); }
    static void timer_cb(lv_timer_t *timer)
    {
        auto *app = static_cast<CornerCal *>(lv_timer_get_user_data(timer));
        if (app->active_) app->update_labels();
    }

    static void draw_mask_cb(lv_event_t *e)
    {
        auto *app = self(e);
        lv_layer_t *layer = lv_event_get_layer(e);
        lv_area_t origin;
        lv_obj_get_coords(app->mask_, &origin);
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(0xff0000);
        d.bg_opa = LV_OPA_COVER;
        // Merge identical neighbouring rows. This overlay uses exactly the
        // output mask, so every pixel it proposes omitting is shown in red.
        for (int y = 0; y < 480;) {
            const int inset = hosino_corner_clip_row_inset(y, app->candidate_);
            int end = y + 1;
            while (end < 480 && hosino_corner_clip_row_inset(end, app->candidate_) == inset) ++end;
            if (inset > 0) {
                lv_area_t left = {origin.x1, origin.y1 + y, origin.x1 + inset - 1, origin.y1 + end - 1};
                lv_area_t right = {origin.x1 + 480 - inset, origin.y1 + y, origin.x1 + 479, origin.y1 + end - 1};
                lv_draw_rect(layer, &d, &left);
                lv_draw_rect(layer, &d, &right);
            }
            y = end;
        }
    }

    lv_obj_t *screen_ = nullptr;
    lv_obj_t *state_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *mask_ = nullptr;
    lv_display_t *display_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    int candidate_ = 0;
    bool preview_ = false;
    bool active_ = false;
    bool saved_enabled_ = false;
};

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, CornerCal, "Corner Cal", []() {
    return std::make_shared<CornerCal>();
})
} // namespace esp_brookesia::apps
#endif
