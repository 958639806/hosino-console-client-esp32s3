/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 * SPDX-License-Identifier: Apache-2.0
 */
#include "AboutPage.hpp"
#include "SettingsUI.hpp"
#include "../Settings.hpp"

#include <cinttypes>
#include <cstdio>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "bsp/esp-bsp.h"

#define ESP_UTILS_LOG_TAG "BS:AboutPage"
#include "esp_lib_utils.h"

namespace esp_brookesia::apps {
namespace {
lv_obj_t *addValue(lv_obj_t *list, const char *name, const char *value, lv_style_t &style)
{
    return settings_ui::add_detail_row(list, name, value, style);
}
}

AboutPage *AboutPage::_instance = nullptr;

AboutPage *AboutPage::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new AboutPage(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

AboutPage::AboutPage(bool use_status_bar, bool use_navigation_bar)
    : App("关于", nullptr, true, use_status_bar, use_navigation_bar)
{
}

AboutPage::~AboutPage() = default;

bool AboutPage::run()
{
    if (page_root != nullptr) {
        close();
    }
    lv_obj_clean(lv_screen_active());
    page_root = settings_ui::create_page(lv_screen_active());
    settings_ui::create_header(page_root, "关于设备", [](lv_event_t *) {
        Settings::requestInstance()->requestRootPage();
    });
    settings_ui::init_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
    list1 = settings_ui::create_content_list(page_root);
    lv_obj_add_style(list1, &style_list, LV_PART_MAIN);

    settings_ui::add_section(list1, "设备", style_list_text);
    addValue(list1, "制造商", "Waveshare", style_list_btn);
    addValue(list1, "型号", "ESP32-S3-Touch-AMOLED-2.16", style_list_btn);
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    char text[96];
    std::snprintf(text, sizeof(text), "ESP32-S3 / rev %u", static_cast<unsigned>(chip.revision));
    addValue(list1, "芯片", text, style_list_btn);
    std::snprintf(text, sizeof(text), "%u 核", static_cast<unsigned>(chip.cores));
    addValue(list1, "CPU 核数", text, style_list_btn);
    cpu_value = addValue(list1, "CPU 频率", "正在读取…", style_list_btn);
    uint32_t flash_size = 0;
    if (esp_flash_get_physical_size(nullptr, &flash_size) == ESP_OK) {
        std::snprintf(text, sizeof(text), "%.1f MiB", flash_size / 1048576.0);
    } else {
        std::snprintf(text, sizeof(text), "不可用");
    }
    addValue(list1, "Flash 容量", text, style_list_btn);
    if (esp_psram_is_initialized()) {
        std::snprintf(text, sizeof(text), "%.1f MiB", esp_psram_get_size() / 1048576.0);
    } else {
        std::snprintf(text, sizeof(text), "未初始化");
    }
    addValue(list1, "PSRAM 容量", text, style_list_btn);
    std::snprintf(text, sizeof(text), "%d x %d", BSP_LCD_H_RES, BSP_LCD_V_RES);
    addValue(list1, "屏幕分辨率", text, style_list_btn);

    settings_ui::add_section(list1, "软件", style_list_text);
    const auto *app = esp_app_get_description();
    addValue(list1, "固件版本", app->version, style_list_btn);
    addValue(list1, "ESP-IDF", esp_get_idf_version(), style_list_btn);
    std::snprintf(text, sizeof(text), "%u.%u.%u%s", lv_version_major(), lv_version_minor(),
                  lv_version_patch(), lv_version_info());
    addValue(list1, "LVGL", text, style_list_btn);
#ifdef HOSINO_BROOKESIA_VERSION
    addValue(list1, "Brookesia", HOSINO_BROOKESIA_VERSION, style_list_btn);
#elif defined(BROOKESIA_CORE_VER_MAJOR)
    std::snprintf(text, sizeof(text), "%d.%d.%d", BROOKESIA_CORE_VER_MAJOR,
                  BROOKESIA_CORE_VER_MINOR, BROOKESIA_CORE_VER_PATCH);
    addValue(list1, "Brookesia", text, style_list_btn);
#else
    addValue(list1, "Brookesia", "版本不可用", style_list_btn);
#endif
    std::snprintf(text, sizeof(text), "%s %s", app->date, app->time);
    addValue(list1, "编译时间", text, style_list_btn);
    uptime_value = addValue(list1, "运行时间", "正在读取…", style_list_btn);
    refreshRuntime();
    refresh_timer = lv_timer_create(refreshTimerCallback, 1000, this);
    return refresh_timer != nullptr;
}

bool AboutPage::back()
{
    Settings::requestInstance()->showRootPage();
    return true;
}

bool AboutPage::close()
{
    if (refresh_timer != nullptr) {
        lv_timer_delete(refresh_timer);
        refresh_timer = nullptr;
    }
    if (page_root != nullptr) {
        lv_obj_delete(page_root);
        page_root = nullptr;
        uptime_value = nullptr;
        cpu_value = nullptr;
        list1 = nullptr;
        settings_ui::reset_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
    }
    return true;
}

void AboutPage::refreshTimerCallback(lv_timer_t *timer)
{
    auto *page = static_cast<AboutPage *>(lv_timer_get_user_data(timer));
    if (page != nullptr) {
        page->refreshRuntime();
    }
}

void AboutPage::refreshRuntime()
{
    if (uptime_value != nullptr) {
        const uint64_t seconds = static_cast<uint64_t>(esp_timer_get_time()) / 1000000;
        lv_label_set_text_fmt(uptime_value, "%" PRIu64 " 天 %02u:%02u:%02u", seconds / 86400,
            static_cast<unsigned>((seconds / 3600) % 24), static_cast<unsigned>((seconds / 60) % 60),
            static_cast<unsigned>(seconds % 60));
    }
    if (cpu_value != nullptr) {
        lv_label_set_text_fmt(cpu_value, "%d MHz", esp_clk_cpu_freq() / 1000000);
    }
}

} // namespace esp_brookesia::apps
