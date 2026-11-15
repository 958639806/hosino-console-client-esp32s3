/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "Settings.hpp"
#include "ui/SettingsUI.hpp"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "esp_event.h"
#include "esp_netif.h"
#include "system_status.hpp"
#include "esp_wifi.h"
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:Settings"
#include "esp_lib_utils.h"

// Settings page icons.
LV_IMG_DECLARE(WIFI);
LV_IMG_DECLARE(sound);
LV_IMG_DECLARE(info);
LV_IMG_DECLARE(BLE);
LV_IMG_DECLARE(backlights);
LV_IMG_DECLARE(battery);
LV_IMG_DECLARE(arrow);
LV_IMG_DECLARE(img_app_settings);

namespace esp_brookesia::apps
{
    Settings *Settings::_instance = nullptr;

    Settings *Settings::requestInstance(bool use_status_bar, bool use_navigation_bar)
    {
        if (_instance == nullptr)
        {
            _instance = new Settings(use_status_bar, use_navigation_bar);
        }
        return _instance;
    }

    Settings::Settings(bool use_status_bar, bool use_navigation_bar)
        : App("设置", &img_app_settings, true, use_status_bar, use_navigation_bar),
          page_root(nullptr), list1(nullptr), active_page(ActivePage::None)
    {
    }

    Settings::~Settings()
    {
        cancelNavigation();
    }

    void Settings::create_settings_ui()
    {
        if (!canNavigate()) {
            return;
        }
        destroy_settings_ui();
        lv_obj_clean(_app_screen);

        page_root = settings_ui::create_page(_app_screen);
        settings_ui::create_header(page_root, "设置");
        settings_ui::init_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);

        list1 = settings_ui::create_content_list(page_root);
        lv_obj_add_style(list1, &style_list, LV_PART_MAIN);

        settings_ui::add_section(list1, "无线连接", style_list_text);
        add_button_with_arrow(&WIFI, "无线网络");
        add_button_with_arrow(&BLE, "蓝牙");

        settings_ui::add_section(list1, "显示与声音", style_list_text);
        add_button_with_arrow(&sound, "声音");
        add_button_with_arrow(&backlights, "显示");

        settings_ui::add_section(list1, "系统", style_list_text);
        add_button_with_arrow(&battery, "电源");
        add_button_with_arrow(LV_SYMBOL_SD_CARD, "存储");

