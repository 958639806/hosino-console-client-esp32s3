/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 * Phone-system adaptation of the official release/v0.6 Calculator. See README.md.
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "esp_brookesia.hpp"
#include "hosino_ui_fonts.h"
#include "calculator_logic.hpp"

LV_IMG_DECLARE(img_app_calculator);

namespace esp_brookesia::apps {
namespace {
constexpr size_t kMaxExpression = 96;
constexpr size_t kMaxHistory = 384;
constexpr char kName[] = "计算器";
const char *const kKeys[] = {
    "C", "/", "x", LV_SYMBOL_BACKSPACE, "\n",
    "7", "8", "9", "-", "\n",
    "4", "5", "6", "+", "\n",
    "1", "2", "3", "%", "\n",
    "0", ".", "=", ""
};
bool digit(char c) { return c >= '0' && c <= '9'; }
bool operation(char c) { return c == '+' || c == '-' || c == 'x' || c == '/'; }

lv_obj_t *label(lv_obj_t *parent, const char *value, int x, int y, int width,
                const lv_font_t *font, uint32_t color)
{
    auto obj = lv_label_create(parent);
    lv_label_set_text(obj, value);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    return obj;
}
} // namespace

class Calculator : public systems::phone::App {
public:
    Calculator() : App(kName, &img_app_calculator, true, true, false) {}
    bool init() override { return true; }
    bool deinit() override { return true; }

    bool run() override
    {
        const int width = lv_area_get_width(&getVisualArea());
        const int usable = lv_area_get_height(&getVisualArea()) - 68;
        if (width < 240 || usable < 280) return false;
        auto screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0b1424), 0);
        lv_obj_set_style_pad_all(screen, 0, 0);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        label(screen, kName, 44, 10, width - 88, &hosino_font_20, 0xf2f5fc);
        history_ = label(screen, "", 44, 41, width - 88, &hosino_font_14, 0x94a8c8);
        formula_ = label(screen, "0", 44, 64, width - 88, &lv_font_montserrat_20, 0xf2f5fc);
        result_ = label(screen, "= 0", 44, 92, width - 88, &hosino_font_18, 0x75c9ef);
        lv_obj_set_style_text_align(formula_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_align(result_, LV_TEXT_ALIGN_RIGHT, 0);
        keyboard_ = lv_buttonmatrix_create(screen);
        lv_obj_set_pos(keyboard_, 40, 125);
        lv_obj_set_size(keyboard_, width - 80, usable - 125);
        lv_buttonmatrix_set_map(keyboard_, kKeys);
        lv_obj_set_style_bg_color(keyboard_, lv_color_hex(0x0b1424), 0);
        lv_obj_set_style_border_width(keyboard_, 0, 0);
        lv_obj_set_style_pad_all(keyboard_, 0, 0);
        lv_obj_set_style_pad_row(keyboard_, 6, 0);
        lv_obj_set_style_pad_column(keyboard_, 6, 0);
        lv_obj_set_style_bg_color(keyboard_, lv_color_hex(0x253951), LV_PART_ITEMS);
        lv_obj_set_style_text_color(keyboard_, lv_color_hex(0xf2f5fc), LV_PART_ITEMS);
        lv_obj_set_style_text_font(keyboard_, &lv_font_montserrat_20, LV_PART_ITEMS);
        lv_obj_set_style_border_width(keyboard_, 0, LV_PART_ITEMS);
        lv_obj_set_style_shadow_width(keyboard_, 0, LV_PART_ITEMS);
        lv_obj_set_style_radius(keyboard_, 8, LV_PART_ITEMS);
        lv_obj_add_event_cb(keyboard_, keyboard_event_cb, LV_EVENT_VALUE_CHANGED, this);
        expression_ = "0";
        history_text_.clear();
        foreground_ = true;
        return true;
    }

