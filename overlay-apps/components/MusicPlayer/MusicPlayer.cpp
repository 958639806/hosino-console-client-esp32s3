/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "MusicPlayer.hpp"

#include <algorithm>
#include <cstring>
#include "freertos/idf_additions.h"
#include "hosino_media_player.h"
#include "storage_service.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "esp_brookesia.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gui_music/lv_demo_music.h"
#include "gui_music/lv_demo_music_main.h"
#include "lvgl.h"

#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:MusicPlayer"
#include "esp_lib_utils.h"

namespace {

const char *TAG = "MusicPlayer";
constexpr char SD_MUSIC_DIR[] = STORAGE_SERVICE_MOUNT_POINT "/music";
constexpr char SD_MUSIC_DIR_COMPAT[] = STORAGE_SERVICE_MOUNT_POINT "/Music";
constexpr char SD_SAMPLE_DIR[] = STORAGE_SERVICE_MOUNT_POINT "/hosino_samples";


size_t scan_music_directory(const char *path, const char *source,
                            file_iterator_instance_t **iterator)
{
    bsp_extra_file_instance_deinit(iterator);

    const esp_err_t result = bsp_extra_file_instance_init(path, iterator);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "%s music directory unavailable (%s): %s",
                 source, path, esp_err_to_name(result));
        bsp_extra_file_instance_deinit(iterator);
        return 0;
    }

    const size_t count = file_iterator_get_count(*iterator);
    if (count == 0) {
        ESP_LOGI(TAG, "%s music directory is empty: %s", source, path);
        bsp_extra_file_instance_deinit(iterator);
        return 0;
    }

    ESP_LOGI(TAG, "Using %s music library: %s (%u track(s))",
             source, path, static_cast<unsigned>(count));
    return count;
}


lv_obj_t *make_music_view(lv_obj_t *screen, const lv_area_t &area)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101323), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *view = lv_obj_create(screen);
    lv_obj_remove_style_all(view);
    lv_obj_set_size(view, lv_area_get_width(&area), lv_area_get_height(&area) - 68);
    lv_obj_set_pos(view, 0, 0);
    lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(view);
    return view;
}

void log_memory(const char *stage, size_t track_count)
{
    ESP_LOGI(
        TAG,
        "%s: tracks=%u, internal=%u/%u, psram=%u/%u, stack_hwm=%u",
        stage,
        static_cast<unsigned>(track_count),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr))
    );
}

} // namespace

LV_IMG_DECLARE(img_app_musicplayer);

