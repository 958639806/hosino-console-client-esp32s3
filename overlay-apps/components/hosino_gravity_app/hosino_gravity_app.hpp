/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 * Modified for the ESP32-S3-Touch-AMOLED-2.16 Chinese desktop; see SOURCE.md.
 */
#pragma once

#include <memory>
#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {
struct GravityWorkerState;

class HosinoGravityApp : public systems::phone::App {
public:
    HosinoGravityApp();
    ~HosinoGravityApp() override;

protected:
    bool run() override;
    bool back() override;
    bool pause() override;
    bool resume() override;
    bool close() override;
    bool cleanResource() override;
    bool deinit() override;

private:
    static void timerCallback(lv_timer_t *timer);
    static void pauseCallback(lv_event_t *event);
    static void calibrateCallback(lv_event_t *event);
    void updateUi();
    void setPlaying(bool playing);
    void requestCalibration();
    void detachWorker();
    void centerBall();

    std::shared_ptr<GravityWorkerState> state_;
    lv_timer_t *timer_ = nullptr;
    lv_obj_t *arena_ = nullptr;
    lv_obj_t *ball_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_obj_t *progress_ = nullptr;
    lv_obj_t *pause_label_ = nullptr;
    int arena_width_ = 0;
    int arena_height_ = 0;
    int ball_x_ = 0;
    int ball_y_ = 0;
    int displayed_mode_ = -1;
    int displayed_progress_ = -1;
    int displayed_attempt_ = -1;
    bool foreground_ = false;
    bool playing_ = true;
};
} // namespace esp_brookesia::apps