    bool back() override { return notifyCoreClosed(); }
    bool pause() override { foreground_ = false; return true; }
    bool resume() override { foreground_ = true; return true; }
    bool close() override { return pause(); }
    bool cleanResource() override
    {
        foreground_ = false;
        keyboard_ = history_ = formula_ = result_ = nullptr;
        expression_.clear();
        history_text_.clear();
        // Brookesia deletes the collected default screen and all of its children.
        return true;
    }

private:
    void key(uint32_t id)
    {
        if (!foreground_ || id >= 19) return;
        bool equals = id == 18;
        if (id == 0) {
            expression_ = "0";
            history_text_.clear();
            lv_label_set_text(history_, "");
        } else if (id == 3) {
            if (!expression_.empty()) expression_.pop_back();
            if (expression_.empty()) expression_ = "0";
        } else if (!equals) {
            if (expression_.size() >= kMaxExpression) {
                lv_label_set_text(result_, "输入已满");
                return;
            }
            const char c = lv_buttonmatrix_get_button_text(keyboard_, id)[0];
            const char last = expression_.back();
            if (digit(c)) {
                if (last == '%') return;
                // Replace a leading zero in the current operand, never in a decimal.
                const size_t start = expression_.find_last_of("+-x/");
                const size_t operand = start == std::string::npos ? 0 : start + 1;
                if (expression_.size() == operand + 1 && last == '0') expression_.pop_back();
                expression_ += c;
            } else if (c == '.') {
                if (last == '%') return;
                const size_t start = expression_.find_last_of("+-x/");
                const size_t operand = start == std::string::npos ? 0 : start + 1;
                if (expression_.find('.', operand) != std::string::npos) return;
                if (!digit(last)) expression_ += '0';
                expression_ += '.';
            } else if (c == '%') {
                if (!digit(last)) return;
                expression_ += '%';
            } else if (operation(c)) {
                if (expression_ == "0" && c == '-') expression_ = "-";
                else if (operation(last)) expression_.back() = c;
                else if (digit(last) || last == '%') expression_ += c;
            }
        }

        lv_label_set_text(formula_, expression_.c_str());
        if (expression_.empty() || operation(expression_.back()) || expression_.back() == '.') {
            lv_label_set_text(result_, "");
            return;
        }
        const double value = calculator::calculate(expression_.c_str());
        if (!std::isfinite(value) || std::fabs(value) >= 1e15) {
            lv_label_set_text(result_, "计算错误");
            return;
        }
        char output[64];
        // Fixed-point output remains valid input to the official decimal parser.
        // Six fractional digits are deliberately bounded for this small display.
        std::snprintf(output, sizeof(output), "%.6f", value);
        char *end = output + std::strlen(output) - 1;
        while (end > output && *end == '0') *end-- = '\0';
        if (*end == '.') *end = '\0';
        if (std::strcmp(output, "-0") == 0) std::strcpy(output, "0");
        lv_label_set_text_fmt(result_, "= %s", output);
        if (equals) {
            const std::string entry = expression_ + " = " + output;
            if (history_text_.size() + entry.size() + 1 > kMaxHistory) history_text_.clear();
            history_text_ += entry + '\n';
            // Show the most recent result; stored history is strictly bounded.
            lv_label_set_text(history_, entry.c_str());
            expression_ = output;
            lv_label_set_text(formula_, output);
        }
    }
    static void keyboard_event_cb(lv_event_t *event)
    {
        auto app = static_cast<Calculator *>(lv_event_get_user_data(event));
        if (app && app->keyboard_) app->key(lv_buttonmatrix_get_selected_button(app->keyboard_));
    }
    bool foreground_ = false;
    std::string expression_, history_text_;
    lv_obj_t *keyboard_ = nullptr, *history_ = nullptr, *formula_ = nullptr, *result_ = nullptr;
};

ESP_UTILS_REGISTER_PLUGIN(systems::base::App, Calculator, kName)
} // namespace esp_brookesia::apps
