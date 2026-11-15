/* SPDX-License-Identifier: Apache-2.0 */
#include "BluetoothPage.hpp"

#include <cstdio>
#include <cstring>
#include "../Settings.hpp"
#include "SettingsUI.hpp"
#include "esp_log.h"
#include "hosino_ui_fonts.h"

namespace esp_brookesia::apps {

namespace {
void setText(lv_obj_t *label, const char *text)
{
    if (label != nullptr && std::strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

const char *stateText(ble_settings_state_t state)
{
    switch (state) {
    case BLE_SETTINGS_OFF: return "蓝牙已关闭";
    case BLE_SETTINGS_STARTING: return "正在启动蓝牙…";
    case BLE_SETTINGS_READY: return "蓝牙已开启";
    case BLE_SETTINGS_SCANNING: return "正在扫描，最长 10 秒…";
    case BLE_SETTINGS_STOPPING: return "正在关闭蓝牙…";
    case BLE_SETTINGS_ERROR: return "蓝牙操作失败";
    default: return "状态未知";
    }
}
} // namespace

BluetoothPage *BluetoothPage::_instance = nullptr;

BluetoothPage *BluetoothPage::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) _instance = new BluetoothPage(use_status_bar, use_navigation_bar);
    return _instance;
}

BluetoothPage::BluetoothPage(bool use_status_bar, bool use_navigation_bar)
    : App("蓝牙", nullptr, true, use_status_bar, use_navigation_bar)
{
}

BluetoothPage::~BluetoothPage()
{
    close();
}

bool BluetoothPage::run()
{
    destroyUi();
    page_root = settings_ui::create_page(lv_screen_active());
    settings_ui::create_header(page_root, "蓝牙", [](lv_event_t *) {
        Settings::requestInstance()->requestRootPage();
    });
    settings_ui::init_list_styles(style_list, style_row, style_section, style_pressed);
    list = settings_ui::create_content_list(page_root);
    lv_obj_add_style(list, &style_list, LV_PART_MAIN);
    /* New objects still have zero coordinates until LVGL runs layout. Reading
     * list's height here used to replace its requested height with -28, hiding
     * every control. Compute the safe region from the laid-out parent instead. */
    lv_obj_update_layout(page_root);
    const lv_coord_t content_top = settings_ui::get_header_top(page_root) +
        settings_ui::PAGE_HEADER_HEIGHT + settings_ui::CONTENT_TOP_GAP;
    const lv_coord_t content_height = lv_obj_get_height(page_root) - content_top - 68;
    lv_obj_set_height(list, content_height > 0 ? content_height : 1);

    lv_obj_t *switch_row = lv_list_add_button(list, nullptr, nullptr);
    lv_obj_add_style(switch_row, &style_row, LV_PART_MAIN);
    lv_obj_clear_flag(switch_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *switch_title = lv_label_create(switch_row);
    lv_label_set_text(switch_title, "低功耗蓝牙");
    lv_obj_set_flex_grow(switch_title, 1);
    toggle = lv_switch_create(switch_row);
    lv_obj_set_size(toggle, 58, 30);
    lv_obj_add_event_cb(toggle, switchEvent, LV_EVENT_VALUE_CHANGED, this);

    scan_button = lv_list_add_button(list, nullptr, "扫描附近设备（10 秒）");
    lv_obj_add_style(scan_button, &style_row, LV_PART_MAIN);
    lv_obj_add_style(scan_button, &style_pressed, LV_STATE_PRESSED);
    settings_ui::use_ellipsis_for_button_label(scan_button);
    scan_label = settings_ui::get_button_label(scan_button);
    lv_obj_add_event_cb(scan_button, scanEvent, LV_EVENT_CLICKED, this);

    status_label = lv_list_add_text(list, "蓝牙已关闭");
    lv_obj_add_style(status_label, &style_section, LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &hosino_font_16, LV_PART_MAIN);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *notice = lv_list_add_text(list,
        "本机支持 BLE 扫描，暂不配对或连接。\n"
        "ESP32-S3 不支持经典蓝牙耳机、音箱或网络共享。\n"
        "连接手机热点请使用无线网络。最多显示 20 台设备。");
    lv_obj_add_style(notice, &style_section, LV_PART_MAIN);
    lv_obj_set_style_text_font(notice, &hosino_font_14, LV_PART_MAIN);
    lv_label_set_long_mode(notice, LV_LABEL_LONG_MODE_WRAP);

    shown_revision = 0;
    refresh();
    timer = lv_timer_create(timerCallback, 300, this);
    lv_obj_update_layout(page_root);
    ESP_LOGI("bluetooth_ui", "page=%ldx%ld list=%ldx%ld switch=%ldx%ld timer=%s",
             static_cast<long>(lv_obj_get_width(page_root)),
             static_cast<long>(lv_obj_get_height(page_root)),
             static_cast<long>(lv_obj_get_width(list)),
             static_cast<long>(lv_obj_get_height(list)),
             static_cast<long>(lv_obj_get_width(toggle)),
             static_cast<long>(lv_obj_get_height(toggle)), timer != nullptr ? "ok" : "failed");
    return timer != nullptr;
}

bool BluetoothPage::back()
{
    Settings::requestInstance()->requestRootPage();
    return true;
}

bool BluetoothPage::close()
{
    ble_settings_cancel_scan();
    destroyUi();
    return true;
}

bool BluetoothPage::pause()
{
    ble_settings_cancel_scan();
    if (timer != nullptr) lv_timer_pause(timer);
    return true;
}

bool BluetoothPage::resume()
{
    if (timer != nullptr) lv_timer_resume(timer);
    shown_revision = 0;
    refresh();
    return page_root != nullptr;
}

void BluetoothPage::switchEvent(lv_event_t *event)
{
    auto *page = static_cast<BluetoothPage *>(lv_event_get_user_data(event));
    if (page->updating) return;
    const bool enabled = lv_obj_has_state(page->toggle, LV_STATE_CHECKED);
    (void)ble_settings_set_enabled(enabled);
    page->shown_revision = 0;
    page->refresh();
}

void BluetoothPage::scanEvent(lv_event_t *event)
{
    auto *page = static_cast<BluetoothPage *>(lv_event_get_user_data(event));
    if (page->snapshot.scanning) ble_settings_cancel_scan();
    else (void)ble_settings_start_scan();
    page->shown_revision = 0;
    page->refresh();
}

void BluetoothPage::timerCallback(lv_timer_t *timer)
{
    auto *page = static_cast<BluetoothPage *>(lv_timer_get_user_data(timer));
    page->refresh();
}

void BluetoothPage::refresh()
{
    if (page_root == nullptr) return;
    ble_settings_get_snapshot(&snapshot);
    if (shown_revision == snapshot.revision) return;
    shown_revision = snapshot.revision;
    updating = true;
    if (snapshot.requested_enabled) lv_obj_add_state(toggle, LV_STATE_CHECKED);
    else lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    const bool busy = snapshot.state == BLE_SETTINGS_STARTING ||
                      snapshot.state == BLE_SETTINGS_STOPPING;
    if (busy || snapshot.restart_required) lv_obj_add_state(toggle, LV_STATE_DISABLED);
    else lv_obj_remove_state(toggle, LV_STATE_DISABLED);
    if (snapshot.enabled && snapshot.requested_enabled && !busy && !snapshot.restart_required) {
        lv_obj_remove_state(scan_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(scan_button, LV_STATE_DISABLED);
    }
    setText(scan_label, snapshot.scanning ? "停止扫描" : "扫描附近设备（10 秒）");
    updating = false;

    char status[192];
    if (snapshot.last_error != 0) {
        std::snprintf(status, sizeof(status), "%s\n错误码：0x%X%s", stateText(snapshot.state),
                      static_cast<unsigned>(snapshot.last_error),
                      snapshot.restart_required ? "，请重启设备后重试" : "");
    } else {
        std::snprintf(status, sizeof(status), "%s\n已发现 %u / %u 台设备", stateText(snapshot.state),
                      static_cast<unsigned>(snapshot.count), BLE_SETTINGS_MAX_DEVICES);
    }
    setText(status_label, status);

    while (shown_devices > snapshot.count) {
        --shown_devices;
        lv_obj_delete(device_rows[shown_devices]);
        device_rows[shown_devices] = nullptr;
        device_names[shown_devices] = nullptr;
        device_details[shown_devices] = nullptr;
    }
    while (shown_devices < snapshot.count) {
        const unsigned i = shown_devices++;
        device_rows[i] = lv_obj_create(list);
        lv_obj_add_style(device_rows[i], &style_row, LV_PART_MAIN);
        lv_obj_set_width(device_rows[i], lv_pct(100));
        lv_obj_set_height(device_rows[i], 86);
        lv_obj_clear_flag(device_rows[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(device_rows[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(device_rows[i], 6, LV_PART_MAIN);
        device_names[i] = lv_label_create(device_rows[i]);
        lv_obj_set_width(device_names[i], lv_pct(100));
        lv_obj_set_style_text_font(device_names[i], &hosino_font_18, LV_PART_MAIN);
        lv_label_set_long_mode(device_names[i], LV_LABEL_LONG_MODE_DOTS);
        device_details[i] = lv_label_create(device_rows[i]);
        lv_obj_set_width(device_details[i], lv_pct(100));
        lv_obj_set_style_text_font(device_details[i], &hosino_font_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(device_details[i], lv_color_hex(settings_ui::COLOR_SECONDARY_TEXT),
                                    LV_PART_MAIN);
    }
    for (unsigned i = 0; i < snapshot.count; ++i) {
        const auto &device = snapshot.devices[i];
        setText(device_names[i], device.name[0] != '\0' ? device.name : "未命名设备");
        char details[96];
        const auto *a = device.address;
        if (device.rssi == 127) {
            std::snprintf(details, sizeof(details), "%02X:%02X:%02X:%02X:%02X:%02X\n信号未知",
                          a[5], a[4], a[3], a[2], a[1], a[0]);
        } else {
            std::snprintf(details, sizeof(details), "%02X:%02X:%02X:%02X:%02X:%02X\n信号 %d dBm",
                          a[5], a[4], a[3], a[2], a[1], a[0], static_cast<int>(device.rssi));
        }
        setText(device_details[i], details);
    }
}

void BluetoothPage::destroyUi()
{
    if (timer != nullptr) {
        lv_timer_delete(timer);
        timer = nullptr;
    }
    if (page_root != nullptr) {
        lv_obj_delete(page_root);
        page_root = nullptr;
        list = toggle = scan_button = scan_label = status_label = nullptr;
        std::memset(device_rows, 0, sizeof(device_rows));
        std::memset(device_names, 0, sizeof(device_names));
        std::memset(device_details, 0, sizeof(device_details));
        shown_devices = 0;
        settings_ui::reset_list_styles(style_list, style_row, style_section, style_pressed);
    }
}

} // namespace esp_brookesia::apps