namespace esp_brookesia::apps {

MusicPlayer *MusicPlayer::_instance = nullptr;

MusicPlayer *MusicPlayer::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new MusicPlayer(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

MusicPlayer::MusicPlayer(bool use_status_bar, bool use_navigation_bar):
    App("音乐", &img_app_musicplayer, true, use_status_bar, use_navigation_bar),
    _file_iterator(nullptr), _demo_created(false)
{
    _command_event = xSemaphoreCreateBinaryStatic(&_command_event_buffer);
}

MusicPlayer::~MusicPlayer()
{
    // Normal home/close keeps this installed singleton and never waits here.
    // If an owner actually deletes it, retain the object until the worker has
    // released all resources; deleting a live worker would be a use-after-free.
    _foreground.store(false);
    _worker_exit.store(true);
    requestStop();
    for (;;) {
        portENTER_CRITICAL(&_playback_lock);
        const bool finished = _worker == nullptr;
        portEXIT_CRITICAL(&_playback_lock);
        if (finished) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vSemaphoreDelete(_command_event);
    _instance = nullptr;
}

bool MusicPlayer::ensureWorker()
{
    if (_worker) return true;
    const BaseType_t result = xTaskCreateWithCaps(workerTask, "music_control", 8192,
        this, 3, &_worker, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result != pdPASS) {
        _worker = nullptr;
        ESP_LOGE(TAG, "Cannot allocate music control worker in PSRAM");
        return false;
    }
    return true;
}

MusicPlayer::PlaybackSnapshot MusicPlayer::getPlaybackSnapshot()
{
    portENTER_CRITICAL(&_playback_lock);
    const PlaybackSnapshot snapshot = _playback;
    portEXIT_CRITICAL(&_playback_lock);
    return snapshot;
}

esp_err_t MusicPlayer::enqueue(const Command &command)
{
    if (!_worker || !_foreground.load()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&_playback_lock);
    if (_pending.type != CommandType::None || _playback.busy || _stop_requested.load()) {
        portEXIT_CRITICAL(&_playback_lock);
        return ESP_ERR_INVALID_STATE;
    }
    _pending = command;
    _playback.busy = true;
    _playback.error = ESP_OK;
    portEXIT_CRITICAL(&_playback_lock);
    xSemaphoreGive(_command_event);
    return ESP_OK;
}

esp_err_t MusicPlayer::requestTrack(uint32_t index, bool allow_resume)
{
    if (!_foreground.load() || !_file_iterator || index >= file_iterator_get_count(_file_iterator)) {
        return ESP_ERR_INVALID_ARG;
    }
    Command command;
    command.type = CommandType::Play;
    command.generation = _generation.load();
    command.track = index;
    portENTER_CRITICAL(&_playback_lock);
    if (allow_resume && _playing_track == index && _playback.state == PlaybackState::Paused) {
        command.type = CommandType::Resume;
    }
    portEXIT_CRITICAL(&_playback_lock);
    const int length = file_iterator_get_full_path_from_index(_file_iterator, index,
                                                            command.path, sizeof(command.path));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(command.path)) return ESP_ERR_INVALID_SIZE;
    return enqueue(command);
}

esp_err_t MusicPlayer::requestPause()
{
    if (getPlaybackSnapshot().state != PlaybackState::Playing) return ESP_ERR_INVALID_STATE;
    Command command;
    command.type = CommandType::Pause;
    command.generation = _generation.load();
    return enqueue(command);
}

void MusicPlayer::requestStop()
{
    // This invalidates both a queued path and an open already in progress.
    // The worker owns its copy; no FILE or iterator is borrowed from the UI.
    ++_generation;
    portENTER_CRITICAL(&_playback_lock);
    _pending = {};
    if (_worker) {
        _stop_requested.store(true);
        _playback.busy = true;
    }
    portEXIT_CRITICAL(&_playback_lock);
    xSemaphoreGive(_command_event);
}

void MusicPlayer::workerTask(void *argument)
{
    auto self = static_cast<MusicPlayer *>(argument);
    hosino_media_player_handle_t player = nullptr;
    PlaybackSnapshot current;
    bool cleanup_pending = false;
    TickType_t retry_at = 0;
    uint32_t current_track = UINT32_MAX;

    auto publish = [&]() {
        portENTER_CRITICAL(&self->_playback_lock);
        current.busy = self->_pending.type != CommandType::None || self->_stop_requested.load();
        self->_playback = current;
        self->_playing_track = current_track;
        portEXIT_CRITICAL(&self->_playback_lock);
    };
    auto release = [&]() -> esp_err_t {
        if (!player) return ESP_OK;
        esp_err_t result = hosino_media_player_stop(player);
        if (result == ESP_OK) result = hosino_media_player_join(player, 0);
        if (result == ESP_OK) result = hosino_media_player_destroy(player);
        if (result == ESP_OK) {
            player = nullptr;
            current_track = UINT32_MAX;
            cleanup_pending = false;
        } else {
            // No forced deletion: the facade retains all uncertain ownership.
            cleanup_pending = true;
            retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
            ESP_LOGE(TAG, "ESP Player shutdown retained resources: %s", esp_err_to_name(result));
        }
        return result;
    };
    auto refresh = [&]() {
        if (!player) return;
        const esp_err_t polled = hosino_media_player_poll(player);
        hosino_media_player_snapshot_t snapshot = {};
        const esp_err_t copied = hosino_media_player_get_snapshot(player, &snapshot);
        if (copied != ESP_OK) {
            current.state = PlaybackState::Error;
            current.error = copied;
            return;
        }
        current.position_ms = snapshot.position_ms;
        current.duration_ms = snapshot.duration_ms;
        current.error = snapshot.last_error != ESP_OK ? snapshot.last_error : polled;
        switch (snapshot.state) {
        case HOSINO_MEDIA_IDLE: current.state = PlaybackState::Idle; break;
        case HOSINO_MEDIA_OPENING: current.state = PlaybackState::Opening; break;
        case HOSINO_MEDIA_PLAYING: current.state = PlaybackState::Playing; break;
        case HOSINO_MEDIA_PAUSED: current.state = PlaybackState::Paused; break;
        case HOSINO_MEDIA_FINISHED: current.state = PlaybackState::Finished; break;
        case HOSINO_MEDIA_ERROR: current.state = PlaybackState::Error; break;
        default: current.state = PlaybackState::Stopped; break;
        }
    };

    for (;;) {
        Command command;
        bool stop = false;
        portENTER_CRITICAL(&self->_playback_lock);
        stop = self->_stop_requested.exchange(false);
        if (!stop) command = self->_pending;
        self->_pending = {};
        portEXIT_CRITICAL(&self->_playback_lock);

        if (stop) {
            const esp_err_t result = release();
            current.state = result == ESP_OK ? PlaybackState::Stopped : PlaybackState::Error;
            current.error = result;
            if (result == ESP_OK) {
                current.position_ms = 0;
                current.duration_ms = 0;
            }
        } else if (command.type != CommandType::None &&
                   command.generation == self->_generation.load() && self->_foreground.load()) {
            esp_err_t result = ESP_OK;
            if (command.type == CommandType::Play) {
                result = release();
                if (result == ESP_OK) {
                    current = {};
                    current.state = PlaybackState::Opening;
                    hosino_media_player_config_t config = {};
                    config.owner = HOSINO_MEDIA_OWNER_MUSIC;
                    result = hosino_media_player_create(&config, &player);
                }
                if (result == ESP_OK) result = hosino_media_player_open(player, command.path);
                // Opening may take time. Never begin stale playback after home/close.
                if (result == ESP_OK && command.generation == self->_generation.load() &&
                    self->_foreground.load() && !self->_stop_requested.load()) {
                    result = hosino_media_player_start(player);
                    if (result == ESP_OK) current_track = command.track;
                } else if (result == ESP_OK) {
                    self->_stop_requested.store(true);
                }
            } else if (player) {
                result = command.type == CommandType::Pause
                    ? hosino_media_player_pause(player) : hosino_media_player_resume(player);
            } else result = ESP_ERR_INVALID_STATE;

            if (result != ESP_OK) {
                current.state = PlaybackState::Error;
                current.error = result;
                ESP_LOGE(TAG, "ESP Player command failed: %s", esp_err_to_name(result));
                // An unsuccessful open may still own resources; keep its handle.
                cleanup_pending = player != nullptr;
                retry_at = xTaskGetTickCount();
            } else if (player) refresh();
        } else if (player && !cleanup_pending) {
            refresh();
        }

        if (player && (current.state == PlaybackState::Finished || current.state == PlaybackState::Error)) {
            if (!cleanup_pending) {
                cleanup_pending = true;
                retry_at = xTaskGetTickCount();
            }
        }
        if (cleanup_pending && static_cast<int32_t>(xTaskGetTickCount() - retry_at) >= 0) {
            const esp_err_t saved_error = current.error;
            const PlaybackState saved_state = current.state;
            const esp_err_t stopped = release();
            current.error = saved_error != ESP_OK ? saved_error : stopped;
            current.state = stopped == ESP_OK ? saved_state : PlaybackState::Error;
        }
        publish();
        if (self->_worker_exit.load() && !player) {
            portENTER_CRITICAL(&self->_playback_lock);
            self->_worker = nullptr;
            portEXIT_CRITICAL(&self->_playback_lock);
            vTaskDeleteWithCaps(nullptr);
            return;
        }
        // Skip the sleep if home/close arrived while a vendor call was blocked.
        if (self->_stop_requested.load()) continue;
        xSemaphoreTake(self->_command_event, player ? pdMS_TO_TICKS(100) : portMAX_DELAY);
    }
}

bool MusicPlayer::run(void)
{
    ESP_UTILS_LOGD("Run");
    if (_demo_created) return true;
    bsp_extra_file_instance_deinit(&_file_iterator);
    size_t track_count = 0;
    storage_service_lease_t scan_lease = {};
    const esp_err_t result = storage_service_acquire(&scan_lease);
    if (result == ESP_OK) {
        for (const char *path : {SD_MUSIC_DIR, SD_MUSIC_DIR_COMPAT, SD_SAMPLE_DIR}) {
            track_count = scan_music_directory(path, "SD", &_file_iterator);
            if (track_count) break;
        }
    } else ESP_LOGW(TAG, "SD music unavailable: %s", esp_err_to_name(result));
    // This scanner has closed DIR and retains only owned strings. The facade
    // acquires a fresh lease for the actual open file when playback is requested.
    storage_service_release(&scan_lease);

    _foreground.store(true);
    const bool worker_ready = track_count == 0 || ensureWorker();
    log_memory("Before music UI", track_count);
    _view = make_music_view(lv_screen_active(), getVisualArea());
    lv_demo_music(_view, worker_ready ? _file_iterator : nullptr);
    if (!worker_ready) lv_demo_music_set_message("内存不足，请关闭其他应用后重试");
    else if (!track_count) lv_demo_music_set_message(result == ESP_OK
        ? "请将 MP3/WAV/FLAC 放入 SD 卡 music 目录"
        : "未检测到 SD 卡，请插卡后重新打开");
    _demo_created = true;
    log_memory("After music UI", track_count);
    return true;
}

bool MusicPlayer::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
    return true;
}

bool MusicPlayer::close(void)
{
    _foreground.store(false);
    requestStop();
    if (_demo_created) {
        lv_demo_music_close();
        _demo_created = false;
        _view = nullptr;
    }
    // Worker paths and playback lease belong to the worker/facade, not this UI.
    bsp_extra_file_instance_deinit(&_file_iterator);
    return true;
}

bool MusicPlayer::init(void) { return true; }
bool MusicPlayer::deinit(void) { return close(); }

bool MusicPlayer::pause(void)
{
    _foreground.store(false);
    if (_demo_created) lv_demo_music_exit_pause();
    requestStop();
    return true;
}

bool MusicPlayer::resume(void)
{
    _foreground.store(true);
    lv_demo_music_activate();
    return true;
}

bool MusicPlayer::cleanResource(void) { return close(); }

} // namespace esp_brookesia::apps
