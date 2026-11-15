/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 * Modified from Waveshare Gravitysphere for the 2.16-inch Chinese desktop.
 * All sensor I/O and teardown run outside LVGL; see SOURCE.md.
 */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <new>

#include "hosino_gravity_app.hpp"
#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hosino_ui_fonts.h"
// The pinned Waveshare header defines M_PI without checking math.h.
#ifdef M_PI
#undef M_PI
#endif
#include "qmi8658.h"

LV_IMAGE_DECLARE(img_app_qmi8658ball);

namespace esp_brookesia::apps {
namespace {
constexpr char kTag[] = "gravity_app";
constexpr int kBallRadius = 22;
constexpr int kBallMargin = kBallRadius + 18;
constexpr int kSensorPeriodMs = 30;
constexpr int kCalibrationSamples = 200;
constexpr int kCalibrationAttempts = 5;
constexpr float kDeadzone = 0.05F;
constexpr float kMovementScale = 8.0F;

enum class SensorMode { Starting, Calibrating, Ready, ReadyUnsteady, Paused, Missing, ReadError, NoMemory };
struct MotionSample { float x, y; uint32_t epoch; };
// Serializes this app's closing/reopening workers, never the shared I2C bus.
std::atomic<bool> sensor_owned{false};
}

struct GravityWorkerState {
    QueueHandle_t samples = xQueueCreate(1, sizeof(MotionSample));
    std::atomic<bool> stop{false};
    std::atomic<bool> active{true};
    std::atomic<uint32_t> epoch{1};
    std::atomic<uint32_t> recalibrate{1};
    std::atomic<SensorMode> mode{SensorMode::Starting};
    std::atomic<int> progress{0};
    std::atomic<int> attempt{1};

    ~GravityWorkerState() { if (samples) vQueueDelete(samples); }
};

namespace {
bool active(const GravityWorkerState &state)
{
    return !state.stop.load() && state.active.load();
}

void waitForRetry(GravityWorkerState &state, uint32_t request)
{
    while (active(state) && state.recalibrate.load() == request) {
        vTaskDelay(pdMS_TO_TICKS(kSensorPeriodMs));
    }
}

void releaseDevice(qmi8658_dev_t &imu, bool identified)
{
    if (!imu.dev_handle) return;
    // Failed init retains its I2C device handle in qmi8658 2.0.1. Only write
    // sensor controls after WHO_AM_I has confirmed that this is a QMI8658.
    if (!identified) {
        uint8_t who = 0;
        identified = qmi8658_get_who_am_i(&imu, &who) == ESP_OK && who == 0x05;
    }
    if (identified) (void)qmi8658_enable_sensors(&imu, QMI8658_DISABLE_ALL);
    const esp_err_t result = i2c_master_bus_rm_device(imu.dev_handle);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Remove private sensor handle failed: %s", esp_err_to_name(result));
    }
    imu = {};
}

bool initializeDevice(qmi8658_dev_t &imu, GravityWorkerState &state)
{
    const i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus || !active(state)) return false;
    uint8_t address = 0;
    for (const uint8_t candidate : {QMI8658_ADDRESS_HIGH, QMI8658_ADDRESS_LOW}) {
        if (!active(state)) return false;
        if (i2c_master_probe(bus, candidate, 100) == ESP_OK) {
            address = candidate;
            break;
        }
    }
    if (!address) return false;
    esp_err_t result = qmi8658_init(&imu, bus, address);
    if (result == ESP_OK && active(state)) result = qmi8658_enable_sensors(&imu, QMI8658_DISABLE_ALL);
    if (result == ESP_OK && active(state)) result = qmi8658_set_accel_range(&imu, QMI8658_ACCEL_RANGE_8G);
    // Match the 2.16-inch Immersive_block sensor configuration, not its display setup.
    if (result == ESP_OK && active(state)) result = qmi8658_set_accel_odr(&imu, QMI8658_ACCEL_ODR_500HZ);
    if (result == ESP_OK && active(state)) result = qmi8658_write_register(&imu, QMI8658_CTRL5, 0x03);
    qmi8658_set_accel_unit_mps2(&imu, true);
    if (result != ESP_OK) ESP_LOGW(kTag, "Sensor initialization failed: %s", esp_err_to_name(result));
    return result == ESP_OK && active(state);
}

