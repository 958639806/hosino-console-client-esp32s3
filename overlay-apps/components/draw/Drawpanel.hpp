/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

// HOST_DRAW_STATE_BEGIN
typedef struct {
    lv_obj_t *canvas;
    lv_point_t previous;
    bool active;
} DrawpanelStroke;
// HOST_DRAW_STATE_END

/**
 * @brief Drawpanel application for touch-based drawing on the device screen
 *
 */
class Drawpanel: public systems::phone::App {
public:
    /**
     * @brief Get the singleton instance of Drawpanel
     *
     * @param use_status_bar Show status bar
     * @param use_navigation_bar Show navigation bar
     * @return Drawpanel* Singleton instance pointer
     */
    static Drawpanel *requestInstance(bool use_status_bar = true, bool use_navigation_bar = false);

    /**
     * @brief Destroy the Drawpanel object
     *
     */
    ~Drawpanel();

    using systems::phone::App::startRecordResource;
    using systems::phone::App::endRecordResource;

protected:
    /**
     * @brief Construct a new Drawpanel object (private to enforce singleton)
     *
     * @param use_status_bar Show status bar
     * @param use_navigation_bar Show navigation bar
     */
    Drawpanel(bool use_status_bar, bool use_navigation_bar);

    bool run(void) override;
    bool back(void) override;

    bool close(void) override;
    bool init(void) override;
    bool deinit(void) override;
    bool pause(void) override;
    bool resume(void) override;
    // bool cleanResource(void) override;

private:
    static Drawpanel *_instance;

    /**
     * @brief Touch callback for a bounded canvas stroke
     *
     * @param e LVGL event data
     */

    lv_obj_t *_panel_obj = nullptr;
    lv_obj_t *_clear_button = nullptr;
    void *_pixels = nullptr;
    DrawpanelStroke _stroke{};
    void clearAllPoints();
    static void touch_event_cb(lv_event_t *e);
};

} // namespace esp_brookesia::apps
