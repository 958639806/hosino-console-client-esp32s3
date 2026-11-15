/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"
#include "ble_settings.h"

namespace esp_brookesia::apps {

class BluetoothPage : public systems::phone::App {
public:
    static BluetoothPage *requestInstance(bool use_status_bar = false,
                                          bool use_navigation_bar = false);
    BluetoothPage(bool use_status_bar, bool use_navigation_bar);
    ~BluetoothPage() override;
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool resume() override;

private:
    static BluetoothPage *_instance;
    static void switchEvent(lv_event_t *event);
    static void scanEvent(lv_event_t *event);
    static void timerCallback(lv_timer_t *timer);
    void refresh();
    void destroyUi();

    lv_obj_t *page_root = nullptr;
    lv_obj_t *list = nullptr;
    lv_obj_t *toggle = nullptr;
    lv_obj_t *scan_button = nullptr;
    lv_obj_t *scan_label = nullptr;
    lv_obj_t *status_label = nullptr;
    lv_timer_t *timer = nullptr;
    lv_obj_t *device_rows[BLE_SETTINGS_MAX_DEVICES] = {};
    lv_obj_t *device_names[BLE_SETTINGS_MAX_DEVICES] = {};
    lv_obj_t *device_details[BLE_SETTINGS_MAX_DEVICES] = {};
    uint8_t shown_devices = 0;
    uint32_t shown_revision = 0;
    bool updating = false;
    ble_settings_snapshot_t snapshot = {};
    lv_style_t style_list = {};
    lv_style_t style_row = {};
    lv_style_t style_section = {};
    lv_style_t style_pressed = {};
};

} // namespace esp_brookesia::apps
