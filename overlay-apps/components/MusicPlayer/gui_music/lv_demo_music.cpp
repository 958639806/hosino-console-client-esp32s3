/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 * Compact view for the ESP Player-backed MusicPlayer. The original demo's
 * fabricated duration, spectrum and album metadata are intentionally omitted.
 */
#include <algorithm>
#include <cstdint>
#include <limits>
#include "lv_demo_music.h"
#include "../MusicPlayer.hpp"
#include "hosino_ui_fonts.h"

namespace {
constexpr uint32_t kRows = 4;
using MusicPlayer = esp_brookesia::apps::MusicPlayer;
using PlaybackState = MusicPlayer::PlaybackState;
struct PlayerView {
    lv_obj_t *parent = nullptr, *name = nullptr, *status = nullptr, *play_label = nullptr;
    lv_obj_t *progress = nullptr;
    lv_obj_t *page_label = nullptr, *page_previous = nullptr, *page_next = nullptr;
    lv_obj_t *rows[kRows] = {}, *row_labels[kRows] = {};
    lv_timer_t *timer = nullptr;
    file_iterator_instance_t *iterator = nullptr;
    uint32_t count = 0, page = 0, selected = 0;
    bool active = false;
    PlaybackState last_state = PlaybackState::Idle;
    esp_err_t last_error = ESP_OK;
    bool last_busy = false;
} view;

lv_obj_t *text(lv_obj_t *parent, const char *value, int x, int y, const lv_font_t *font)
{
    auto label = lv_label_create(parent);
    lv_label_set_text(label, value);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xeff3ff), 0);
    return label;
}
lv_obj_t *button(lv_obj_t *parent, const char *title, int x, int y, int w, int h,
                 lv_event_cb_t callback, intptr_t action, lv_obj_t **label_out = nullptr)
{
    auto obj = lv_button_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x293657), 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 7, 0);
    auto label = text(obj, title, 0, 0, &hosino_font_16);
    lv_obj_center(label);
    if (label_out) *label_out = label;
    lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, reinterpret_cast<void *>(action));
    return obj;
}
void update_rows()
{
    if (!view.parent) return;
    const uint32_t pages = view.count ? (view.count - 1) / kRows + 1 : 1;
    lv_label_set_text_fmt(view.page_label, "%u / %u", static_cast<unsigned>(view.page + 1), static_cast<unsigned>(pages));
    lv_obj_set_state(view.page_previous, LV_STATE_DISABLED, view.page == 0);
    lv_obj_set_state(view.page_next, LV_STATE_DISABLED, view.page + 1 >= pages);
    for (uint32_t row = 0; row < kRows; ++row) {
        const uint32_t index = view.page * kRows + row;
        lv_obj_set_flag(view.rows[row], LV_OBJ_FLAG_HIDDEN, index >= view.count);
        if (index >= view.count) continue;
        const char *name = file_iterator_get_name_from_index(view.iterator, index);
        lv_label_set_text(view.row_labels[row], name ? name : "无法读取文件名");
        lv_obj_set_style_bg_color(view.rows[row], lv_color_hex(index == view.selected ? 0x3a5382 : 0x1b2640), 0);
    }
    if (view.count) {
        const char *name = file_iterator_get_name_from_index(view.iterator, view.selected);
        lv_label_set_text(view.name, name ? name : "无法读取文件名");
    }
}
void poll(lv_timer_t *)
{
    if (!view.active || !view.count) return;
    const auto state = MusicPlayer::requestInstance()->getPlaybackSnapshot();
    const uint32_t seconds = state.position_ms / 1000;
    if (state.duration_ms > 0) {
        const uint32_t total = state.duration_ms / 1000;
        lv_label_set_text_fmt(view.progress, "%02lu:%02lu / %02lu:%02lu",
                              (unsigned long)(seconds / 60), (unsigned long)(seconds % 60),
                              (unsigned long)(total / 60), (unsigned long)(total % 60));
    } else {
        lv_label_set_text_fmt(view.progress, "%02lu:%02lu / --:--",
                              (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
    }
    if (state.state != view.last_state || state.error != view.last_error || state.busy != view.last_busy) {
        if (state.busy || state.state == PlaybackState::Opening) lv_demo_music_set_message("正在处理…");
        else if (state.error != ESP_OK || state.state == PlaybackState::Error) {
            lv_label_set_text_fmt(view.status, "无法播放：%s", esp_err_to_name(state.error));
        } else if (state.state == PlaybackState::Playing) lv_demo_music_set_message("正在播放");
        else if (state.state == PlaybackState::Paused) lv_demo_music_set_message("已暂停");
        else if (state.state == PlaybackState::Finished || state.state == PlaybackState::Stopped) lv_demo_music_set_message("播放已停止");
        lv_label_set_text(view.play_label, state.state == PlaybackState::Playing ? "暂停" : "播放");
    }
    view.last_state = state.state;
    view.last_error = state.error;
    view.last_busy = state.busy;
}
void play(uint32_t index, bool allow_resume)
{
    if (!view.active || !view.count || index >= view.count) return;
    const esp_err_t result = MusicPlayer::requestInstance()->requestTrack(index, allow_resume);
    if (result != ESP_OK) {
        lv_label_set_text_fmt(view.status, "无法播放：%s", esp_err_to_name(result));
        return;
    }
    view.selected = index;
    view.page = index / kRows;
    update_rows();
    lv_demo_music_set_message("正在打开");
}
void row_cb(lv_event_t *event)
{
    const uint32_t row = static_cast<uint32_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    play(view.page * kRows + row, false);
}
void page_cb(lv_event_t *event)
{
    if (!view.active || !view.count) return;
    const intptr_t direction = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (direction < 0 && view.page) --view.page;
    if (direction > 0 && view.page < (view.count - 1) / kRows) ++view.page;
    update_rows();
}
void control_cb(lv_event_t *event)
{
    if (!view.active || !view.count) return;
    const intptr_t action = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (action < 0) play(view.selected ? view.selected - 1 : view.count - 1, false);
    else if (action > 0) play(view.selected + 1 < view.count ? view.selected + 1 : 0, false);
    else if (MusicPlayer::requestInstance()->getPlaybackSnapshot().state == PlaybackState::Playing) lv_demo_music_pause();
    else play(view.selected, true);
}
} // namespace

extern "C" void lv_demo_music(lv_obj_t *parent, file_iterator_instance_t *iterator)
{
    view = {};
    view.parent = parent;
    view.iterator = iterator;
    view.active = true;
    const size_t count = iterator ? file_iterator_get_count(iterator) : 0;
    // The backend uses an int track index; don't narrow a large size_t silently.
    view.count = static_cast<uint32_t>(std::min(count, static_cast<size_t>(std::numeric_limits<int>::max())));
    const int width = lv_obj_get_width(parent), height = lv_obj_get_height(parent);
    text(parent, "音乐", 44, 10, &hosino_font_20);
    view.name = text(parent, view.count ? "" : "没有可播放的音乐", 44, 42, &hosino_font_18);
    lv_obj_set_width(view.name, width - 88);
    lv_label_set_long_mode(view.name, LV_LABEL_LONG_DOT);
    view.status = text(parent, view.count ? "点击曲目播放" : "请将 MP3/WAV/FLAC 放入 SD 卡 music 目录", 44, 70, &hosino_font_14);
    lv_obj_set_width(view.status, width - 88);
    lv_obj_set_height(view.status, view.count ? 24 : 52);
    lv_label_set_long_mode(view.status, LV_LABEL_LONG_WRAP);
    view.progress = text(parent, "00:00 / --:--", 44, 98, &hosino_font_14);
    lv_obj_set_flag(view.progress, LV_OBJ_FLAG_HIDDEN, view.count == 0);
    const int row_height = std::max(16, std::min(37, (height - 242) / 4));
    for (uint32_t row = 0; row < kRows; ++row) {
        view.rows[row] = button(parent, "", 44, 124 + row * (row_height + 6), width - 88, row_height, row_cb, row, &view.row_labels[row]);
        lv_obj_set_width(view.row_labels[row], width - 116);
        lv_obj_set_style_text_font(view.row_labels[row], &hosino_font_14, 0);
        lv_label_set_long_mode(view.row_labels[row], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(view.row_labels[row], LV_TEXT_ALIGN_LEFT, 0);
    }
    view.page_previous = button(parent, "上一页", 44, height - 94, 96, 32, page_cb, -1);
    view.page_next = button(parent, "下一页", width - 140, height - 94, 96, 32, page_cb, 1);
    view.page_label = text(parent, "1 / 1", width / 2 - 40, height - 86, &hosino_font_14);
    lv_obj_set_width(view.page_label, 80);
    lv_obj_set_style_text_align(view.page_label, LV_TEXT_ALIGN_CENTER, 0);
    const int start = (width - 300) / 2;
    auto previous = button(parent, "上一曲", start, height - 42, 90, 38, control_cb, -1);
    auto toggle = button(parent, "播放", start + 102, height - 42, 96, 38, control_cb, 0, &view.play_label);
    auto next = button(parent, "下一曲", start + 210, height - 42, 90, 38, control_cb, 1);
    for (auto obj : {previous, toggle, next}) lv_obj_set_state(obj, LV_STATE_DISABLED, view.count == 0);
    update_rows();
    view.timer = lv_timer_create(poll, 300, nullptr);
    if (view.timer && !view.count) lv_timer_pause(view.timer);
}

extern "C" void lv_demo_music_set_message(const char *message)
{
    if (view.status) lv_label_set_text(view.status, message);
}
extern "C" void lv_demo_music_pause(void)
{
    if (!view.parent) return;
    if (MusicPlayer::requestInstance()->requestPause() != ESP_OK) {
        lv_demo_music_set_message("暂停失败，请重试");
        return;
    }
    lv_demo_music_set_message("正在处理…");
}
extern "C" void lv_demo_music_exit_pause(void)
{
    view.active = false;
    if (view.timer) lv_timer_pause(view.timer);
}
extern "C" void lv_demo_music_activate(void)
{
    if (!view.parent) return;
    view.active = true;
    view.last_state = PlaybackState::Idle;
    view.last_error = ESP_OK;
    view.last_busy = false;
    if (view.timer && view.count) { lv_timer_reset(view.timer); lv_timer_resume(view.timer); }
    if (view.count) lv_demo_music_set_message("点击播放，从头开始");
}
extern "C" void lv_demo_music_close(void)
{
    lv_demo_music_exit_pause();
    // The timer was created during run(): Brookesia deletes it along with the
    // default screen. Do not manually delete either collected resource twice.
    view = {};
}