enum class CalibrationResult { Cancelled, Failed, Stable, Unsteady };

CalibrationResult calibrate(qmi8658_dev_t &imu, GravityWorkerState &state,
                            uint32_t request, float &bias_x, float &bias_y)
{
    state.mode.store(SensorMode::Calibrating);
    bool have_samples = false;
    for (int attempt = 1; attempt <= kCalibrationAttempts; ++attempt) {
        float sum_x = 0, sum_y = 0;
        float min_x = INFINITY, min_y = INFINITY;
        float max_x = -INFINITY, max_y = -INFINITY;
        int count = 0, errors = 0;
        state.attempt.store(attempt);
        state.progress.store(0);
        for (int sample = 0; sample < kCalibrationSamples; ++sample) {
            if (!active(state) || state.recalibrate.load() != request) return CalibrationResult::Cancelled;
            float x = 0, y = 0, z = 0;
            const esp_err_t result = qmi8658_read_accel(&imu, &x, &y, &z);
            if (result == ESP_OK && std::isfinite(x) && std::isfinite(y)) {
                sum_x += x;
                sum_y += y;
                min_x = std::min(min_x, x); max_x = std::max(max_x, x);
                min_y = std::min(min_y, y); max_y = std::max(max_y, y);
                ++count;
                errors = 0;
            } else if (++errors >= 3) {
                return CalibrationResult::Failed;
            }
            state.progress.store((sample + 1) * 100 / kCalibrationSamples);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!active(state) || state.recalibrate.load() != request) return CalibrationResult::Cancelled;
        // Do not accept a nominal calibration based on just a few successful reads.
        if (count < kCalibrationSamples * 9 / 10) return CalibrationResult::Failed;
        bias_x = sum_x / count;
        bias_y = sum_y / count;
        have_samples = true;
        if (max_x - min_x <= 0.1F && max_y - min_y <= 0.1F) return CalibrationResult::Stable;
    }
    // Preserve the donor's last-real-samples fallback and disclose it in the UI.
    return have_samples ? CalibrationResult::Unsteady : CalibrationResult::Failed;
}

void workerLoop(GravityWorkerState &state)
{
    bool acquired = false;
    while (!state.stop.load()) {
        bool expected = false;
        if (sensor_owned.compare_exchange_strong(expected, true)) { acquired = true; break; }
        vTaskDelay(pdMS_TO_TICKS(kSensorPeriodMs));
    }
    if (!acquired) return;

    qmi8658_dev_t imu{};
    bool initialized = false, enabled = false, calibrated = false, unsteady = false;
    uint32_t completed_request = 0;
    float bias_x = 0, bias_y = 0;
    int failures = 0;
    while (!state.stop.load()) {
        if (!state.active.load()) {
            if (enabled) {
                (void)qmi8658_enable_sensors(&imu, QMI8658_DISABLE_ALL);
                enabled = false;
            }
            state.mode.store(SensorMode::Paused);
            vTaskDelay(pdMS_TO_TICKS(kSensorPeriodMs));
            continue;
        }
        if (!initialized) {
            const uint32_t request = state.recalibrate.load();
            state.mode.store(SensorMode::Starting);
            initialized = initializeDevice(imu, state);
            if (!initialized) {
                releaseDevice(imu, false);
                if (!active(state)) continue;
                state.mode.store(SensorMode::Missing);
                waitForRetry(state, request);
                continue;
            }
        }
        if (!active(state)) continue;
        if (!enabled) {
            const uint32_t request = state.recalibrate.load();
            if (qmi8658_enable_sensors(&imu, QMI8658_ENABLE_ACCEL) != ESP_OK) {
                state.mode.store(SensorMode::ReadError);
                waitForRetry(state, request);
                continue;
            }
            enabled = true;
        }
        const uint32_t request = state.recalibrate.load();
        if (!calibrated || request != completed_request) {
            const auto result = calibrate(imu, state, request, bias_x, bias_y);
            if (result == CalibrationResult::Cancelled) { calibrated = false; continue; }
            if (result == CalibrationResult::Failed) {
                calibrated = false;
                state.mode.store(SensorMode::ReadError);
                waitForRetry(state, request);
                continue;
            }
            calibrated = true;
            unsteady = result == CalibrationResult::Unsteady;
            completed_request = request;
            ESP_LOGI(kTag, "Real sensor calibration complete: X %.4f Y %.4f, stable=%d",
                     bias_x, bias_y, !unsteady);
        }
        if (!active(state)) continue;
        const uint32_t epoch = state.epoch.load();
        float x = 0, y = 0, z = 0;
        const esp_err_t result = qmi8658_read_accel(&imu, &x, &y, &z);
        if (!active(state) || state.epoch.load() != epoch) continue;
        if (result == ESP_OK && std::isfinite(x) && std::isfinite(y)) {
            failures = 0;
            x -= bias_x; y -= bias_y;
            if (std::fabs(x) < kDeadzone) x = 0;
            if (std::fabs(y) < kDeadzone) y = 0;
            const MotionSample sample{x, y, epoch};
            xQueueOverwrite(state.samples, &sample);
            state.mode.store(unsteady ? SensorMode::ReadyUnsteady : SensorMode::Ready);
        } else if (++failures >= 3) {
            state.mode.store(SensorMode::ReadError);
            calibrated = false;
            waitForRetry(state, request);
        }
        vTaskDelay(pdMS_TO_TICKS(kSensorPeriodMs));
    }
    releaseDevice(imu, initialized);
    sensor_owned.store(false);
}

