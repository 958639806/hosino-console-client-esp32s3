/* SPDX-FileCopyrightText: 2026 Waveshare
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include "hosino_media_player.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "storage_service.h"
#include "video_view_math.h"
#include "video_rgb565_scale.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"
namespace esp_brookesia::apps {
class VideoPlayer : public systems::phone::App {
public:
    static VideoPlayer *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~VideoPlayer();
protected:
    VideoPlayer(bool use_status_bar, bool use_navigation_bar);
    bool run() override; bool back() override; bool close() override; bool init() override;
    bool deinit() override; bool pause() override; bool resume() override; bool cleanResource() override;
private:
    enum class State : uint8_t { Idle, Mounting, Scanning, Preparing, Playing, Paused, Empty, NoCard, Error, Stopping, Browsing };
    enum class Command : uint8_t { None, Play, Pause, Previous, Next, Files, Parent, Refresh, PagePrevious, PageNext, OpenEntry, Seek, Backward, Forward, Decoder, Scale };
    static constexpr unsigned PAGE_SIZE = 4;
    struct BrowserEntry { char name[HOSINO_MEDIA_PATH_MAX]; bool directory; unsigned video_index; };
    struct BrowserPage {
        char directory[HOSINO_MEDIA_PATH_MAX];
        BrowserEntry entries[PAGE_SIZE];
        unsigned page, count, total, videos;
        esp_err_t error;
    };
    struct BrowserStore {
        BrowserPage published, working;
        char directory[HOSINO_MEDIA_PATH_MAX];
        char selected[HOSINO_MEDIA_PATH_MAX];
        char command_path[HOSINO_MEDIA_PATH_MAX];
        uint64_t command_seek_ms;
        unsigned page;
    };
    static VideoPlayer *_instance;
    lv_obj_t *_canvas = nullptr, *_screen = nullptr, *_viewport = nullptr;
    lv_obj_t *_header = nullptr, *_controls = nullptr, *_hero_icon = nullptr;
    lv_obj_t *_status_label = nullptr, *_counter_label = nullptr;
    lv_obj_t *_previous_button = nullptr, *_play_button = nullptr, *_next_button = nullptr, *_play_button_label = nullptr;
    lv_obj_t *_files_button = nullptr, *_backward_button = nullptr, *_forward_button = nullptr;
    lv_obj_t *_progress = nullptr, *_time_label = nullptr;
    lv_obj_t *_volume_slider = nullptr, *_volume_label = nullptr, *_scale_button = nullptr, *_scale_label = nullptr;
    std::atomic<hosino_video_scale_mode_t> _scale_mode{HOSINO_VIDEO_ORIGINAL};
    std::atomic<hosino_video_scale_mode_t> _scale_applied{HOSINO_VIDEO_ORIGINAL};
    std::atomic<bool> _scale_failed{false};
    bool _volume_dragging = false;
    std::atomic<int> _volume_requested{-1}, _volume_applied{50};
    std::atomic<bool> _audio_enabled{false};
    std::atomic<esp_err_t> _volume_error{ESP_OK};
    lv_obj_t *_browser_root = nullptr, *_directory_label = nullptr, *_browser_status = nullptr;
    lv_obj_t *_parent_button = nullptr, *_refresh_button = nullptr, *_page_previous = nullptr, *_page_next = nullptr;
    lv_obj_t *_page_label = nullptr, *_entry_buttons[PAGE_SIZE] = {}, *_entry_labels[PAGE_SIZE] = {};
    lv_obj_t *_decoder_button = nullptr, *_decoder_label = nullptr;
    std::atomic<hosino_media_decoder_t> _decoder{HOSINO_MEDIA_DECODER_ESPRESSIF};
    lv_timer_t *_ui_timer = nullptr;
    BrowserStore *_browser = nullptr; // Explicit PSRAM; bounded two pages, never all directory entries.
    unsigned _last_browser_revision = 0;
    std::atomic<unsigned> _browser_revision{0};
    std::atomic<bool> _browser_visible{true};
    uint64_t _position_ms = 0, _duration_ms = 0; // Protected by the file-local internal spinlock.
    // User seek intent survives asynchronous backend seeks. Protected by browser_lock.
    uint64_t _seek_target_ms = 0;
    uint32_t _seek_revision = 0, _seek_issued_revision = 0;
    bool _seek_intent_active = false;
    bool _seek_dragging = false;
    alignas(LV_DRAW_BUF_ALIGN) uint16_t _placeholder_pixel = 0;
    State _last_ui_state = State::Idle;
    bool _frame_presented = false, _chrome_visible = true, _foreground = false, _restart_pending = false;
    bool _pinch_armed = false, _pinch_applied = false, _pinch_seen = false;
    uint32_t _chrome_hide_at_ms = 0, _fps_since_ms = 0, _fps_frames = 0;
    float _video_fps = 0;
    std::atomic<bool> _canvas_detached{true};
    std::atomic<int> _viewport_width{480}, _viewport_height{372};
    std::atomic<State> _state{State::Idle};
    std::atomic<Command> _command{Command::None};
    std::atomic<TaskHandle_t> _worker_task{nullptr};
    std::atomic<bool> _stop_requested{false}, _worker_stopped{true}, _playing{false}, _ui_accept_frames{false};
    std::atomic<int> _file_count{0}, _current_index{0};
    std::atomic<uint32_t> _decoded_frames{0}, _dropped_frames{0};
    std::atomic<esp_err_t> _last_error{ESP_OK};
    std::atomic<bool> _cleanup_complete{true};
    storage_service_lease_t _storage_lease = {};
    hosino_media_player_handle_t _media = nullptr;
    uint8_t *_frame_buffers[2] = {};
    hosino_video_rgb565_scratch_t *_scale_scratch = nullptr; // 2880B internal, playback-only.
    size_t _frame_buffer_capacity[2] = {};
    uint16_t _frame_width[2] = {}, _frame_height[2] = {};
    uint16_t _frame_source_width[2] = {}, _frame_source_height[2] = {};
    hosino_video_scale_mode_t _frame_scale_mode[2] = {};
    uint32_t _frame_copy_us[2] = {};
    uint32_t _view_copy_sum_us = 0, _view_copy_max_us = 0, _view_copy_samples = 0;
    std::atomic<int> _display_buffer_index{-1}, _pending_buffer_index{-1};
    bool startWorker(); bool stopWorker(TickType_t timeout); void requestStop(); void detachCanvas(); void destroyUi();
    void updateUi(); void setChromeVisible(bool visible); void showChrome(); void updateControlState(State state, int count);
    void applyViewScale();
    void resetPinch();
    void createBrowser(); void updateBrowser(); void showBrowser();
    bool postCommand(Command command, const char *path = nullptr, uint64_t seek_ms = 0);
    Command takeCommand(char *path, uint64_t *seek_ms);
    void cancelPendingSeek();
    esp_err_t dispatchPendingSeek(const hosino_media_player_snapshot_t &snapshot);
    esp_err_t scanDirectory(); esp_err_t adjacentVideo(int direction); esp_err_t startSelected();
    void releaseFrameBuffers(); bool cleanupPlaybackResources();
    static void workerTask(void *arg);
    static esp_err_t videoCallback(const hosino_media_video_frame_t *frame, void *arg);
    static void controlEvent(lv_event_t *event); static void browserEvent(lv_event_t *event);
    static void seekEvent(lv_event_t *event); static void chromeEvent(lv_event_t *event); static void uiTimer(lv_timer_t *timer);
    static void volumeEvent(lv_event_t *event);
    static void pinchEvent(lv_event_t *event);
};
}
