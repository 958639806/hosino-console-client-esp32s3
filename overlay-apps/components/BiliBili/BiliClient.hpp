// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <memory>
#include "BiliBili.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

namespace esp_brookesia::apps::bili {
constexpr unsigned PageSize=10, PartCount=20;
struct Video { char bvid[16]{}, title[256]{}, author[96]{}; uint64_t cid=0; double duration=0; };
struct Row { Video video; uint64_t folder=0; unsigned count=0; };
struct Part { uint64_t cid=0; double duration=0; char title[128]{}; };
struct Model {
    uint32_t revision=0;
    uint32_t completed_serial=0;
    bool ready=false, configured=false, busy=false, more=false, logged_in=false, playing=false, paused=false, token_present=false;
    char base[256]{}, error[192]{}, name[96]{}, vip[80]{}, login_id[96]{}, qr_url[1024]{}, qr_status[24]{};
    char session_id[96]{};
    Row rows[PageSize]{}; unsigned count=0, page=1;
    Video selected{};
    Part parts[PartCount]{}; unsigned part_count=0;
    uint64_t mid=0; int level=0; double coins=0;
    int profile=0, bitrate=0, volume=50;
    double source_start=0, position=0, duration=0;
    hosino_media_state_t media_state=HOSINO_MEDIA_IDLE;
};
enum class Op { Feed, Search, Detail, Me, Folders, Favorites, Qr, PollQr, Logout, Favorite,
    Configure, Play, Stop, Pause, Seek, Quality, Rate, Volume };
struct Command {
    Op op=Op::Feed;
    uint32_t serial=0;
    int page=1, value=0;
    uint64_t cid=0, folder=0;
    double position=0;
    char text[256]{}, token[256]{}, bvid[16]{};
};
struct Worker {
    SemaphoreHandle_t mutex=nullptr;
    QueueHandle_t queue=nullptr;
    std::atomic<bool> stop{false}, done{false}, foreground{true}, frames{false};
    BiliMediaHooks hooks{};
    Model model{};
    char token[256]{};
    uint8_t *pixels[2]{nullptr,nullptr};
    size_t capacity[2]{0,0};
    uint16_t width[2]{0,0}, height[2]{0,0};
    int displayed=-1, pending=-1;
    bool media_active=false;
    static std::shared_ptr<Worker> create(const BiliMediaHooks &);
    bool submit(const Command &);
    bool copy(Model &);
    ~Worker();
    static void task(void *);
    static esp_err_t frame(const hosino_media_video_frame_t *, void *);
};
} // namespace esp_brookesia::apps::bili
