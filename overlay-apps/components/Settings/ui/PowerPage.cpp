/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "PowerPage.hpp"

#include "../Settings.hpp"
#include "SettingsUI.hpp"
#include "system_status.hpp"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:PowerPage"
#include "esp_lib_utils.h"

LV_IMG_DECLARE(battery);

namespace esp_brookesia::apps {

namespace {

enum TelemetryRow {
    INPUT_PRESENT, BATTERY_VOLTAGE, INPUT_VOLTAGE, SYSTEM_VOLTAGE, DIE_TEMPERATURE,
    CHARGER_ENABLED, TARGET_VOLTAGE, CHARGE_CURRENT, INPUT_VOLTAGE_LIMIT, INPUT_CURRENT_LIMIT,
    TELEMETRY_ROW_COUNT,
};

void setNumericValue(lv_obj_t *label, int value, const char *unit)
{
    if (value < 0) {
        lv_label_set_text(label, "不可用");
    } else {
        lv_label_set_text_fmt(label, "%d %s", value, unit);
    }
}

void setAdcVoltage(lv_obj_t *label, const brookesia::system_status::PmuTelemetry &pmu,
                   unsigned channel_bit, int millivolts)
{
    if (pmu.adc_config_valid && !(pmu.adc_channels & (1U << channel_bit))) {
        lv_label_set_text(label, "ADC 未启用");
    } else {
        setNumericValue(label, millivolts, "mV");
    }
}

lv_obj_t *getValueLabel(lv_obj_t *row)
{
    if (row == nullptr) {
        return nullptr;
    }
    const uint32_t child_count = lv_obj_get_child_count(row);
    return child_count > 0 ? lv_obj_get_child(row, static_cast<int32_t>(child_count - 1)) : nullptr;
}

const char *chargeStateText(const brookesia::system_status::Snapshot &snapshot)
{
    if (!snapshot.battery_present) {
        return "未连接电池";
    }

    switch (snapshot.battery_power_state) {
    case brookesia::system_status::BatteryPowerState::CHARGING:
        switch (snapshot.charger_status) {
        case 1:
            return "预充电";
        case 2:
            return "恒流充电";
        case 3:
            return "恒压充电";
        default:
            return "充电中";
        }
    case brookesia::system_status::BatteryPowerState::DISCHARGING:
        return "放电中";
    case brookesia::system_status::BatteryPowerState::STANDBY:
        return snapshot.charger_status == 4 ? "已充满" : "待机";
    case brookesia::system_status::BatteryPowerState::UNKNOWN:
    default:
        return snapshot.charger_status == 4 ? "已充满" : "未知";
    }
}

} // namespace

PowerPage *PowerPage::_instance = nullptr;

PowerPage *PowerPage::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new PowerPage(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

PowerPage::PowerPage(bool use_status_bar, bool use_navigation_bar)
    : App("电源", nullptr, true, use_status_bar, use_navigation_bar)
{
}

PowerPage::~PowerPage()
{
}

bool PowerPage::run()
{
    ESP_UTILS_LOGD("PowerPage Run");
    if (page_root != nullptr) {
        close();
    }
    lv_obj_clean(lv_screen_active());

    page_root = settings_ui::create_page(lv_screen_active());
    settings_ui::create_header(page_root, "电源与网络", [](lv_event_t *event) {
        (void)event;
        Settings::requestInstance()->requestRootPage();
    });

    settings_ui::init_list_styles(style_list, style_row, style_section, style_pressed);
    list = settings_ui::create_content_list(page_root);
    lv_obj_add_style(list, &style_list, LV_PART_MAIN);

    settings_ui::add_section(list, "电池", style_section);
    percent_value = getValueLabel(
        settings_ui::add_info_row(list, &battery, "电量", "--", style_row)
    );
    charge_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "状态", "正在读取…", style_row)
    );
    present_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "电池连接", "未知", style_row)
    );

    settings_ui::add_section(list, "AXP2101 实时测量", style_section);
    const char *measurement_names[] = {"USB 输入", "电池电压", "输入电压", "系统电压", "PMU 温度"};
    for (unsigned i = 0; i < 5; ++i) {
        telemetry_values[i] = getValueLabel(
            settings_ui::add_info_row(list, nullptr, measurement_names[i], "不可用", style_row));
    }
    settings_ui::add_section(list, "充电配置（只读）", style_section);
    const char *config_names[] = {"充电开关", "目标电压", "恒流设定", "输入限压", "输入限流"};
    for (unsigned i = 0; i < 5; ++i) {
        telemetry_values[5 + i] = getValueLabel(
            settings_ui::add_info_row(list, nullptr, config_names[i], "不可用", style_row));
    }

    settings_ui::add_section(list, "网络", style_section);
    wifi_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "Wi-Fi", "未连接", style_row)
    );
    rssi_value = getValueLabel(
        settings_ui::add_info_row(list, nullptr, "信号强度", "--", style_row)
    );

    refreshValues();
    refresh_timer = lv_timer_create(refreshTimerCallback, 1000, this);
    return refresh_timer != nullptr;
}

