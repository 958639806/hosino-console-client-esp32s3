// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>
#include "lvgl.h"
#include "hosino_media_player.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {
struct BiliPlaybackRequest {
    const char *stream_url;
    const char *allowed_base_url;
    const char *device_token;
    const char *session_id;
    double position_seconds;
    double duration_seconds;
    uint16_t video_width,video_height,video_fps;
};
struct BiliMediaSnapshot {
    hosino_media_state_t state = HOSINO_MEDIA_IDLE;
    esp_err_t error = ESP_OK;
    uint64_t position_ms = 0; // Relative FLV PTS; app adds source start offset.
    uint64_t duration_ms = 0;
};
// Called exclusively by the app's control worker. open must copy request fields
// it retains. stop must quiesce the borrowed-frame callback before returning OK.
// Hooks/context must outlive the installed singleton. No hook may call LVGL.
struct BiliMediaHooks {
    void *context = nullptr;
    esp_err_t (*open)(void *, const BiliPlaybackRequest &, hosino_media_video_cb_t, void *) = nullptr;
    esp_err_t (*stop)(void *) = nullptr;
    esp_err_t (*pause)(void *, bool) = nullptr;
    esp_err_t (*volume)(void *, int) = nullptr;
    esp_err_t (*snapshot)(void *, BiliMediaSnapshot &) = nullptr;
};
namespace bili { struct Worker; struct Model; struct Command; }
class BiliBili : public systems::phone::App {
public:
    static BiliBili *requestInstance(bool use_status_bar = true, bool use_navigation_bar = false);
    void setMediaHooks(const BiliMediaHooks &hooks);
    ~BiliBili() override;
protected:
    bool init() override;
    bool run() override;
    bool back() override;
    bool pause() override;
    bool resume() override;
    bool close() override;
    bool cleanResource() override;
    bool deinit() override;
private:
    BiliBili(bool status, bool navigation);
    enum class View { Home, Videos, Detail, Profile, Folders, Login, Settings, Search, Player, Options, FavoritePick };
    enum class Action : uintptr_t { Back=1, Home, Search, Profile, Settings, Save, Query,
        Previous, Next, Select, Play, Part, Login, Logout, Folders, Favorite, Remove,
        PickFolder, Pause, Rewind, Forward, Options, Quality, Rate, Volume, Fit, Seek, Keyboard };
    static void event(lv_event_t *);
    static void tick(lv_timer_t *);
    bool ensureWorker();
    bool send(bili::Command &);
    void navigate(View);
    void refresh();
    void render();
    void playerUi();
    void detachCanvas();
    void action(Action, int);
    void showNotice(const char *);
    lv_obj_t *label(lv_obj_t *, const char *, int x, int y, int w, const lv_font_t * = nullptr);
    lv_obj_t *button(lv_obj_t *, const char *, int x, int y, int w, int h, Action, int value=0, bool accent=false);
    static BiliBili *_instance;
    BiliMediaHooks _hooks{};
    std::shared_ptr<bili::Worker> _worker;
    bili::Model *_model = nullptr;
    lv_obj_t *_root=nullptr, *_body=nullptr, *_notice=nullptr, *_canvas=nullptr;
    lv_obj_t *_edit=nullptr, *_tokenEdit=nullptr, *_keyboard=nullptr, *_ime=nullptr;
    lv_obj_t *_time=nullptr, *_progress=nullptr, *_playLabel=nullptr, *_playerControls=nullptr;
    lv_timer_t *_timer=nullptr;
    View _view=View::Home, _presentedView=View::Home, _listView=View::Home;
    uint32_t _revision=0, _qrNext=0, _chromeUntil=0;
    uint32_t _nextSerial=0, _waitingFor=0;
    int _width=0, _height=0, _page=1, _keyboardMode=0;
    int _presentedPage=1;
    uint64_t _folder=0, _presentedFolder=0;
    bool _foreground=false, _rebuild=true, _fit=false, _searchList=false, _loaded=false;
    char _query[256]{}, _noticeText[192]{}, _detailBvid[16]{}, _qrRendered[1024]{};
    alignas(LV_DRAW_BUF_ALIGN) uint16_t _placeholder=0;
};
} // namespace esp_brookesia::apps
