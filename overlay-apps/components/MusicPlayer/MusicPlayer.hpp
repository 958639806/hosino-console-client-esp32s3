/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <atomic>
#include <cstdint>
#include "file_iterator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class MusicPlayer: public systems::phone::App {
public:
    enum class PlaybackState : uint8_t { Idle, Opening, Playing, Paused, Stopped, Finished, Error };
    struct PlaybackSnapshot {
        PlaybackState state = PlaybackState::Idle;
        uint64_t position_ms = 0;
        uint64_t duration_ms = 0;
        esp_err_t error = ESP_OK;
        bool busy = false;
    };

    static MusicPlayer *requestInstance(bool use_status_bar = true, bool use_navigation_bar = false);
    ~MusicPlayer();
    esp_err_t requestTrack(uint32_t index, bool allow_resume);
    esp_err_t requestPause();
    PlaybackSnapshot getPlaybackSnapshot();

protected:
    MusicPlayer(bool use_status_bar, bool use_navigation_bar);

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;
    bool deinit(void) override;
    bool pause(void) override;
    bool resume(void) override;
    bool cleanResource(void) override;

private:
    static MusicPlayer *_instance;

    file_iterator_instance_t *_file_iterator;
    bool _demo_created;
    lv_obj_t *_view = nullptr;

    enum class CommandType : uint8_t { None, Play, Pause, Resume };
    struct Command {
        CommandType type = CommandType::None;
        uint32_t generation = 0;
        uint32_t track = 0;
        char path[512] = {};
    };
    portMUX_TYPE _playback_lock = portMUX_INITIALIZER_UNLOCKED;
    PlaybackSnapshot _playback;
    Command _pending;
    TaskHandle_t _worker = nullptr;
    StaticSemaphore_t _command_event_buffer;
    SemaphoreHandle_t _command_event = nullptr;
    std::atomic<uint32_t> _generation{0};
    std::atomic_bool _foreground{false};
    std::atomic_bool _stop_requested{false};
    std::atomic_bool _worker_exit{false};
    uint32_t _playing_track = UINT32_MAX;

    bool ensureWorker();
    void requestStop();
    esp_err_t enqueue(const Command &command);
    static void workerTask(void *argument);
};

} // namespace esp_brookesia::apps