bool PowerPage::back()
{
    ESP_UTILS_LOGD("PowerPage Back");
    Settings::requestInstance()->showRootPage();
    return true;
}

bool PowerPage::close()
{
    ESP_UTILS_LOGD("PowerPage Close");
    if (refresh_timer != nullptr) {
        lv_timer_delete(refresh_timer);
        refresh_timer = nullptr;
    }

    if (page_root != nullptr) {
        lv_obj_delete(page_root);
        page_root = nullptr;
        list = nullptr;
        percent_value = nullptr;
        charge_value = nullptr;
        present_value = nullptr;
        wifi_value = nullptr;
        rssi_value = nullptr;
        for (auto &value : telemetry_values) {
            value = nullptr;
        }
        settings_ui::reset_list_styles(style_list, style_row, style_section, style_pressed);
    }
    return true;
}

void PowerPage::refreshTimerCallback(lv_timer_t *timer)
{
    auto *page = static_cast<PowerPage *>(lv_timer_get_user_data(timer));
    if (page != nullptr) {
        page->refreshValues();
    }
}

void PowerPage::refreshValues()
{
    if (percent_value == nullptr || charge_value == nullptr || present_value == nullptr ||
            wifi_value == nullptr || rssi_value == nullptr) {
        return;
    }

    brookesia::system_status::Snapshot snapshot = {};
    if (!brookesia::system_status::get_snapshot(snapshot)) {
        lv_label_set_text(percent_value, "--");
        lv_label_set_text(charge_value, "暂不可用");
        lv_label_set_text(present_value, "未知");
        lv_label_set_text(wifi_value, "暂不可用");
        lv_label_set_text(rssi_value, "--");
        for (auto *value : telemetry_values) {
            lv_label_set_text(value, "不可用");
        }
        return;
    }

    const auto &pmu = snapshot.pmu;
    lv_label_set_text(telemetry_values[INPUT_PRESENT], pmu.status_valid ?
                      (pmu.vbus_present ? "已接入" : "未接入") : "不可用");
    setAdcVoltage(telemetry_values[BATTERY_VOLTAGE], pmu, 0, pmu.battery_voltage_mv);
    setAdcVoltage(telemetry_values[INPUT_VOLTAGE], pmu, 2, pmu.vbus_voltage_mv);
    setAdcVoltage(telemetry_values[SYSTEM_VOLTAGE], pmu, 3, pmu.system_voltage_mv);
    if (pmu.adc_config_valid && !(pmu.adc_channels & (1U << 4))) {
        lv_label_set_text(telemetry_values[DIE_TEMPERATURE], "ADC 未启用");
    } else if (pmu.temperature_valid) {
        // This is the PMU die temperature, not the battery or ESP32 temperature.
        const int tenths = pmu.die_temperature_tenths_c;
        const int magnitude = tenths < 0 ? -tenths : tenths;
        lv_label_set_text_fmt(telemetry_values[DIE_TEMPERATURE], "%s%d.%d C",
                              tenths < 0 ? "-" : "", magnitude / 10, magnitude % 10);
    } else {
        lv_label_set_text(telemetry_values[DIE_TEMPERATURE], "不可用");
    }
    lv_label_set_text(telemetry_values[CHARGER_ENABLED], pmu.charger_config_valid ?
                      (pmu.charger_enabled ? "已启用" : "已关闭") : "不可用");
    setNumericValue(telemetry_values[TARGET_VOLTAGE], pmu.charge_target_mv, "mV");
    setNumericValue(telemetry_values[CHARGE_CURRENT], pmu.charge_current_ma, "mA");
    setNumericValue(telemetry_values[INPUT_VOLTAGE_LIMIT], pmu.input_voltage_limit_mv, "mV");
    setNumericValue(telemetry_values[INPUT_CURRENT_LIMIT], pmu.input_current_limit_ma, "mA");

    if (!snapshot.battery_valid) {
        lv_label_set_text(percent_value, "--");
        lv_label_set_text(charge_value, "未知");
        lv_label_set_text(present_value, "未知");
    } else if (!snapshot.battery_present) {
        lv_label_set_text(percent_value, "--");
        lv_label_set_text(charge_value, "未连接电池");
        lv_label_set_text(present_value, "否");
    } else {
        lv_label_set_text_fmt(percent_value, "%d%%", snapshot.battery_percent);
        lv_label_set_text(charge_value, chargeStateText(snapshot));
        lv_label_set_text(present_value, "是");
    }

    if (!snapshot.wifi_enabled) {
        lv_label_set_text(wifi_value, "已关闭");
        lv_label_set_text(rssi_value, "--");
    } else if (snapshot.wifi_connected) {
        lv_label_set_text(wifi_value, "已连接");
        if (snapshot.wifi_rssi > -127) {
            lv_label_set_text_fmt(rssi_value, "%d dBm", snapshot.wifi_rssi);
        } else {
            lv_label_set_text(rssi_value, "正在获取…");
        }
    } else {
        lv_label_set_text(wifi_value, snapshot.wifi_reconnect_paused ? "已手动断开" : "未连接");
        lv_label_set_text(rssi_value, "--");
    }
}

} // namespace esp_brookesia::apps