void workerEntry(void *argument)
{
    // The worker owns its state independently from the app. A closed screen or
    // destroyed App cannot be accessed even during the driver's 1000 ms timeout.
    {
        auto *holder = static_cast<std::shared_ptr<GravityWorkerState> *>(argument);
        std::shared_ptr<GravityWorkerState> state = *holder;
        delete holder;
        workerLoop(*state);
    } // Release the last state/queue reference before vTaskDelete (no C++ unwind).
    vTaskDelete(nullptr);
}

lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xe7ecf5), 0);
    return label;
}
}

HosinoGravityApp::HosinoGravityApp() : App("重力小球", &img_app_qmi8658ball, true, true, false) {}
HosinoGravityApp::~HosinoGravityApp() { detachWorker(); }

bool HosinoGravityApp::run()
{
    foreground_ = true;
    playing_ = true;
    displayed_mode_ = displayed_progress_ = displayed_attempt_ = -1;
    const int width = lv_area_get_width(&getVisualArea());
    const int height = lv_area_get_height(&getVisualArea());
    arena_width_ = width - 24;
    arena_height_ = height - 84 - 68;
    if (arena_width_ <= 2 * kBallMargin || arena_height_ <= 2 * kBallMargin) return false;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0b1020), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(makeLabel(screen, "重力小球", &hosino_font_20), 16, 12);
    lv_obj_t *pause = lv_button_create(screen);
    lv_obj_set_pos(pause, width - 210, 8);
    lv_obj_set_size(pause, 92, 36);
    pause_label_ = makeLabel(pause, "暂停", &hosino_font_16);
    lv_obj_center(pause_label_);
    lv_obj_add_event_cb(pause, pauseCallback, LV_EVENT_CLICKED, this);
    lv_obj_t *calibration = lv_button_create(screen);
    lv_obj_set_pos(calibration, width - 110, 8);
    lv_obj_set_size(calibration, 94, 36);
    lv_obj_center(makeLabel(calibration, "校准", &hosino_font_16));
    lv_obj_add_event_cb(calibration, calibrateCallback, LV_EVENT_CLICKED, this);
    status_ = makeLabel(screen, "正在连接运动传感器", &hosino_font_14);
    lv_obj_set_pos(status_, 16, 52);
    lv_obj_set_width(status_, width - 32);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_MODE_CLIP);

    arena_ = lv_obj_create(screen);
    lv_obj_remove_style_all(arena_);
    lv_obj_set_pos(arena_, 12, 84);
    lv_obj_set_size(arena_, arena_width_, arena_height_);
    lv_obj_set_style_radius(arena_, 5, 0);
    lv_obj_set_style_border_width(arena_, 1, 0);
    lv_obj_set_style_border_color(arena_, lv_color_hex(0x52627f), 0);
    lv_obj_set_style_border_opa(arena_, LV_OPA_30, 0);
    lv_obj_remove_flag(arena_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    ball_ = lv_obj_create(arena_);
    lv_obj_remove_style_all(ball_);
    lv_obj_set_size(ball_, kBallRadius * 2, kBallRadius * 2);
    lv_obj_set_style_radius(ball_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ball_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ball_, lv_color_hex(0xff4f70), 0);
    lv_obj_set_style_bg_grad_color(ball_, lv_color_hex(0xffb14e), 0);
    lv_obj_set_style_bg_grad_dir(ball_, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_color(ball_, lv_color_hex(0xff4f70), 0);
    lv_obj_set_style_shadow_width(ball_, 18, 0);
    lv_obj_set_style_shadow_opa(ball_, LV_OPA_50, 0);
    lv_obj_remove_flag(ball_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(ball_, LV_OBJ_FLAG_HIDDEN);
    centerBall();
    hint_ = makeLabel(arena_, "请保持设备水平", &hosino_font_18);
    lv_obj_set_width(hint_, arena_width_ - 48);
    lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint_, LV_ALIGN_CENTER, 0, 12);
    progress_ = lv_bar_create(arena_);
    lv_obj_set_size(progress_, std::min(280, arena_width_ - 60), 12);
    lv_obj_align(progress_, LV_ALIGN_CENTER, 0, -24);
    lv_bar_set_range(progress_, 0, 100);
    lv_obj_add_flag(progress_, LV_OBJ_FLAG_HIDDEN);

    // Created during run(), so Brookesia records and owns this timer.
    timer_ = lv_timer_create(timerCallback, 33, this);
    if (!timer_) return false;
    state_ = std::make_shared<GravityWorkerState>();
    if (!state_->samples) {
        state_->mode.store(SensorMode::NoMemory);
    } else {
        auto *holder = new (std::nothrow) std::shared_ptr<GravityWorkerState>(state_);
        if (!holder || xTaskCreate(workerEntry, "gravity_sensor", 6144, holder, 3, nullptr) != pdPASS) {
            delete holder;
            state_->mode.store(SensorMode::NoMemory);
        }
    }
    updateUi();
    return true;
}

bool HosinoGravityApp::back() { return notifyCoreClosed(); }
bool HosinoGravityApp::pause()
{
    foreground_ = false;
    if (timer_) lv_timer_pause(timer_);
    if (state_) { state_->active.store(false); state_->epoch.fetch_add(1); }
    return true;
}
bool HosinoGravityApp::resume()
{
    foreground_ = true;
    if (state_) { state_->epoch.fetch_add(1); state_->active.store(playing_); }
    if (timer_) { lv_timer_reset(timer_); lv_timer_resume(timer_); }
    return true;
}
void HosinoGravityApp::detachWorker()
{
    if (!state_) return;
    state_->active.store(false);
    state_->stop.store(true);
    state_.reset();
}
bool HosinoGravityApp::close() { pause(); detachWorker(); return true; }
bool HosinoGravityApp::deinit() { detachWorker(); return true; }
bool HosinoGravityApp::cleanResource()
{
    detachWorker();
    // Never delete the recorded timer or screens a second time.
    timer_ = nullptr;
    arena_ = ball_ = status_ = hint_ = progress_ = pause_label_ = nullptr;
    foreground_ = false;
    return true;
}
void HosinoGravityApp::centerBall()
{
    ball_x_ = arena_width_ / 2;
    ball_y_ = arena_height_ / 2;
    if (ball_) lv_obj_set_pos(ball_, ball_x_ - kBallRadius, ball_y_ - kBallRadius);
}
void HosinoGravityApp::setPlaying(bool playing)
{
    playing_ = playing;
    if (state_) { state_->epoch.fetch_add(1); state_->active.store(foreground_ && playing_); }
    if (pause_label_) lv_label_set_text(pause_label_, playing_ ? "暂停" : "继续");
    updateUi();
}
void HosinoGravityApp::requestCalibration()
{
    if (!state_) return;
    centerBall();
    state_->epoch.fetch_add(1);
    state_->progress.store(0);
    state_->attempt.store(1);
    state_->recalibrate.fetch_add(1);
    if (state_->mode.load() != SensorMode::NoMemory) state_->mode.store(SensorMode::Calibrating);
    setPlaying(true);
}

void HosinoGravityApp::updateUi()
{
    if (!foreground_ || !state_ || !ball_) return;
    const auto mode = playing_ ? state_->mode.load() : SensorMode::Paused;
    const int progress = state_->progress.load();
    const int attempt = state_->attempt.load();
    const bool ready = mode == SensorMode::Ready || mode == SensorMode::ReadyUnsteady;
    if (displayed_mode_ != static_cast<int>(mode) || displayed_progress_ != progress || displayed_attempt_ != attempt) {
        if (ready || mode == SensorMode::Paused) lv_obj_remove_flag(ball_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ball_, LV_OBJ_FLAG_HIDDEN);
        if (ready) lv_obj_add_flag(hint_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(hint_, LV_OBJ_FLAG_HIDDEN);
        if (mode == SensorMode::Calibrating) {
            lv_obj_remove_flag(progress_, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(progress_, progress, LV_ANIM_OFF);
            lv_label_set_text_fmt(status_, "正在校准 %d%%  第%d次 / 共5次", progress, attempt);
            lv_label_set_text(hint_, "请保持设备水平");
        } else {
            lv_obj_add_flag(progress_, LV_OBJ_FLAG_HIDDEN);
            const char *text = "正在连接运动传感器";
            switch (mode) {
            case SensorMode::Ready: text = "倾斜设备控制小球 / 点击校准重新归零"; break;
            case SensorMode::ReadyUnsteady: text = "校准时有晃动 / 可放平设备重新校准"; break;
            case SensorMode::Paused: text = "已暂停 / 点击继续恢复"; break;
            case SensorMode::Missing: text = "运动传感器不可用 / 点击校准重试"; break;
            case SensorMode::ReadError: text = "传感器读取失败 / 点击校准重试"; break;
            case SensorMode::NoMemory: text = "内存不足 / 请关闭后重开"; break;
            default: break;
            }
            lv_label_set_text(status_, text);
            lv_label_set_text(hint_, text);
        }
        displayed_mode_ = static_cast<int>(mode);
        displayed_progress_ = progress;
        displayed_attempt_ = attempt;
    }
    if (ready && state_->samples) {
        MotionSample sample{};
        if (xQueueReceive(state_->samples, &sample, 0) == pdTRUE && sample.epoch == state_->epoch.load()) {
            // The 2.16-inch official example uses -X for horizontal and +Y for vertical.
            const int x = std::clamp(ball_x_ - static_cast<int>(sample.x * kMovementScale),
                                     kBallMargin, arena_width_ - kBallMargin);
            const int y = std::clamp(ball_y_ + static_cast<int>(sample.y * kMovementScale),
                                     kBallMargin, arena_height_ - kBallMargin);
            if (x != ball_x_ || y != ball_y_) {
                ball_x_ = x; ball_y_ = y;
                lv_obj_set_pos(ball_, x - kBallRadius, y - kBallRadius);
            }
        }
    }
}

void HosinoGravityApp::timerCallback(lv_timer_t *timer)
{
    static_cast<HosinoGravityApp *>(lv_timer_get_user_data(timer))->updateUi();
}
void HosinoGravityApp::pauseCallback(lv_event_t *event)
{
    auto *app = static_cast<HosinoGravityApp *>(lv_event_get_user_data(event));
    app->setPlaying(!app->playing_);
}
void HosinoGravityApp::calibrateCallback(lv_event_t *event)
{
    static_cast<HosinoGravityApp *>(lv_event_get_user_data(event))->requestCalibration();
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, HosinoGravityApp, "HosinoGravity", []() {
    return std::make_shared<HosinoGravityApp>();
})
} // namespace esp_brookesia::apps