        settings_ui::add_section(list1, "更多", style_list_text);
        add_button_with_arrow(&info, "关于");
    }

    void Settings::destroy_settings_ui()
    {
        if (page_root != nullptr)
        {
            lv_obj_del(page_root);
            page_root = nullptr;
            list1 = nullptr;
            settings_ui::reset_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
        }
    }

    void Settings::add_button_with_arrow(const void *icon, const char *text)
    {
        lv_obj_t *btn = lv_list_add_button(list1, icon, text);
        lv_obj_add_style(btn, &style_list_btn, LV_PART_MAIN);
        lv_obj_add_style(btn, &style_list_btn_pressed, LV_STATE_PRESSED);
        settings_ui::use_ellipsis_for_button_label(btn);
        lv_obj_add_event_cb(btn, event_handler_cb, LV_EVENT_CLICKED, this);

        lv_obj_t *arrow_img = lv_image_create(btn);
        lv_image_set_src(arrow_img, &arrow);
    }

    void Settings::event_handler_cb(lv_event_t *e)
    {
        Settings *instance = static_cast<Settings *>(lv_event_get_user_data(e));
        if (instance != nullptr)
        {
            instance->event_handler(e);
        }
    }

    bool Settings::canNavigate() const
    {
        return _foreground && _app_screen != nullptr &&
               lv_obj_is_valid(_app_screen) && lv_screen_active() == _app_screen;
    }

    void Settings::cancelNavigation()
    {
        (void)lv_async_call_cancel(open_page_async_cb, this);
        (void)lv_async_call_cancel(show_root_async_cb, this);
        _pending_page = nullptr;
        _pending_root = false;
    }

    void Settings::requestPage(const char *page_name)
    {
        if (!canNavigate() || page_name == nullptr || _pending_page != nullptr || _pending_root) {
            return;
        }
        // Callers pass static route literals, never a label's soon-to-be-deleted text buffer.
        _pending_page = page_name;
        if (lv_async_call(open_page_async_cb, this) != LV_RESULT_OK) {
            _pending_page = nullptr;
        }
    }

    void Settings::requestRootPage()
    {
        if (!canNavigate()) {
            return;
        }
        cancelNavigation();
        _pending_root = true;
        if (lv_async_call(show_root_async_cb, this) != LV_RESULT_OK) {
            _pending_root = false;
        }
    }

    void Settings::open_page_async_cb(void *user_data)
    {
        auto *instance = static_cast<Settings *>(user_data);
        if (instance == nullptr) {
            return;
        }
        const char *page_name = instance->_pending_page;
        instance->_pending_page = nullptr;
        if (instance->canNavigate() && page_name != nullptr) {
            instance->open_page(page_name);
        }
    }

    void Settings::show_root_async_cb(void *user_data)
    {
        auto *instance = static_cast<Settings *>(user_data);
        if (instance == nullptr) {
            return;
        }
        const bool requested = instance->_pending_root;
        instance->_pending_root = false;
        if (requested && instance->canNavigate()) {
            instance->showRootPage();
        }
    }

    void Settings::event_handler(lv_event_t *e)
    {
        if (!canNavigate() || list1 == nullptr || lv_event_get_code(e) != LV_EVENT_CLICKED) {
            return;
        }

        lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_target(e));
        const char *txt = lv_list_get_button_text(list1, obj);
        if (txt == nullptr) {
            return;
        }
        ESP_UTILS_LOGI("Clicked: %s", txt);

        if (strcmp(txt, "无线网络") == 0)
        {
            requestPage("无线网络");
        }
        else if (strcmp(txt, "蓝牙") == 0)
        {
            requestPage("蓝牙");
        }
        else if (strcmp(txt, "声音") == 0)
        {
            requestPage("声音");
        }
        else if (strcmp(txt, "显示") == 0)
        {
            requestPage("显示");
        }
        else if (strcmp(txt, "电源") == 0)
        {
            requestPage("电源");
        }
        else if (strcmp(txt, "存储") == 0)
        {
            requestPage("存储");
        }
        else if (strcmp(txt, "关于") == 0)
        {
            requestPage("关于");
        }
    }

    void Settings::open_page(const char *page_name)
    {
        if (!canNavigate() || page_name == nullptr) {
            return;
        }
        cancelNavigation();
        close_active_page();
        destroy_settings_ui();

        if (strcmp(page_name, "无线网络") == 0)
        {
            active_page = ActivePage::Wlan;
            WlanPage::requestInstance(false, false)->run();
        }
        else if (strcmp(page_name, "蓝牙") == 0)
        {
            active_page = ActivePage::Bluetooth;
            BluetoothPage::requestInstance(false, false)->run();
        }
        else if (strcmp(page_name, "声音") == 0)
        {
            active_page = ActivePage::Sound;
            SoundPage::requestInstance(false, false)->run();
        }
        else if (strcmp(page_name, "显示") == 0)
        {
            active_page = ActivePage::Display;
            DisplayPage::requestInstance(false, false)->run();
        }
        else if (strcmp(page_name, "电源") == 0)
        {
            active_page = ActivePage::Power;
            PowerPage::requestInstance(false, false)->run();
        }
        else if (strcmp(page_name, "存储") == 0)
        {
            active_page = ActivePage::Storage;
            StoragePage::requestInstance(false, false)->run();
        }
        else if (strcmp(page_name, "关于") == 0)
        {
            active_page = ActivePage::About;
            AboutPage::requestInstance(false, false)->run();
        }
    }

    void Settings::close_active_page()
    {
        switch (active_page)
        {
        case ActivePage::Wlan:
            WlanPage::requestInstance(false, false)->close();
            break;
        case ActivePage::Bluetooth:
            BluetoothPage::requestInstance(false, false)->close();
            break;
        case ActivePage::Sound:
            SoundPage::requestInstance(false, false)->close();
            break;
        case ActivePage::Display:
            DisplayPage::requestInstance(false, false)->close();
            break;
        case ActivePage::Power:
            PowerPage::requestInstance(false, false)->close();
            break;
        case ActivePage::Storage:
            StoragePage::requestInstance(false, false)->close();
            break;
        case ActivePage::About:
            AboutPage::requestInstance(false, false)->close();
            break;
        case ActivePage::None:
        default:
            break;
        }
        active_page = ActivePage::None;
    }

    void Settings::showRootPage()
    {
        if (!canNavigate()) {
            return;
        }
        cancelNavigation();
        close_active_page();
        create_settings_ui();
    }

    bool Settings::run(void)
    {
        ESP_UTILS_LOGD("Run");
        cancelNavigation();
        _app_screen = lv_screen_active();
        _foreground = true;
        active_page = ActivePage::None;
        if (page_root == nullptr)
        {
            create_settings_ui();
        }
        return true;
    }

    bool Settings::back(void)
    {
        ESP_UTILS_LOGD("Back");
        if (active_page != ActivePage::None)
        {
            showRootPage();
            return true;
        }
        return notifyCoreClosed();
    }

    bool Settings::close(void)
    {
        ESP_UTILS_LOGD("Close");
        _foreground = false;
        cancelNavigation();
        close_active_page();
        destroy_settings_ui();
        _app_screen = nullptr;
        return true;
    }

    bool Settings::init()
    {
        ESP_UTILS_LOGD("Init");
        return initWifi() == ESP_OK;
    }

    bool Settings::deinit()
    {
        ESP_UTILS_LOGD("Deinit");
        _foreground = false;
        cancelNavigation();
        return true;
    }

    bool Settings::pause()
    {
        ESP_UTILS_LOGD("Pause");
        _foreground = false;
        cancelNavigation();
        if (active_page == ActivePage::Bluetooth) BluetoothPage::requestInstance()->pause();
        return true;
    }

    bool Settings::resume()
    {
        ESP_UTILS_LOGD("Resume");
        _foreground = _app_screen != nullptr && lv_screen_active() == _app_screen;
        if (_foreground && active_page == ActivePage::Bluetooth) BluetoothPage::requestInstance()->resume();
        return _foreground;
    }

    esp_err_t Settings::initWifi()
    {
        return brookesia::system_status::init_wifi_stack();
    }

} // namespace esp_brookesia::apps
