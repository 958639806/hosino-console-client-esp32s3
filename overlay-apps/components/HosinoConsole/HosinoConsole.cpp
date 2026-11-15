// SPDX-License-Identifier: Apache-2.0
#include "HosinoConsole.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include "esp_brookesia.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "hosino_ui_fonts.h"
#include "hosino_chat_fonts.h"
#include "hosino_chat_display.h"

LV_IMAGE_DECLARE(hosino_console_icon);

namespace esp_brookesia::apps {
namespace {
constexpr uint32_t kBackground = 0x0c111c, kSurface = 0x192232, kMuted = 0x9caac0, kAccent = 0x397eac;
constexpr int kMargin = 40, kBottom = 36, kHeader = 44, kBody = 76;
// Each page contributes at most 18 printable ASCII characters. Space has its own key.
constexpr const char *kPasswordPages[] = {
    "abcdefghijklmnopqr", "stuvwxyz0123456789",
    "ABCDEFGHIJKLMNOPQR", "STUVWXYZ0123456789",
    "0123456789.-_@/+=?",
    "!\"#$%&'()*+,-./:;<", "=>?@[\\]^_`{|}~",
};
constexpr const char *kPasswordPageNames[] = {
    "小写 1/2 · a-r", "小写 2/2 · s-z、数字",
    "大写 1/2 · A-R", "大写 2/2 · S-Z、数字",
    "数字 · 常用符号", "符号 1/2", "符号 2/2",
};

void erase_text(char *text, size_t size)
{
    volatile char *p = text;
    while (size--) *p++ = 0;
}

void copy_text(char *dst, size_t capacity, const char *src)
{
    if (!capacity) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= capacity) {
        n = capacity - 1;
        while (n && (static_cast<unsigned char>(src[n]) & 0xc0) == 0x80) --n;
    }
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

bool touch_down()
{
    for (lv_indev_t *input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER &&
            lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) return true;
    }
    return false;
}

bool writes_state(hosino_console::CommandType type)
{
    using hosino_console::CommandType;
    return type == CommandType::Login || type == CommandType::Logout ||
           type == CommandType::CreateSession || type == CommandType::Send ||
           type == CommandType::Abort || type == CommandType::TranscribeMic || type == CommandType::SendQQ ||
           type == CommandType::ResolveUncertain || type == CommandType::PairStart || type == CommandType::ForgetDevice;
}
} // namespace

HosinoConsole::HosinoConsole() : App("星野控制台", &hosino_console_icon, true, true, false) {}

HosinoConsole::~HosinoConsole()
{
    _foreground = false;
    hosino_buttons_set_active(false);
    if (_buttonTimer) lv_timer_pause(_buttonTimer);
    if (_timer) lv_timer_pause(_timer);
    if (_client) {
        // release() hands ownership to the network worker for async cleanup;
        // no GUI wait and no deletion of a client still referenced by HTTP.
        _client->release();
        _client = nullptr;
    }
    if (_snapshot) hosino_console::Client::freeSnapshot(_snapshot);
    if (_command) { erase_text(reinterpret_cast<char *>(_command), sizeof(*_command)); heap_caps_free(_command); }
    erase_text(_draft, sizeof(_draft));
    erase_text(_query, sizeof(_query));
    erase_text(_sentDraft, sizeof(_sentDraft)); erase_text(_sentSession, sizeof(_sentSession));
    erase_text(_confirmText, sizeof(_confirmText)); erase_text(_confirmSession, sizeof(_confirmSession));
}

bool HosinoConsole::init() { return true; }
bool HosinoConsole::deinit() { return close(); }
bool HosinoConsole::cleanResource() { return close(); }

bool HosinoConsole::ensureClient()
{
    if (_client && _closingClient) {
        if (!_client->isStopped()) {
            setNotice("正在结束上次连接，请稍候…");
            return false;
        }
        if (!_client->destroy()) return false;
        _client = nullptr;
        _closingClient = false;
    }
    if (!_snapshot) _snapshot = hosino_console::Client::allocSnapshot();
    if (!_command) {
        _command = static_cast<hosino_console::Command *>(heap_caps_calloc(
            1, sizeof(hosino_console::Command), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (_command) new (_command) hosino_console::Command();
    }
    if (!_client && _snapshot && _command) {
        memset(_snapshot, 0, sizeof(*_snapshot));
        _client = hosino_console::Client::create();
        _voiceResultSeen = 0;
        _voiceAwaiting = false;
    }
    if (!_client || !_snapshot || !_command) {
        setNotice("内存不足，请关闭其他应用后重试");
        return false;
    }
    _clientReady = true;
    _notice[0] = 0;
    return true;
}

bool HosinoConsole::run()
{
    _screen = lv_screen_active();
    _foreground = true;
    _view = View::Chat;
    _viewPending = _inputPending = false;
    _requestPending = false;
    _renderedContent = 0;
    _nextClientAttempt = lv_tick_get() + 1000;
    lv_obj_set_style_bg_color(_screen, lv_color_hex(kBackground), 0);
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(_screen);
    const auto &visual = getVisualArea();
    _width = std::max(1, static_cast<int>(visual.x2 - visual.x1 + 1) - 2 * kMargin);
    _height = std::max(1, static_cast<int>(visual.y2 - visual.y1 + 1) - kBottom - 8);
    _root = lv_obj_create(_screen);
    lv_obj_remove_style_all(_root);
    lv_obj_set_pos(_root, kMargin, 8);
    lv_obj_set_size(_root, _width, _height);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
    if (ensureClient()) {
        _client->setForeground(true);
        _client->copySnapshot(*_snapshot);
    }
    loadShortcuts();
    _buttonsReady = hosino_buttons_init() == ESP_OK;
    if (_buttonsReady) {
        // Created during run(): Brookesia records and finally deletes this timer.
        _buttonTimer = lv_timer_create(buttonTimerCallback, 20, this);
        _buttonsReady = _buttonTimer != nullptr;
    }
    hosino_buttons_set_active(_buttonsReady);
    if (!_buttonsReady) {
        copy_text(_keyNotice, sizeof(_keyNotice), "实体键暂不可用，请继续使用触屏");
        setNotice(_keyNotice);
    }
    render();
    _timer = lv_timer_create(timerCallback, 100, this);
    if (!_timer) {
        hosino_buttons_set_active(false);
        if (_buttonTimer) lv_timer_pause(_buttonTimer);
    }
    return _timer != nullptr;
}

bool HosinoConsole::canNavigate() const
{
    return _foreground && _screen && _root && lv_obj_is_valid(_screen) &&
           lv_obj_is_valid(_root) && lv_screen_active() == _screen;
}

bool HosinoConsole::pendingWrite() const
{
    return _requestPending && writes_state(_pendingType);
}

const char *HosinoConsole::pendingWriteStatus() const
{
    using hosino_console::CommandType;
    if (_pendingType == CommandType::Login)
        return _snapshot && _snapshot->status[0] ? _snapshot->status : "正在登录，可上滑返回";
    if (_pendingType == CommandType::Send || _pendingType == CommandType::SendQQ)
        return "正在发送，请勿重复提交";
    if (_pendingType == CommandType::TranscribeMic)
        return _snapshot && _snapshot->status[0] ? _snapshot->status : "正在准备语音";
    return "正在处理操作…";
}

bool HosinoConsole::selectionReady() const
{
    return !_selectingKey[0] || (_snapshot && _snapshot->selected &&
           !strcmp(_selectingKey, _snapshot->selected_key));
}

bool HosinoConsole::canResolveSend() const
{
    return _view == View::ReviewSend && _snapshot && _snapshot->authed &&
        _snapshot->selected && _snapshot->send_uncertain && _confirmSession[0] &&
        !strcmp(_snapshot->selected_key, _confirmSession) && selectionReady() && !pendingWrite();
}

bool HosinoConsole::canStartPairing() const
{
    if (!_snapshot || pendingWrite() || _snapshot->device_credential_busy) return false;
    using hosino_console::PairState;
    switch (_snapshot->pairing_state) {
    case PairState::Starting: case PairState::Waiting: case PairState::Storing:
    case PairState::Activating: case PairState::Verifying: return false;
    default: return true;
    }
}

bool HosinoConsole::canForgetDevice() const
{
    return _view == View::ForgetDevice && _snapshot && _snapshot->device_saved &&
        !_snapshot->device_credential_busy && !pendingWrite() && _confirmDeviceId[0] &&
        !strcmp(_snapshot->device_id, _confirmDeviceId);
}

void HosinoConsole::cancelPairingUi()
{
    if (_client) _client->cancelPairing();
    if (_requestPending && _pendingType == hosino_console::CommandType::PairStart)
        _requestPending = false;
}

bool HosinoConsole::actionBlocked(Action action) const
{
    if (action == Action::PairCancel) return false;
    if (action == Action::PairStart && !canStartPairing()) return true;
    if (action == Action::ConfirmForgetDevice && !canForgetDevice()) return true;
    if (!selectionReady() && (action == Action::Compose || action == Action::Voice ||
        action == Action::Abort || action == Action::EditAccept || action == Action::ConfirmSend || action == Action::ResolveSend || action == Action::SendOptions)) return true;
    if (action == Action::RememberLogin && _snapshot && _snapshot->remember_busy) return true;
    if (!pendingWrite()) return false;
    if (action == Action::Compose) return _pendingType != hosino_console::CommandType::Send &&
        _pendingType != hosino_console::CommandType::SendQQ;
    switch (action) {
    case Action::Chat: case Action::ChatMenu: case Action::Manage: case Action::Back:
    case Action::Chinese: case Action::English: case Action::Number:
    case Action::PasswordVisibility: case Action::Shortcuts: case Action::KeyCycle:
    case Action::KeySave: case Action::VoiceFinish: case Action::VoiceCancel:
    case Action::DeviceIdentity: case Action::ReviewSend: case Action::SendOptions: case Action::ChooseDeviceAI: case Action::ChooseChannelAI: case Action::ChooseDirectQQ:
        return false; // Local navigation/editing remains available during a write.
    default: return true;
    }
}

uint64_t HosinoConsole::contentSignature() const
{
    uint64_t hash = UINT64_C(14695981039346656037);
    auto add = [&](const auto &value) {
        const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) { hash ^= bytes[i]; hash *= UINT64_C(1099511628211); }
    };
    add(_view); add(_clientReady);
    if (!_snapshot) return hash;
    const auto &s = *_snapshot;
    add(s.authed); add(s.auth_known);
    add(s.device_authenticated); add(s.device_saved);
    if (_view == View::Pairing) {
        add(s.pairing_state); add(s.user_code); add(s.verification_uri);
    }
    if (_view == View::DeviceIdentity || _view == View::ForgetDevice || _view == View::ChatMenu) {
        add(s.device_label); add(s.device_id);
    }
    if (!s.authed) return hash;
    if (_view == View::Chat || _view == View::ChatMenu || _view == View::SendOptions || _view == View::ConfirmSend || _view == View::ReviewSend) {
        add(s.selected); add(s.send_available); add(s.send_uncertain);
        add(s.selected_key); add(s.selected_name); add(s.selected_channel);
        if (_view == View::ChatMenu) add(s.selected_active);
        if (_view == View::SendOptions || _view == View::ConfirmSend) add(s.qq_send_available);
    }
    switch (_view) {
    case View::Chat:
        add(s.message_count); add(s.history_available); add(s.history_has_more);
        for (unsigned i = 0; i < s.message_count && i < hosino_console::kMessageCount; ++i) {
            add(s.messages[i].role); add(s.messages[i].text); add(s.messages[i].sender); add(s.messages[i].truncated);
        }
        break;
    case View::Sessions:
        add(s.session_offset); add(s.session_total); add(s.sessions_available);
        add(s.session_count); add(s.sessions_have_more);
        for (unsigned i = 0; i < s.session_count && i < hosino_console::kSessionCount; ++i) {
            add(s.sessions[i].key); add(s.sessions[i].name); add(s.sessions[i].channel); add(s.sessions[i].active);
        }
        break;
    case View::Overview: add(s.overview_available); add(s.overview); break;
    case View::Usage: add(s.usage_available); add(s.usage); break;
    case View::Models:
        add(s.models_available); add(s.model_count); add(s.model_total); add(s.models_truncated);
        for (unsigned i = 0; i < s.model_count && i < hosino_console::kModelCount; ++i) add(s.models[i]);
        break;
    case View::Memory:
        add(s.memory_available); add(s.memory_count);
        for (unsigned i = 0; i < s.memory_count && i < hosino_console::kMemoryCount; ++i) add(s.memories[i]);
        break;
    case View::Voice: add(s.voice_state); add(_voiceAwaiting); break;
    case View::ReviewSend: add(s.send_result_key); add(s.send_http_status); add(s.send_error_code); add(s.send_status); break;
    case View::Manage: add(s.quiet); break;
    default: break;
    }
    // Deliberately exclude revision, busy, status and elapsed time: polling
    // changes those without changing the visible list or its hit targets.
    return hash;
}

void HosinoConsole::refreshControls()
{
    auto updateLabel = [](lv_obj_t *object, const char *value) {
        if (object && lv_obj_is_valid(object) && strcmp(lv_label_get_text(object), value))
            lv_label_set_text(object, value);
    };
    for (unsigned i = 0; i < _bindingCount; ++i) {
        auto &binding = _bindings[i];
        if (!binding.object || !lv_obj_is_valid(binding.object)) continue;
        const bool disabled = !binding.enabled || actionBlocked(binding.action);
        if (disabled != lv_obj_has_state(binding.object, LV_STATE_DISABLED)) {
            if (disabled) lv_obj_add_state(binding.object, LV_STATE_DISABLED);
            else lv_obj_remove_state(binding.object, LV_STATE_DISABLED);
        }
        auto *caption = lv_obj_get_child(binding.object, 0);
        if (binding.action == Action::RememberLogin && _snapshot) {
            updateLabel(caption, _snapshot->remember_busy ? "登录设置保存中…" :
                _snapshot->remember_error != ESP_OK ? "登录设置保存失败 · 点此重试" :
                _snapshot->remember_login ? "[已勾选] 密码登录记住凭证" : "[未勾选] 密码登录记住凭证");
        } else if (binding.action == Action::EditAccept && _edit == EditKind::Message && _snapshot) {
            updateLabel(caption, _snapshot->send_uncertain ? "核对" : "发送");
        } else if (binding.action == Action::Compose && _snapshot) {
            updateLabel(caption, _snapshot->send_uncertain ? "编辑草稿 · 发送待核对" :
                !_snapshot->send_available ? "当前会话暂不能发送" : _draft[0] ? "继续编辑…" : "输入消息…");
        }
    }
    if (_status) {
        if (_view == View::Pairing) {
            updateLabel(_status, _snapshot && _snapshot->pair_status[0] ? _snapshot->pair_status : "正在准备设备配对…");
        } else if (_view == View::Edit) {
            if (_notice[0]) updateLabel(_status, _notice); // Keep editor target context during polling.
        } else {
            const char *message = _notice[0] ? _notice : _snapshot ? _snapshot->status : "连接状态待读取";
            if (_snapshot && _snapshot->send_uncertain && (_view == View::Chat || _view == View::ChatMenu))
                message = "发送未确认，仍可编辑；菜单中可核对恢复";
            else if (!selectionReady()) message = "正在切换到所选会话…";
            else if (pendingWrite()) message = pendingWriteStatus();
            else if (_snapshot && _snapshot->authed && _snapshot->busy && !_notice[0] && _snapshot->last_error == ESP_OK)
                message = "后台同步中，可继续操作";
            updateLabel(_status, message);
        }
    }
    if (_pairCountdown && _snapshot) {
        char remaining[80];
        if (_snapshot->pairing_state == hosino_console::PairState::Waiting)
            snprintf(remaining, sizeof(remaining), "剩余 %lu 秒 · 网页批准后自动继续", static_cast<unsigned long>(_snapshot->pair_remaining_sec));
        else copy_text(remaining, sizeof(remaining), _snapshot->pairing_state == hosino_console::PairState::Ready ?
                       "设备授权已完成" : "配对期间可随时取消");
        updateLabel(_pairCountdown, remaining);
    }
    if (_voiceElapsed && _snapshot) {
        char elapsed[64]; snprintf(elapsed, sizeof(elapsed), "%.1f / 15 秒", _snapshot->voice_elapsed_ms / 1000.0);
        updateLabel(_voiceElapsed, elapsed);
    }
}

void HosinoConsole::acceptSentDraft()
{
    if (!_sentRequestId || !_snapshot || _snapshot->accepted_send_request_id != _sentRequestId) return;
    if (!strcmp(_snapshot->selected_key, _sentSession)) {
        if (_view == View::Edit && !_viewPending && _edit == EditKind::Message && _textarea && !strcmp(_editSession, _sentSession)) {
            const char *editing = lv_textarea_get_text(_textarea);
            if (!strcmp(editing, _sentDraft)) lv_textarea_set_text(_textarea, "");
            // A user can type the next message while the previous POST is in flight.
            copy_text(_draft, sizeof(_draft), lv_textarea_get_text(_textarea));
        } else if (!strcmp(_draft, _sentDraft)) erase_text(_draft, sizeof(_draft));
    }
    _sentRequestId = 0;
    erase_text(_sentDraft, sizeof(_sentDraft)); erase_text(_sentSession, sizeof(_sentSession));
}

bool HosinoConsole::pause()
{
    if (_view == View::Pairing) { cancelPairingUi(); _nextView = View::Chat; _viewPending = true; }
    if (_view == View::ForgetDevice) { _confirmDeviceId[0] = 0; _nextView = View::Chat; _viewPending = true; }
    if (_client) _client->stopVoice(true);
    _voiceAwaiting = false;
    if (_view == View::Voice) { _nextView = View::Chat; _viewPending = true; }
    if (_view == View::ConfirmSend || _view == View::SendOptions || _view == View::ReviewSend) {
        erase_text(_confirmText, sizeof(_confirmText)); erase_text(_confirmSession, sizeof(_confirmSession));
        _nextView = View::Chat; _viewPending = true;
    }
    hosino_buttons_set_active(false);
    if (_buttonTimer) lv_timer_pause(_buttonTimer);
    if (_view == View::Edit) {
        preserveEditor();
        wipeEditor();
        _nextView = _edit == EditKind::Memory ? View::Memory : View::Chat;
        _viewPending = true;
    }
    _foreground = false;
    _requestPending = false;
    _selectingKey[0] = 0;
    if (_client && _clientReady) _client->setForeground(false);
    if (_timer) lv_timer_pause(_timer);
    _inputPending = false;
    return true;
}

bool HosinoConsole::resume()
{
    _foreground = _screen && lv_obj_is_valid(_screen) && lv_screen_active() == _screen;
    if (!_foreground) return false;
    if (_client && _clientReady) _client->setForeground(true);
    if (_timer) { lv_timer_resume(_timer); lv_timer_ready(_timer); }
    if (_buttonsReady && _buttonTimer) {
        hosino_buttons_set_active(true);
        lv_timer_resume(_buttonTimer);
        lv_timer_ready(_buttonTimer);
    }
    _renderPending = true;
    return true;
}

bool HosinoConsole::back()
{
    if (!canNavigate()) return false;
    if (_view == View::ConfirmSend || _view == View::SendOptions) {
        erase_text(_confirmText, sizeof(_confirmText)); erase_text(_confirmSession, sizeof(_confirmSession));
        queueView(View::Edit); return true;
    }
    if (_view == View::Pairing) { queueView(View::Chat); return true; }
    if (_view == View::ForgetDevice) { _confirmDeviceId[0] = 0; queueView(View::DeviceIdentity); return true; }
    if (_view == View::DeviceIdentity) { queueView(View::ChatMenu); return true; }
    if (_view == View::ReviewSend) {
        erase_text(_confirmSession, sizeof(_confirmSession)); queueView(View::Chat); return true;
    }
    if (_view == View::Voice) { queueView(View::Chat); return true; }
    if (_view == View::Edit) {
        preserveEditor();
        wipeEditor();
        queueView(_edit == EditKind::Memory ? View::Memory : View::Chat);
        return true;
    }
    if (_view == View::Shortcuts) { queueView(View::ChatMenu); return true; }
    if (_view == View::Sessions || _view == View::ChatMenu) { queueView(View::Chat); return true; }
    if (_view != View::Chat && _view != View::Manage) { queueView(View::Manage); return true; }
    return notifyCoreClosed();
}

bool HosinoConsole::close()
{
    _foreground = false;
    _voiceAwaiting = false;
    _confirmDeviceId[0] = 0;
    hosino_buttons_set_active(false);
    if (_client) {
        _client->shutdown();
        _closingClient = true;
    }
    _clientReady = false;
    _requestPending = false;
    _selectingKey[0] = 0;
    _sentRequestId = 0;
    erase_text(_sentDraft, sizeof(_sentDraft)); erase_text(_sentSession, sizeof(_sentSession));
    erase_text(_confirmText, sizeof(_confirmText)); erase_text(_confirmSession, sizeof(_confirmSession));
    _viewPending = _inputPending = _renderPending = false;
    wipeEditor();
    destroyUi();
    if (_snapshot) {
        hosino_console::Client::freeSnapshot(_snapshot);
        _snapshot = nullptr;
    }
    if (_command) {
        erase_text(reinterpret_cast<char *>(_command), sizeof(*_command));
        heap_caps_free(_command);
        _command = nullptr;
    }
    _authedBefore = false;
    _screen = nullptr;
    return true;
}

void HosinoConsole::destroyKeyboard()
{
#if LV_USE_IME_PINYIN
    // LVGL's IME destructor owns both its keyboard and candidate panel.
    if (_ime) { lv_obj_delete(_ime); _ime = nullptr; _keyboard = nullptr; }
#endif
    if (_keyboard) { lv_obj_delete(_keyboard); _keyboard = nullptr; }
}

void HosinoConsole::wipeEditor()
{
    _passwordVisible = false;
    if (_textarea && lv_obj_is_valid(_textarea)) {
        if (_edit == EditKind::Password) lv_textarea_set_password_mode(_textarea, true);
        lv_textarea_set_text(_textarea, "");
    }
}

void HosinoConsole::preserveEditor()
{
    // A pending navigation has already saved the draft and may have wiped
    // the editor. A pause/back in that transition must not save an empty copy.
    if (_viewPending || _view != View::Edit || !_textarea || !lv_obj_is_valid(_textarea)) return;
    if (_edit == EditKind::Message) copy_text(_draft, sizeof(_draft), lv_textarea_get_text(_textarea));
    else if (_edit == EditKind::Memory) copy_text(_query, sizeof(_query), lv_textarea_get_text(_textarea));
    // Passwords are never copied into a draft or shortcut state.
}

void HosinoConsole::destroyUi()
{
    if (_buttonTimer) {
        // Keep callback and user_data intact for Brookesia's ownership matcher.
        lv_timer_pause(_buttonTimer);
        _buttonTimer = nullptr;
    }
    _buttonsReady = false;
    if (_timer) {
        // Brookesia records run()-created timers and owns their final deletion.
        lv_timer_pause(_timer);
        _timer = nullptr;
    }
    destroyKeyboard();
    if (_root) { lv_obj_delete(_root); _root = nullptr; }
    _status = _list = _textarea = _passwordInfo = _passwordReveal = _voiceElapsed = _pairCountdown = nullptr;
    _bindingCount = 0;
}

void HosinoConsole::setNotice(const char *text)
{
    _noticeIsLogin = false;
    copy_text(_notice, sizeof(_notice), text);
    if (_view == View::Edit && _status && lv_obj_is_valid(_status)) lv_label_set_text(_status, _notice);
    // Status changes update the existing label; they never rebuild hit targets.
}

void HosinoConsole::queueView(View view)
{
    if (_view == View::Pairing && view != View::Pairing &&
        !(_snapshot && _snapshot->authed && _snapshot->device_authenticated &&
          _snapshot->pairing_state == hosino_console::PairState::Ready)) cancelPairingUi();
    if (_view == View::Voice && view != View::Voice && _voiceAwaiting) {
        if (_client) _client->stopVoice(true);
        _voiceAwaiting = false;
    }
    _nextView = view;
    _viewPending = true;
    _savedScroll = 0;
    if (view == View::Chat) _followChat = true;
}

lv_obj_t *HosinoConsole::label(lv_obj_t *parent, const char *text, int x, int y, int w,
                             const lv_font_t *font, uint32_t color)
{
    auto *object = lv_label_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, w);
    lv_label_set_long_mode(object, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_label_set_text(object, text ? text : "");
    return object;
}

lv_obj_t *HosinoConsole::button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                              Action action, unsigned index, bool enabled, bool accent)
{
    auto *object = lv_button_create(parent);
    lv_obj_set_pos(object, x, y); lv_obj_set_size(object, w, h);
    lv_obj_set_style_radius(object, 10, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 4, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(accent ? kAccent : kSurface), 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x354c67), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(object, LV_OPA_50, LV_STATE_DISABLED);
    auto *title = label(object, text, 0, 0, w - 12, &hosino_chat_font_16);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(title);
    if (_bindingCount < kBindings) {
        auto &binding = _bindings[_bindingCount++];
        binding = {this, action, index, object, enabled};
        lv_obj_add_event_cb(object, eventCallback, LV_EVENT_CLICKED, &binding);
    } else enabled = false;
    if (actionBlocked(action)) enabled = false;
    if (!enabled) lv_obj_add_state(object, LV_STATE_DISABLED);
    return object;
}

lv_obj_t *HosinoConsole::scrollList(int y, int bottomReserve)
{
    _list = lv_obj_create(_root);
    lv_obj_remove_style_all(_list);
    lv_obj_set_pos(_list, 0, y);
    lv_obj_set_size(_list, _width, std::max(1, _height - y - bottomReserve));
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(_list, 8, 0);
    lv_obj_set_style_pad_right(_list, 4, 0);
    lv_obj_set_scroll_dir(_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    return _list;
}

void HosinoConsole::detailCard(lv_obj_t *parent, const char *title, const char *body, bool secondary)
{
    auto *card = lv_obj_create(parent);
    lv_obj_set_size(card, _width - 6, LV_SIZE_CONTENT);
    const bool chat = _view == View::Chat;
    lv_obj_set_style_pad_all(card, chat ? 6 : 12, 0); lv_obj_set_style_pad_row(card, chat ? 3 : 7, 0);
    lv_obj_set_style_radius(card, 10, 0); lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(secondary ? 0x1c3141 : kSurface), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    auto *caption = label(card, title, 0, 0, _width - (chat ? 20 : 36), &hosino_chat_font_16, 0x72c5e8);
    if (chat) { lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_DOTS); lv_obj_set_height(caption, 18); }
    // Format only a display copy. Stored history and outgoing text stay UTF-8
    // originals; combined Emoji are shown as their monochrome components.
    const size_t displayBytes = hosino_chat_format_display(nullptr, 0, body);
    char *display = displayBytes < 16384 ? static_cast<char *>(heap_caps_malloc(
        displayBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) : nullptr;
    if (display) hosino_chat_format_display(display, displayBytes + 1, body);
    label(card, display ? display : body, 0, 0, _width - (chat ? 20 : 36), &hosino_chat_font_16);
    heap_caps_free(display);
}

void HosinoConsole::render()
{
    if (!canNavigate()) return;
    destroyKeyboard();
    lv_obj_clean(_root);
    _textarea = _status = _list = _passwordInfo = _passwordReveal = _voiceElapsed = _pairCountdown = nullptr;
    _bindingCount = 0;
    _renderPending = false;
    _renderedContent = contentSignature();
    if (_view == View::Edit) { renderEditor(); return; }
    if (_view == View::SendOptions) { renderSendOptions(); return; }
    if (_view == View::ConfirmSend) { renderSendConfirmation(); return; }
    if (_view == View::ReviewSend) { renderSendReview(); return; }
    if (_view == View::Pairing) { renderPairing(); return; }
    if (_view == View::DeviceIdentity || _view == View::ForgetDevice) { renderDeviceIdentity(_view == View::ForgetDevice); return; }
    if (_view == View::Voice) {
        renderVoice(); return;
    }
    if (_view == View::Shortcuts || _view == View::ChatMenu) {
        if (_view == View::Shortcuts) renderShortcuts();
        else renderChatMenu();
        return;
    }
    if (_snapshot && _client && _clientReady && _snapshot->authed &&
        (_view == View::Chat || _view == View::Sessions)) {
        if (_view == View::Chat) renderChat(0);
        else renderSessions(0);
        lv_obj_update_layout(_list);
        if (_view == View::Chat && _followChat) lv_obj_scroll_to_y(_list, lv_obj_get_scroll_bottom(_list), LV_ANIM_OFF);
        else if (_savedScroll > 0) lv_obj_scroll_to_y(_list, _savedScroll, LV_ANIM_OFF);
        return;
    }
    label(_root, "星野", 0, 10, 84, &hosino_font_20);
    button(_root, "对话", _width - 246, 0, 118, kHeader, Action::Chat, 0, true,
           _view == View::Chat || _view == View::Sessions);
    button(_root, "管理", _width - 118, 0, 118, kHeader, Action::Manage, 0, true,
           _view != View::Chat && _view != View::Sessions);
    const char *status = _view == View::Chat && _snapshot && _snapshot->send_uncertain
                         ? "上次发送未确认，请核对历史，勿重复发送"
                         : _notice[0] ? _notice : (_snapshot && _snapshot->status[0] ? _snapshot->status : "连接状态待读取");
    _status = label(_root, status, 0, 50, _width, &hosino_chat_font_16, kMuted);
    lv_label_set_long_mode(_status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(_status, 20);
    if (!_snapshot || !_client || !_clientReady) {
        detailCard(scrollList(kBody, 0), _closingClient ? "正在清理连接" : "暂时无法打开",
                   _closingClient ? "网络任务正在安全退出。完成后自动重新连接，返回桌面无需等待。" :
                   "请关闭其他应用释放内存，再重新打开星野控制台。");
        return;
    }
    if (!_snapshot->authed) {
        label(_root, "连接你的星野", 0, 96, _width, &hosino_font_20);
        label(_root, "在网页批准设备，无需在小屏输入密码。", 0, 136, _width, &hosino_chat_font_16, kMuted);
        button(_root, "设备配对 · 网页批准", 0, 188, _width, 48, Action::PairStart, 0, true, true);
        button(_root, "输入密码登录（后备）", 0, 246, _width, 42, Action::Login);
        button(_root, _snapshot->remember_busy ? "正在保存登录设置…" :
               _snapshot->remember_error != ESP_OK ? "登录设置保存失败 · 点此重试" :
               _client->rememberLogin() ? "[已勾选] 密码登录记住凭证" : "[未勾选] 密码登录记住凭证", 0, 298,
               _width, 36, Action::RememberLogin);
        label(_root, "可上滑返回桌面", 0, 342, _width, &hosino_chat_font_16, kMuted);
        if (_snapshot->retry_after_ms) {
            char text[80]; snprintf(text, sizeof(text), "请约 %lu 秒后再试", static_cast<unsigned long>((_snapshot->retry_after_ms + 999) / 1000));
            label(_root, text, 0, 366, _width, &hosino_chat_font_16, kMuted);
        }
        return;
    }
    if (_view == View::Chat) renderChat(kBody);
    else if (_view == View::Sessions) renderSessions(kBody);
    else if (_view == View::Manage) renderManage(kBody);
    else renderDetail(kBody);
    if (_list) {
        lv_obj_update_layout(_list);
        if (_view == View::Chat && _followChat) lv_obj_scroll_to_y(_list, lv_obj_get_scroll_bottom(_list), LV_ANIM_OFF);
        else if (_savedScroll > 0) lv_obj_scroll_to_y(_list, _savedScroll, LV_ANIM_OFF);
    }
}

void HosinoConsole::renderChat(int y)
{
    const bool selected = _snapshot->selected;
    char heading[200];
    if (selected) snprintf(heading, sizeof(heading), "%s%s · %s",
        _snapshot->send_available ? "" : "暂不可发送 · ",
        !strcmp(_snapshot->selected_channel, "dashboard") ? "设备" :
            (_snapshot->selected_channel[0] ? _snapshot->selected_channel : "未知渠道"),
        _snapshot->selected_name[0] ? _snapshot->selected_name : "当前会话");
    else copy_text(heading, sizeof(heading), "选择会话后开始对话");
    auto *sessionButton = button(_root, heading, 0, y, _width - 58, 34,
           Action::Sessions, 0, _snapshot->authed);
    lv_obj_set_style_text_align(lv_obj_get_child(sessionButton, 0), LV_TEXT_ALIGN_LEFT, 0);
    button(_root, "菜单", _width - 52, y, 52, 34, Action::ChatMenu);
    _status = label(_root, "", 2, y + 36, _width - 4, &hosino_chat_font_16, kMuted);
    lv_label_set_long_mode(_status, LV_LABEL_LONG_MODE_DOTS); lv_obj_set_height(_status, 18);
    auto *list = scrollList(y + 54, 42);
    lv_obj_set_style_pad_row(list, 6, 0);
    if (!selected) detailCard(list, "开始前先选会话", "这里不会自动创建会话或发送消息。请核对会话名称和渠道后再发送。");
    else if (!_snapshot->message_count) detailCard(list, "最近消息", "暂无可显示消息，可在菜单中刷新。");
    else {
        for (unsigned i = 0; i < _snapshot->message_count && i < hosino_console::kMessageCount; ++i) {
            const auto &message = _snapshot->messages[i];
            const bool user = !strcmp(message.role, "user");
            const char *role = user ? "我" : !strcmp(message.role, "assistant") ? "星野" : message.role;
            detailCard(list, message.sender[0] ? message.sender : role, message.text, user);
            if (message.truncated) label(list, "长消息已截取，完整内容请到控制台查看", 0, 0, _width - 8, &hosino_chat_font_16, kMuted);
        }
        if (_snapshot->history_has_more) label(list, "显示最近消息", 0, 0, _width - 8, &hosino_chat_font_16, kMuted);
    }
    button(_root, "输入消息…", 0, _height - 40, _width - 78, 40,
           Action::Compose, 0, selected && _snapshot->send_available, true);
    button(_root, "语音", _width - 72, _height - 40, 72, 40, Action::Voice, 0,
           selected && _snapshot->send_available);
}

void HosinoConsole::renderChatMenu()
{
    const bool authed = _clientReady && _snapshot && _snapshot->authed;
    const bool selected = authed && _snapshot->selected;
    const bool idle = authed;
    button(_root, "返回", 0, 0, 84, 40, Action::Back);
    label(_root, "对话菜单", 98, 10, _width - 100, &hosino_chat_font_16);
    char channel[160];
    snprintf(channel, sizeof(channel), "渠道：%s%s", selected ?
        (_snapshot->selected_channel[0] ? _snapshot->selected_channel : "未知") : "未选择",
        selected && !_snapshot->send_available ? " · 暂不可发送" : "");
    auto *context = label(_root, channel, 0, 48, _width, &hosino_chat_font_16, kMuted);
    lv_label_set_long_mode(context, LV_LABEL_LONG_MODE_DOTS);
    const int w = (_width - 10) / 2;
    button(_root, "切换会话", 0, 78, w, 48, Action::Sessions, 0, authed);
    button(_root, "新建设备会话", w + 10, 78, w, 48, Action::CreateSession, 0, idle);
    button(_root, "刷新消息", 0, 136, w, 48, Action::Refresh, 0, selected && idle);
    button(_root, "停止生成", w + 10, 136, w, 48, Action::Abort, 0,
           selected && _snapshot->send_available && _snapshot->selected_active);
    button(_root, "管理", 0, 194, w, 48, Action::Manage);
    button(_root, _snapshot && _snapshot->device_authenticated ? "解除本机授权" : "退出登录",
           w + 10, 194, w, 48, _snapshot && _snapshot->device_authenticated ? Action::ForgetDevice : Action::Logout, 0, idle);
    button(_root, "快捷键", 0, 252, w, 42, Action::Shortcuts);
    if (_snapshot && _snapshot->device_saved) {
        button(_root, _snapshot->device_label[0] ? _snapshot->device_label : "设备身份", w + 10, 252, w, 42, Action::DeviceIdentity);
    } else {
        button(_root, _snapshot && _snapshot->remember_busy ? "登录设置保存中…" :
               _snapshot && _snapshot->remember_error != ESP_OK ? "登录设置重试" :
               _client && _client->rememberLogin() ? "密码记住：开" : "密码记住：关",
               w + 10, 252, w, 42, Action::RememberLogin, 0, _client && _snapshot);
    }
    const char *notice = !_buttonsReady ? "实体键暂不可用，请继续使用触屏" :
        authed && _snapshot->send_uncertain ? "上次发送未确认，请核对历史，勿重复发送。" :
        _notice[0] ? _notice : authed ? _snapshot->status : "请返回对话页登录";
    if (authed && _snapshot->send_uncertain) {
        button(_root, "核对上次发送结果", 0, 304, _width, 40, Action::ReviewSend, 0, true, true);
        _status = label(_root, notice, 0, 350, _width, &hosino_chat_font_16, kMuted);
    } else _status = label(_root, notice, 0, 304, _width, &hosino_chat_font_16, kMuted);
}

void HosinoConsole::loadShortcuts()
{
    if (_keysLoaded) return;
    _keysLoaded = true;
    nvs_handle_t handle;
    esp_err_t result = nvs_open("hs_keys", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return;
    if (result != ESP_OK) {
        copy_text(_keyNotice, sizeof(_keyNotice), "快捷键设置读取失败，使用默认设置");
        return;
    }
    uint8_t mapping[3] = {};
    size_t size = sizeof(mapping);
    result = nvs_get_blob(handle, "map", mapping, &size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return;
    bool valid = result == ESP_OK && size == sizeof(mapping);
    for (const uint8_t value : mapping) valid = valid && value < static_cast<uint8_t>(KeyAction::Count);
    if (!valid) {
        copy_text(_keyNotice, sizeof(_keyNotice), "快捷键存储无效，使用默认设置");
        return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        _savedKeyActions[i] = _keyActions[i] = static_cast<KeyAction>(mapping[i]);
    }
}

void HosinoConsole::saveShortcuts()
{
    uint8_t mapping[3];
    for (unsigned i = 0; i < 3; ++i) mapping[i] = static_cast<uint8_t>(_keyActions[i]);
    nvs_handle_t handle;
    esp_err_t result = nvs_open("hs_keys", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, "map", mapping, sizeof(mapping));
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result == ESP_OK) {
        for (unsigned i = 0; i < 3; ++i) _savedKeyActions[i] = _keyActions[i];
        _keysDirty = false;
        copy_text(_keyNotice, sizeof(_keyNotice), "快捷键已保存，重启后仍然生效");
    } else {
        copy_text(_keyNotice, sizeof(_keyNotice), "保存失败；当前修改仅在本次运行生效，可重试保存");
    }
    _renderPending = true;
}

void HosinoConsole::renderShortcuts()
{
    static constexpr const char *names[] = {"电源键", "BOOT 键", "自定义键"};
    static constexpr const char *actions[] = {"无", "返回", "菜单", "编写", "语音"};
    button(_root, "返回", 0, 0, 84, 40, Action::Back);
    label(_root, "快捷键", 98, 10, _width - 100, &hosino_chat_font_16);
    const char *notice = !_buttonsReady ? "实体键暂不可用，请继续使用触屏" :
        _keyNotice[0] ? _keyNotice : "点击切换功能，立即生效；保存后重启保留。";
    label(_root, notice, 0, 48, _width, &hosino_chat_font_16, kMuted);
    for (unsigned i = 0; i < 3; ++i) {
        char text[80];
        snprintf(text, sizeof(text), "%s：%s", names[i], actions[static_cast<unsigned>(_keyActions[i])]);
        button(_root, text, 0, 100 + static_cast<int>(i) * 50, _width, 44, Action::KeyCycle, i);
    }
    label(_root, "仅短按有效；长按电源仍保留硬件电源功能。", 0, 252, _width, &hosino_chat_font_16, kMuted);
    button(_root, _keysDirty ? "保存设置 · 尚未保存" : "保存设置", 0, _height - 44, _width, 42, Action::KeySave, 0, true, true);
}

void HosinoConsole::onHardwareButton(hosino_button_id_t button)
{
    if (!canNavigate() || !_buttonsReady || !_buttonTimer || _viewPending) return;
    const unsigned index = static_cast<unsigned>(button);
    if (index >= 3) return;
    const KeyAction action = _keyActions[index];
    if (action == KeyAction::None) return;
    if (action == KeyAction::Back) { onAction(Action::Back, 0); return; }
    if (action == KeyAction::Voice && _view == View::Voice) { onVoiceRequested(); return; }
    if (action == KeyAction::Voice && _view == View::Edit && _edit == EditKind::Password) {
        setNotice("密码页面不启用语音，请使用键盘输入");
        return;
    }
    if (action == KeyAction::Compose || action == KeyAction::Voice) {
        if (!_clientReady || !_snapshot || !_snapshot->authed || !_snapshot->selected ||
            !_snapshot->send_available ||
            actionBlocked(action == KeyAction::Compose ? Action::Compose : Action::Voice)) {
            setNotice("请先选择可发送的会话，等待当前操作完成");
            return;
        }
    }
    if (_view == View::Edit && action != KeyAction::Voice) {
        preserveEditor();
        wipeEditor();
        _inputPending = false;
    }
    if (action == KeyAction::Menu) onAction(Action::ChatMenu, 0);
    else if (action == KeyAction::Compose) onAction(Action::Compose, 0);
    else if (action == KeyAction::Voice) onAction(Action::Voice, 0);
}

void HosinoConsole::onVoiceRequested()
{
    using hosino_console::VoiceState;
    if (!_client || !_snapshot) return;
    if (_view == View::Voice && _voiceAwaiting) {
        if (_snapshot->voice_state == VoiceState::Recording) {
            _client->stopVoice(false);
        }
        return;
    }
    if (!_snapshot->authed || !_snapshot->selected || !_snapshot->send_available ||
        pendingWrite() || !selectionReady()) {
        setNotice("请先选择可发送的会话，等待当前操作完成"); return;
    }
    preserveEditor();
    if (submit(hosino_console::CommandType::TranscribeMic)) {
        _voiceResultSeen = _snapshot->voice_result_revision;
        copy_text(_voiceKey, sizeof(_voiceKey), _snapshot->selected_key);
        _voiceAwaiting = true;
        queueView(View::Voice);
    }
}

void HosinoConsole::renderVoice()
{
    using hosino_console::VoiceState;
    button(_root, "返回", 0, 0, 84, 40, Action::Back);
    label(_root, "语音输入", 98, 10, _width - 100, &hosino_chat_font_16);
    const VoiceState state = _snapshot ? _snapshot->voice_state : VoiceState::Idle;
    const bool recording = _voiceAwaiting && state == VoiceState::Recording;
    const bool uploading = _voiceAwaiting && state == VoiceState::Uploading;
    const char *title = recording ? "正在聆听" : uploading ? "MiMo 正在识别" :
        _voiceAwaiting ? "正在准备麦克风" : "语音输入已结束";
    label(_root, title, 0, 76, _width, &hosino_chat_font_16);
    char elapsed[64];
    snprintf(elapsed, sizeof(elapsed), "%.1f / 15 秒", _snapshot ? _snapshot->voice_elapsed_ms / 1000.0 : 0.0);
    _voiceElapsed = label(_root, elapsed, 0, 114, _width, &hosino_font_20);
    _status = label(_root, _notice[0] ? _notice : _snapshot && _snapshot->status[0] ? _snapshot->status : "请稍候…",
          0, 152, _width, &hosino_chat_font_16, kMuted);
    label(_root, "识别结果加入草稿，核对后再发送。\n取消或返回会丢弃本次录音。", 0, 210,
          _width, &hosino_chat_font_16, kMuted);
    const int w = (_width - 10) / 2;
    button(_root, recording ? "结束并识别" : uploading ? "正在识别…" : "请稍候…", 0, _height - 56,
           w, 48, Action::VoiceFinish, 0, recording, true);
    button(_root, _voiceAwaiting ? "取消" : "回到对话", w + 10, _height - 56, w, 48, Action::VoiceCancel);
}

void HosinoConsole::renderSessions(int y)
{
    button(_root, "返回", 0, y, 64, 36, Action::Back);
    label(_root, "选择会话", 76, y + 8, _width - 204, &hosino_chat_font_16);
    button(_root, "新建会话", _width - 116, y, 116, 36, Action::CreateSession, 0, true, true);
    _status = label(_root, "", 2, y + 40, _width - 4, &hosino_chat_font_16, kMuted);
    lv_label_set_long_mode(_status, LV_LABEL_LONG_MODE_DOTS); lv_obj_set_height(_status, 18);
    auto *list = scrollList(y + 60, 44);
    if (!_snapshot->session_count) detailCard(list, "会话列表", "没有可显示会话，可新建或刷新。");
    for (unsigned i = 0; i < _snapshot->session_count && i < hosino_console::kSessionCount; ++i) {
        const auto &session = _snapshot->sessions[i];
        char text[192]; snprintf(text, sizeof(text), "%s\n渠道：%s%s", session.name[0] ? session.name : "未命名会话",
                                session.channel[0] ? session.channel : "未提供", session.active ? " · 运行中" : "");
        // Selecting is local intent + a read, not permission to send to this
        // channel. The client's generation fence cancels the previous GET.
        auto *row = button(list, text, 0, 0, _width - 6, 58, Action::SelectSession, i,
                           _snapshot->authed && session.key[0]);
        auto *caption = lv_obj_get_child(row, 0);
        lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_LEFT, 0);
    }
    button(_root, "上一页", 0, _height - 40, 118, 40, Action::Prev, 0, _snapshot->session_offset > 0);
    button(_root, "刷新", 126, _height - 40, _width - 252, 40, Action::Refresh);
    button(_root, "下一页", _width - 118, _height - 40, 118, 40, Action::Next, 0, _snapshot->sessions_have_more);
}

void HosinoConsole::renderManage(int y)
{
    const int w = (_width - 10) / 2;
    button(_root, "运行总览", 0, y, w, 64, Action::Overview);
    button(_root, "模型列表", w + 10, y, w, 64, Action::Models);
    button(_root, "用量查询", 0, y + 74, w, 64, Action::Usage);
    button(_root, "记忆搜索", w + 10, y + 74, w, 64, Action::Memory);
    label(_root, _snapshot->quiet ? "当前处于服务静默窗口，写操作暂不可用" : "显示服务返回的数据；暂不修改服务器配置。",
          0, y + 154, _width, &hosino_chat_font_16, kMuted);
    button(_root, "刷新状态", 0, _height - 48, w, 48, Action::Refresh);
    button(_root, "退出登录", w + 10, _height - 48, w, 48, Action::Logout);
}

void HosinoConsole::renderDetail(int y)
{
    button(_root, "返回", 0, y, 90, 44, Action::Back);
    const char *title = _view == View::Overview ? "运行总览" : _view == View::Models ? "模型列表" : _view == View::Usage ? "用量查询" : "记忆搜索";
    label(_root, title, 102, y + 12, _width - 200, &hosino_font_18);
    button(_root, _view == View::Memory ? "搜索" : "刷新", _width - 90, y, 90, 44,
           _view == View::Memory ? Action::Search : Action::Refresh);
    auto *list = scrollList(y + 54, 0);
    if (_view == View::Overview) detailCard(list, "服务与主机", _snapshot->overview_available ? _snapshot->overview : "尚无总览数据，请刷新。缺失字段不会显示为零。");
    else if (_view == View::Usage) detailCard(list, "供应商用量", _snapshot->usage_available ? _snapshot->usage : "尚无用量数据，请刷新。不同供应商可能不提供相同统计。");
    else if (_view == View::Models) {
        if (!_snapshot->models_available) detailCard(list, "模型", "暂无可用列表，请刷新。");
        for (unsigned i = 0; i < _snapshot->model_count && i < hosino_console::kModelCount; ++i) {
            const auto &model = _snapshot->models[i];
            const char *availability = !strcmp(model.availability, "available") ? "可用" : !strcmp(model.availability, "unavailable") ? "不可用" : "状态未知";
            char body[256]; snprintf(body, sizeof(body), "%s\n供应商：%s\n%s", model.id, model.provider[0] ? model.provider : "未提供", availability);
            detailCard(list, model.name[0] ? model.name : model.id, body);
        }
        label(list, _snapshot->models_truncated ? "列表已截取，完整内容请到控制台查看" : "此页仅查看模型，不修改当前会话",
              0, 0, _width - 8, &hosino_chat_font_16, kMuted);
    } else {
        if (!_snapshot->memory_available) detailCard(list, "搜索记忆", "输入关键词搜索。查询范围由服务接口决定，结果按服务返回显示。");
        else if (!_snapshot->memory_count) detailCard(list, "搜索结果", "没有匹配的记忆。");
        for (unsigned i = 0; i < _snapshot->memory_count && i < hosino_console::kMemoryCount; ++i) {
            const auto &memory = _snapshot->memories[i];
            char caption[128];
            if (memory.has_score) snprintf(caption, sizeof(caption), "%s · %.3f", memory.user[0] ? memory.user : "记忆", static_cast<double>(memory.score));
            else copy_text(caption, sizeof(caption), memory.user[0] ? memory.user : "记忆");
            detailCard(list, caption, memory.text);
            if (memory.truncated) label(list, "内容已截取", 0, 0, _width - 8, &hosino_chat_font_16, kMuted);
        }
    }
}

void HosinoConsole::showEditor(EditKind kind)
{
    _edit = kind;
    _passwordPage = 0;
    _passwordVisible = false;
    _nextInputMode = kind == EditKind::Password ? InputMode::English : InputMode::Chinese;
    if (kind == EditKind::Message && _snapshot) copy_text(_editSession, sizeof(_editSession), _snapshot->selected_key);
    else _editSession[0] = 0;
    queueView(View::Edit);
}

void HosinoConsole::renderSendOptions()
{
    button(_root, "返回", 0, 0, 64, 36, Action::Back);
    label(_root, "发送方式", 78, 8, _width - 80, &hosino_chat_font_16);
    const bool selected = _snapshot && _snapshot->authed && _snapshot->selected;
    const bool qq = selected && !strcmp(_snapshot->selected_channel, "qqbot");
    const bool ai = selected && _snapshot->send_available;
    label(_root, selected ? _snapshot->selected_name : "未选择会话", 0, 48, _width, &hosino_chat_font_16, kMuted);
    button(_root, "AI·设备：不自动投递到渠道", 0, 96, _width, 48, Action::ChooseDeviceAI, 0, ai, _sendMode == SendMode::DeviceAI);
    button(_root, "AI·QQ：让 AI 回复到 QQ", 0, 154, _width, 48, Action::ChooseChannelAI, 0, qq && ai, _sendMode == SendMode::ChannelAI);
    button(_root, qq && _snapshot->qq_send_available ? "直发QQ：直接发送这段文字" : "直发QQ：当前未启用",
           0, 212, _width, 48, Action::ChooseDirectQQ, 0,
           qq && _snapshot->qq_send_available, _sendMode == SendMode::DirectQQ);
    label(_root, "AI·设备不自动投递到渠道，但不会限制模型工具。\nAI·QQ和直发QQ发送前都需要确认。",
          0, 276, _width, &hosino_chat_font_16, kMuted);
    if (_snapshot && _snapshot->send_uncertain)
        button(_root, "核对上次发送结果", 0, _height - 40, _width, 40, Action::ReviewSend, 0, true, true);
    else _status = label(_root, "", 0, _height - 36, _width, &hosino_chat_font_16, kMuted);
}

void HosinoConsole::renderSendConfirmation()
{
    button(_root, "返回", 0, 0, 64, 36, Action::Back);
    label(_root, _sendMode == SendMode::DirectQQ ? "确认直发 QQ" : "确认让 AI 回复 QQ",
          78, 8, _width - 80, &hosino_chat_font_16);
    const bool same = _snapshot && _snapshot->authed && _snapshot->selected &&
        !strcmp(_snapshot->selected_key, _confirmSession) && !strcmp(_snapshot->selected_channel, "qqbot");
    char target[160]; snprintf(target, sizeof(target), "目标：%s · QQ", same ? _snapshot->selected_name : "会话已改变");
    label(_root, target, 0, 48, _width, &hosino_chat_font_16);
    _status = label(_root, "", 0, 90, _width, &hosino_chat_font_16, kMuted);
    lv_label_set_long_mode(_status, LV_LABEL_LONG_MODE_DOTS); lv_obj_set_height(_status, 18);
    detailCard(scrollList(114, 62), "发送预览", _confirmText);
    const bool allowed = same && !_snapshot->send_uncertain &&
        (_sendMode == SendMode::DirectQQ ? _snapshot->qq_send_available : _snapshot->send_available);
    button(_root, "确认发送到 QQ", 0, _height - 48, _width, 48, Action::ConfirmSend, 0, allowed, true);
}

void HosinoConsole::renderSendReview()
{
    button(_root, "返回", 0, 0, 64, 36, Action::Back);
    label(_root, "核对发送结果", 78, 8, _width - 80, &hosino_chat_font_16);
    const bool same = _snapshot && _snapshot->authed && _snapshot->selected &&
        !strcmp(_snapshot->selected_key, _confirmSession);
    const bool hasResult = same && !strcmp(_snapshot->selected_key, _snapshot->send_result_key);
    auto *list = scrollList(46, 108);
    detailCard(list, same ? _snapshot->selected_name : "会话已改变",
        hasResult && _snapshot->send_status[0] ? _snapshot->send_status : "上次发送未获成功确认，草稿已保留。");
    if (hasResult && _snapshot->send_http_status) {
        char code[64]; snprintf(code, sizeof(code), "HTTP %d · %s", _snapshot->send_http_status,
                              _snapshot->send_error_code);
        label(list, code, 0, 0, _width - 8, &hosino_chat_font_16, kMuted);
    }
    detailCard(list, "先核对，再恢复", "请先查看 QQ 或会话历史，确认上次是否送达。恢复只解除发送限制，不会发送或重试草稿；编辑、语音和返回仍可使用。");
    button(_root, "返回查看会话历史", 0, _height - 98, _width, 42, Action::Refresh, 0, same);
    button(_root, "已核对，恢复发送", 0, _height - 48, _width, 48, Action::ResolveSend, 0,
           canResolveSend(), true);
}

void HosinoConsole::renderPairing()
{
    button(_root, "取消", 0, 0, 64, 36, Action::PairCancel);
    label(_root, "设备配对", 82, 8, _width - 84, &hosino_chat_font_16);
    label(_root, "用电脑或手机打开控制台网页，批准这台设备。", 0, 50, _width, &hosino_chat_font_16, kMuted);
    const bool waiting = _snapshot && _snapshot->pairing_state == hosino_console::PairState::Waiting;
    auto *code = label(_root, waiting && _snapshot->user_code[0] ? _snapshot->user_code : "---- ----",
                       0, 102, _width, &lv_font_montserrat_40);
    lv_obj_set_style_text_align(code, LV_TEXT_ALIGN_CENTER, 0);
    label(_root, "hosino.ume.wiki/pair.html\n网页登录后输入上方短码", 0, 160, _width, &hosino_chat_font_16);
    _pairCountdown = label(_root, "", 0, 214, _width, &hosino_chat_font_16, kMuted);
    _status = label(_root, "", 0, 246, _width, &hosino_chat_font_16, kMuted);
    lv_obj_set_height(_status, 76);
    button(_root, "重新获取配对码", 0, _height - 48, _width, 48, Action::PairStart, 0, true, true);
}

void HosinoConsole::renderDeviceIdentity(bool confirm)
{
    button(_root, "返回", 0, 0, 64, 36, Action::Back);
    label(_root, confirm ? "解除本机授权" : "设备身份", 82, 8, _width - 84, &hosino_chat_font_16);
    auto *list = scrollList(48, 62);
    const bool saved = _snapshot && _snapshot->device_saved;
    detailCard(list, saved && _snapshot->device_label[0] ? _snapshot->device_label : "这台设备",
               saved && _snapshot->device_id[0] ? _snapshot->device_id : "未保存设备身份");
    if (confirm) {
        detailCard(list, "确认仅清除本机", "将删除这台设备保存的授权凭证，需要重新配对才能使用设备授权。服务器上的设备记录不会在此吊销；需要吊销时请到网页登录后操作。");
        button(_root, "确认解除本机授权", 0, _height - 48, _width, 48, Action::ConfirmForgetDevice, 0, true, true);
    } else {
        detailCard(list, "授权状态", saved ? (_snapshot->device_authenticated ?
                   "正在使用设备授权。凭证自动保存在本机，不需要记住密码。" :
                   "本机保存了设备凭证，当前未以此身份连接。") : "本机没有设备凭证。");
        button(_root, "解除本机授权…", 0, _height - 48, _width, 48, Action::ForgetDevice, 0, saved);
    }
}

void HosinoConsole::renderEditor()
{
    if (_edit == EditKind::Password) { renderPasswordEditor(); return; }
    button(_root, "返回", 0, 0, 66, 36, Action::Back);
    char context[160];
    if (_edit == EditKind::Memory) copy_text(context, sizeof(context), "搜索记忆");
    else snprintf(context, sizeof(context), "%s · %s",
        !strcmp(_snapshot->selected_channel, "dashboard") ? "设备" : _snapshot->selected_channel, _snapshot->selected_name);
    if (_edit == EditKind::Message) {
        auto *mode = button(_root, _sendMode == SendMode::DeviceAI ? "AI·设备" : _sendMode == SendMode::ChannelAI ? "AI·QQ" : "直发QQ",
                            74, 0, _width - 162, 36, Action::SendOptions);
        _status = lv_obj_get_child(mode, 0);
    } else {
        _status = label(_root, context, 74, 8, _width - 162, &hosino_chat_font_16);
        lv_label_set_long_mode(_status, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(_status, 24);
    }
    button(_root, _edit == EditKind::Memory ? "搜索" : "发送",
           _width - 80, 0, 80, 36, Action::EditAccept, 0, true, true);
    _textarea = lv_textarea_create(_root);
    lv_obj_set_pos(_textarea, 0, 40); lv_obj_set_size(_textarea, _width, 44);
    lv_obj_set_style_text_font(_textarea, &hosino_chat_font_16, 0);
    lv_obj_set_style_text_font(_textarea, &hosino_chat_font_16, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_color(_textarea, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_color(_textarea, lv_color_hex(kSurface), 0);
    lv_obj_set_style_border_color(_textarea, lv_color_hex(kAccent), 0);
    lv_obj_set_style_pad_all(_textarea, 6, 0);
    lv_textarea_set_max_length(_textarea, _edit == EditKind::Message ? hosino_console::kCommandTextSize - 1 : 120);
    lv_textarea_set_placeholder_text(_textarea, "全键拼音：输入拼音后点选汉字");
    lv_textarea_set_text(_textarea, _edit == EditKind::Message ? _draft : _query);
    const int modeWidth = (_width - 12) / 3;
    button(_root, "全键拼音", 0, 88, modeWidth, 30, Action::Chinese);
    button(_root, "英文", modeWidth + 6, 88, modeWidth, 30, Action::English);
    button(_root, "数字", (modeWidth + 6) * 2, 88, modeWidth, 30, Action::Number);
    createKeyboard(_nextInputMode);
}

void HosinoConsole::renderPasswordEditor()
{
    static const lv_font_t keyFont = [] {
        lv_font_t font = lv_font_montserrat_20;
        font.fallback = &hosino_chat_font_16;
        return font;
    }();
    button(_root, "返回", 0, 0, 84, 40, Action::Back);
    _status = label(_root, "登录星野", 96, 8, _width - 210, &hosino_chat_font_16);
    lv_obj_set_height(_status, 32);
    button(_root, "登录", _width - 104, 0, 104, 40, Action::EditAccept, 0, true, true);
    _textarea = lv_textarea_create(_root);
    lv_textarea_set_one_line(_textarea, true);
    lv_obj_set_pos(_textarea, 0, 46);
    lv_obj_set_size(_textarea, _width - 96, 48);
    lv_obj_set_style_pad_all(_textarea, 8, 0);
    lv_obj_set_style_text_font(_textarea, &hosino_chat_font_16, 0);
    lv_obj_set_style_text_font(_textarea, &hosino_chat_font_16, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_color(_textarea, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_color(_textarea, lv_color_hex(kSurface), 0);
    lv_obj_set_style_border_color(_textarea, lv_color_hex(kAccent), 0);
    lv_textarea_set_max_length(_textarea, 256);
    lv_textarea_set_password_mode(_textarea, true);
    lv_textarea_set_password_show_time(_textarea, 0);
    lv_textarea_set_placeholder_text(_textarea, "请输入密码");
    _passwordVisible = false;
    _passwordReveal = button(_root, "显示", _width - 88, 46, 88, 48, Action::PasswordVisibility);
    _passwordInfo = label(_root, "", 0, 98, _width, &hosino_chat_font_16, kMuted);
    lv_obj_set_height(_passwordInfo, 20);
    _keyboard = lv_keyboard_create(_root);
    // Only our handler inserts characters; the stock handler would insert control labels too.
    lv_obj_remove_event_cb(_keyboard, lv_keyboard_def_event_cb);
    lv_obj_set_align(_keyboard, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(_keyboard, 0, 120);
    lv_obj_set_size(_keyboard, _width, std::max(1, _height - 120));
    lv_obj_set_style_pad_all(_keyboard, 0, 0);
    lv_obj_set_style_pad_gap(_keyboard, 4, 0);
    lv_obj_set_style_border_width(_keyboard, 0, 0);
    lv_obj_set_style_bg_color(_keyboard, lv_color_hex(kBackground), 0);
    lv_obj_set_style_text_font(_keyboard, &keyFont, LV_PART_ITEMS);
    lv_obj_set_style_text_color(_keyboard, lv_color_hex(0xffffff), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(_keyboard, lv_color_hex(kSurface), LV_PART_ITEMS);
    lv_keyboard_set_textarea(_keyboard, _textarea);
    updatePasswordKeyboard();
    lv_obj_add_event_cb(_keyboard, keyboardCallback, LV_EVENT_ALL, this);
}

void HosinoConsole::updatePasswordKeyboard()
{
    const char *page = kPasswordPages[_passwordPage];
    const size_t count = strlen(page);
    unsigned mapIndex = 0;
    for (unsigned i = 0; i < 18; ++i) {
        _passwordKeys[i][0] = i < count ? page[i] : ' ';
        _passwordKeys[i][1] = 0;
        _passwordMap[mapIndex++] = _passwordKeys[i];
        if (i % 6 == 5) _passwordMap[mapIndex++] = "\n";
    }
    for (const char *key : {"Aa", "123", "符号", "空格", "翻页", LV_SYMBOL_BACKSPACE})
        _passwordMap[mapIndex++] = key;
    _passwordMap[mapIndex] = "";
    for (unsigned i = 0; i < 24; ++i) {
        _passwordControls[i] = static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_WIDTH_1 |
            LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
            (i < 18 && i >= count ? LV_BUTTONMATRIX_CTRL_DISABLED : 0));
    }
    lv_keyboard_set_map(_keyboard, LV_KEYBOARD_MODE_USER_1, _passwordMap, _passwordControls);
    lv_keyboard_set_mode(_keyboard, LV_KEYBOARD_MODE_USER_1);
    lv_label_set_text(_passwordInfo, kPasswordPageNames[_passwordPage]);
    _inputPending = false;
}

void HosinoConsole::onPasswordKey(unsigned index)
{
    if (_viewPending || _inputPending || !_textarea) return;
    if (index < 18) {
        const char *page = kPasswordPages[_passwordPage];
        if (index < strlen(page)) {
            const char character[2] = {page[index], 0};
            lv_textarea_add_text(_textarea, character);
        }
        return;
    }
    switch (index) {
    case 18: _passwordPage = _passwordPage < 2 ? _passwordPage + 2 : _passwordPage < 4 ? _passwordPage - 2 : 0; break;
    case 19: _passwordPage = 4; break;
    case 20: _passwordPage = 5; break;
    case 21: lv_textarea_add_text(_textarea, " "); return;
    case 22: _passwordPage = _passwordPage < 4 ? _passwordPage ^ 1u : _passwordPage == 5 ? 6 : _passwordPage == 6 ? 5 : 4; break;
    case 23: lv_textarea_delete_char(_textarea); return;
    default: return;
    }
    // Do not rebuild a button map from inside its VALUE_CHANGED callback.
    _inputPending = true;
}

void HosinoConsole::createKeyboard(InputMode mode)
{
    destroyKeyboard();
    _inputMode = mode;
    _inputPending = false;
    _keyboard = lv_keyboard_create(_root);
    // Keyboard defaults to BOTTOM_MID: set_pos alone would put it below this root.
    lv_obj_set_align(_keyboard, LV_ALIGN_TOP_LEFT);
    const int keyboardY = mode == InputMode::Chinese ? 160 : 124;
    lv_obj_set_pos(_keyboard, 0, keyboardY);
    lv_obj_set_size(_keyboard, _width, std::max(1, _height - keyboardY));
    lv_obj_set_style_pad_all(_keyboard, 0, 0);
    lv_obj_set_style_pad_gap(_keyboard, 3, 0);
    lv_obj_set_style_bg_color(_keyboard, lv_color_hex(kBackground), 0);
    lv_obj_set_style_text_font(_keyboard, &hosino_chat_font_16, LV_PART_ITEMS);
    lv_obj_set_style_text_color(_keyboard, lv_color_hex(0xffffff), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(_keyboard, lv_color_hex(kSurface), LV_PART_ITEMS);
    lv_keyboard_set_textarea(_keyboard, _textarea);
    lv_keyboard_set_mode(_keyboard, mode == InputMode::Number ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(_keyboard, keyboardCallback, LV_EVENT_ALL, this);
#if LV_USE_IME_PINYIN
    if (mode == InputMode::Chinese) {
        _ime = lv_ime_pinyin_create(_root);
        lv_obj_set_style_text_font(_ime, &hosino_chat_font_16, 0);
        lv_ime_pinyin_set_keyboard(_ime, _keyboard);
        // LVGL's dictionary provides single-character candidates, not phrase prediction.
        lv_ime_pinyin_set_mode(_ime, LV_IME_PINYIN_MODE_K26);
        auto *candidates = lv_ime_pinyin_get_cand_panel(_ime);
        lv_obj_set_align(candidates, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(candidates, 0, 122); lv_obj_set_size(candidates, _width, 36);
        lv_obj_set_style_pad_all(candidates, 0, 0);
        lv_obj_set_style_text_font(candidates, &hosino_chat_font_16, 0);
        lv_obj_set_style_text_font(candidates, &hosino_chat_font_16, LV_PART_ITEMS);
        lv_obj_set_style_text_color(candidates, lv_color_hex(0xffffff), LV_PART_ITEMS);
        lv_obj_set_style_bg_color(candidates, lv_color_hex(kAccent), LV_PART_ITEMS);
    }
#else
    if (mode == InputMode::Chinese) setNotice("当前固件未启用拼音输入");
#endif
}

bool HosinoConsole::submit(hosino_console::CommandType type, const char *text, unsigned index)
{
    using hosino_console::CommandType;
    if (!canNavigate() || !_clientReady || !_client || !_command || !_snapshot) return false;
    if (pendingWrite()) { setNotice("上次操作正在处理，请勿重复提交"); return false; }
    if (!selectionReady() && (type == CommandType::Send || type == CommandType::Abort ||
        type == CommandType::SendQQ || type == CommandType::ResolveUncertain || type == CommandType::TranscribeMic)) {
        setNotice("正在切换到所选会话，请稍候"); return false;
    }
    if (_requestPending && type == _pendingType && type != CommandType::SelectSession && !writes_state(type)) {
        setNotice("正在读取，可先选择会话或编辑消息"); return false;
    }
    if ((type == CommandType::Send || type == CommandType::Abort || type == CommandType::TranscribeMic) && !_snapshot->send_available) {
        setNotice(_snapshot->quiet ? "当前处于静默窗口，请稍后发送" : "当前会话暂不能发送，请检查登录和会话");
        return false;
    }
    if (type == CommandType::SendQQ && !_snapshot->qq_send_available) { setNotice("当前会话未启用 QQ 直发"); return false; }
    if (text && strlen(text) >= sizeof(_command->text)) { setNotice("输入过长，请缩短后重试"); return false; }
    *_command = {};
    _command->type = type;
    _command->deliver = type == CommandType::Send && _sendMode == SendMode::ChannelAI;
    _command->confirm = type == CommandType::SendQQ || type == CommandType::ResolveUncertain || type == CommandType::PairStart || type == CommandType::ForgetDevice;
    if (++_nextRequestId == 0) ++_nextRequestId;
    _command->request_id = _nextRequestId;
    if (text) copy_text(_command->text, sizeof(_command->text), text);
    if (type == CommandType::PairStart) copy_text(_command->text, sizeof(_command->text), "星野桌面助手");
    if (type == CommandType::CreateSession) copy_text(_command->text, sizeof(_command->text), "星野桌面助手");
    if (type == CommandType::SelectSession) {
        if (index >= _snapshot->session_count || index >= hosino_console::kSessionCount) return false;
        const auto &session = _snapshot->sessions[index];
        copy_text(_command->session_key, sizeof(_command->session_key), session.key);
        copy_text(_command->label, sizeof(_command->label), session.name);
        copy_text(_command->channel, sizeof(_command->channel), session.channel);
    } else if (type == CommandType::RefreshSessions) {
        _command->offset = index;
    } else if (type == CommandType::Send || type == CommandType::SendQQ || type == CommandType::Abort || type == CommandType::RefreshHistory ||
               type == CommandType::TranscribeMic || type == CommandType::ResolveUncertain) {
        if (!_snapshot->selected) { setNotice("请先选择会话"); return false; }
        copy_text(_command->session_key, sizeof(_command->session_key), _snapshot->selected_key);
    }
    const esp_err_t result = _client->submit(*_command);
    erase_text(reinterpret_cast<char *>(_command), sizeof(*_command));
    if (result != ESP_OK) {
        setNotice(result == ESP_ERR_TIMEOUT ? "请求繁忙，请稍后再试" : "请求未提交，请检查连接状态");
        return false;
    }
    _notice[0] = 0;
    _requestPending = true;
    _pendingType = type;
    _pendingRequestId = _nextRequestId;
    if (type == CommandType::SelectSession)
        copy_text(_selectingKey, sizeof(_selectingKey), _snapshot->sessions[index].key);
    else if (writes_state(type))
        _selectingKey[0] = 0; // A successful POST may supersede an unapplied queued SelectSession.
    if (type == CommandType::Send || type == CommandType::SendQQ) {
        _sentRequestId = _pendingRequestId;
        copy_text(_sentDraft, sizeof(_sentDraft), text);
        copy_text(_sentSession, sizeof(_sentSession), _snapshot->selected_key);
    }
    return true;
}

void HosinoConsole::onAction(Action action, unsigned index)
{
    using hosino_console::CommandType;
    if (!canNavigate()) return;
    if (_viewPending || actionBlocked(action)) return;
    switch (action) {
    case Action::Chat: queueView(View::Chat); break;
    case Action::ChatMenu: queueView(View::ChatMenu); break;
    case Action::Shortcuts: queueView(View::Shortcuts); break;
    case Action::KeyCycle:
        if (_view == View::Shortcuts && index < 3) {
            _keyActions[index] = static_cast<KeyAction>((static_cast<unsigned>(_keyActions[index]) + 1) %
                static_cast<unsigned>(KeyAction::Count));
            _keysDirty = false;
            for (unsigned i = 0; i < 3; ++i) _keysDirty = _keysDirty || _keyActions[i] != _savedKeyActions[i];
            copy_text(_keyNotice, sizeof(_keyNotice), _keysDirty ? "修改已生效，尚未保存到设备" : "当前功能与已保存设置相同");
            _renderPending = true;
        }
        break;
    case Action::KeySave:
        if (_view == View::Shortcuts) saveShortcuts();
        break;
    case Action::Voice: onVoiceRequested(); break;
    case Action::VoiceFinish: onVoiceRequested(); break;
    case Action::VoiceCancel: queueView(View::Chat); break;
    case Action::SendOptions:
        if (_view == View::Edit && _edit == EditKind::Message) {
            preserveEditor(); wipeEditor(); _notice[0] = 0; queueView(View::SendOptions);
        }
        break;
    case Action::ChooseDeviceAI: case Action::ChooseChannelAI: case Action::ChooseDirectQQ:
        if (_view == View::SendOptions && _snapshot && _snapshot->selected) {
            const bool qq = !strcmp(_snapshot->selected_channel, "qqbot");
            if ((action != Action::ChooseDeviceAI && !qq) ||
                (action == Action::ChooseDirectQQ ? !_snapshot->qq_send_available : !_snapshot->send_available)) {
                setNotice("当前会话不支持这种发送方式"); break;
            }
            _sendMode = action == Action::ChooseDeviceAI ? SendMode::DeviceAI :
                action == Action::ChooseChannelAI ? SendMode::ChannelAI : SendMode::DirectQQ;
            _notice[0] = 0; queueView(View::Edit);
        }
        break;
    case Action::ConfirmSend:
        if (_view == View::ConfirmSend && _snapshot && _snapshot->selected && _confirmText[0] &&
            !_snapshot->send_uncertain && !strcmp(_snapshot->selected_key, _confirmSession) &&
            !strcmp(_snapshot->selected_channel, "qqbot") && _sendMode != SendMode::DeviceAI) {
            if (submit(_sendMode == SendMode::DirectQQ ? CommandType::SendQQ : CommandType::Send, _confirmText)) {
                erase_text(_confirmText, sizeof(_confirmText)); erase_text(_confirmSession, sizeof(_confirmSession));
                queueView(View::Chat);
            }
        } else setNotice("目标会话已改变，请返回重新核对");
        break;
    case Action::ReviewSend:
        if (_snapshot && _snapshot->selected && _snapshot->send_uncertain) {
            preserveEditor(); wipeEditor();
            copy_text(_confirmSession, sizeof(_confirmSession), _snapshot->selected_key);
            _notice[0] = 0; queueView(View::ReviewSend);
        }
        break;
    case Action::ResolveSend:
        if (canResolveSend() && submit(CommandType::ResolveUncertain)) {
            erase_text(_confirmSession, sizeof(_confirmSession));
            setNotice("正在恢复发送权限，不会重发草稿"); queueView(View::Chat);
        }
        break;
    case Action::RememberLogin:
        if (_client && _snapshot && !pendingWrite() && !_snapshot->remember_busy) {
            _client->setRememberLogin(_snapshot->remember_error != ESP_OK ?
                _client->rememberLogin() : !_client->rememberLogin());
        }
        break;
    case Action::Manage: queueView(View::Manage); break;
    case Action::PairStart:
        if (canStartPairing() && submit(CommandType::PairStart)) queueView(View::Pairing);
        break;
    case Action::PairCancel:
        _notice[0] = 0; queueView(View::Chat); break;
    case Action::DeviceIdentity: queueView(View::DeviceIdentity); break;
    case Action::ForgetDevice:
        if (_snapshot && _snapshot->device_saved && !_snapshot->device_credential_busy) {
            copy_text(_confirmDeviceId, sizeof(_confirmDeviceId), _snapshot->device_id);
            _notice[0] = 0; queueView(View::ForgetDevice);
        }
        break;
    case Action::ConfirmForgetDevice:
        if (canForgetDevice() && submit(CommandType::ForgetDevice)) {
            _confirmDeviceId[0] = 0; erase_text(_draft, sizeof(_draft));
            queueView(View::Chat);
        }
        break;
    case Action::Login: cancelPairingUi(); showEditor(EditKind::Password); break;
    case Action::Logout:
        if (submit(CommandType::Logout)) { erase_text(_draft, sizeof(_draft)); erase_text(_query, sizeof(_query)); queueView(View::Chat); }
        break;
    case Action::Sessions:
        if (submit(CommandType::RefreshSessions, nullptr, 0)) queueView(View::Sessions);
        break;
    case Action::CreateSession:
        if (submit(CommandType::CreateSession)) { _sendMode = SendMode::DeviceAI; erase_text(_draft, sizeof(_draft)); queueView(View::Chat); }
        break;
    case Action::SelectSession:
        if (submit(CommandType::SelectSession, nullptr, index)) {
            _sendMode = SendMode::DeviceAI; erase_text(_draft, sizeof(_draft)); queueView(View::Chat);
        }
        break;
    case Action::Prev:
        if (submit(CommandType::RefreshSessions, nullptr, _snapshot->session_offset >= hosino_console::kSessionCount ?
               _snapshot->session_offset - hosino_console::kSessionCount : 0)) queueView(View::Sessions);
        break;
    case Action::Next:
        if (submit(CommandType::RefreshSessions, nullptr, _snapshot->session_offset + _snapshot->session_count)) queueView(View::Sessions);
        break;
    case Action::Compose:
        if (_snapshot && selectionReady() && _snapshot->selected && _snapshot->send_available &&
            !actionBlocked(Action::Compose)) showEditor(EditKind::Message);
        break;
    case Action::Abort:
        if (submit(CommandType::Abort) && (_view == View::ChatMenu || _view == View::ReviewSend)) queueView(View::Chat);
        break;
    case Action::Overview: submit(CommandType::Overview); queueView(View::Overview); break;
    case Action::Models: submit(CommandType::Models); queueView(View::Models); break;
    case Action::Usage: submit(CommandType::Usage); queueView(View::Usage); break;
    case Action::Memory: queueView(View::Memory); break;
    case Action::Search: showEditor(EditKind::Memory); break;
    case Action::Refresh:
        if (_view == View::Chat || _view == View::ChatMenu || _view == View::ReviewSend) {
            if (submit(CommandType::RefreshHistory) && (_view == View::ChatMenu || _view == View::ReviewSend)) queueView(View::Chat);
        }
        else if (_view == View::Sessions) submit(CommandType::RefreshSessions, nullptr, _snapshot->session_offset);
        else if (_view == View::Overview) submit(CommandType::Overview);
        else if (_view == View::Models) submit(CommandType::Models);
        else if (_view == View::Usage) submit(CommandType::Usage);
        else submit(CommandType::RefreshStatus);
        break;
    case Action::Back: back(); break;
    case Action::PasswordVisibility:
        if (_view == View::Edit && _edit == EditKind::Password && _textarea && _passwordReveal) {
            _passwordVisible = !_passwordVisible;
            lv_textarea_set_password_mode(_textarea, !_passwordVisible);
            lv_label_set_text(lv_obj_get_child(_passwordReveal, 0), _passwordVisible ? "隐藏" : "显示");
        }
        break;
    case Action::Chinese: case Action::English: case Action::Number:
        if (_view == View::Edit) {
            _nextInputMode = action == Action::Chinese ? InputMode::Chinese : action == Action::English ? InputMode::English : InputMode::Number;
            _inputPending = true;
        }
        break;
    case Action::EditAccept:
        if (_view != View::Edit || !_textarea) break;
        {
            const char *text = lv_textarea_get_text(_textarea);
            if (!text || !text[0]) { setNotice("请先输入内容"); break; }
            if (_edit == EditKind::Message && _snapshot->send_uncertain) {
                preserveEditor(); wipeEditor();
                copy_text(_confirmSession, sizeof(_confirmSession), _snapshot->selected_key);
                _notice[0] = 0; queueView(View::ReviewSend); break;
            }
            if (_edit == EditKind::Message && (!_snapshot->selected ||
                (_sendMode == SendMode::DirectQQ ? !_snapshot->qq_send_available : !_snapshot->send_available) ||
                strcmp(_editSession, _snapshot->selected_key) || _snapshot->send_uncertain)) {
                setNotice("会话状态已改变，请返回核对后再发送"); break;
            }
            if (_edit == EditKind::Message) copy_text(_draft, sizeof(_draft), text);
            if (_edit == EditKind::Memory) copy_text(_query, sizeof(_query), text);
            if (_edit == EditKind::Message && _sendMode != SendMode::DeviceAI) {
                if (strcmp(_snapshot->selected_channel, "qqbot")) { setNotice("这不是 QQ 会话，请改为 AI·设备"); break; }
                copy_text(_confirmText, sizeof(_confirmText), text);
                copy_text(_confirmSession, sizeof(_confirmSession), _snapshot->selected_key);
                _notice[0] = 0; wipeEditor(); queueView(View::ConfirmSend); break;
            }
            if (submit(_edit == EditKind::Password ? CommandType::Login : _edit == EditKind::Memory ? CommandType::SearchMemory : CommandType::Send, text)) {
                wipeEditor();
                // Keep a recoverable draft until this request's validated
                // server acceptance arrives; a queue acknowledgment is not it.
                queueView(_edit == EditKind::Memory ? View::Memory : View::Chat);
            }
        }
        break;
    }
}

void HosinoConsole::update()
{
    if (!canNavigate()) return;
    // Keep the visible row's index and its snapshot/key together until release.
    // Freezing only the redraw would still let a pressed old row select a new key.
    if (touch_down()) return;
    if (!_clientReady && static_cast<int32_t>(lv_tick_get() - _nextClientAttempt) >= 0) {
        _nextClientAttempt = lv_tick_get() + 1000;
        if (ensureClient()) { _client->setForeground(true); _renderPending = true; }
    }
    if (_clientReady && _client && _snapshot && _client->copySnapshot(*_snapshot)) {
        if (_selectingKey[0] && selectionReady()) _selectingKey[0] = 0;
        acceptSentDraft();
        if (_requestPending && _snapshot->completed_request_id == _pendingRequestId) {
            _requestPending = false;
            if (_pendingType == hosino_console::CommandType::SelectSession) _selectingKey[0] = 0;
            if (_pendingType == hosino_console::CommandType::ResolveUncertain)
                setNotice(_snapshot->completed_request_error == ESP_OK ?
                    "已恢复发送，草稿保留；没有自动重发" : "恢复未完成，请核对会话后再试");
            if (_snapshot->completed_request_error != ESP_OK && !_notice[0]) {
                const bool send = _pendingType == hosino_console::CommandType::Send ||
                    _pendingType == hosino_console::CommandType::SendQQ;
                setNotice(send ? (!strcmp(_snapshot->selected_key, _snapshot->send_result_key) && _snapshot->send_status[0] ? _snapshot->send_status :
                    "发送未成功确认，草稿已保留，请核对结果") :
                    _snapshot->completed_request_status[0] ? _snapshot->completed_request_status :
                    _pendingType == hosino_console::CommandType::Login ? "登录未完成，可重试或上滑返回" : "操作未完成，可返回或重新选择");
                _noticeIsLogin = _pendingType == hosino_console::CommandType::Login;
            }
            if ((_pendingType == hosino_console::CommandType::Send || _pendingType == hosino_console::CommandType::SendQQ) &&
                _sentRequestId == _pendingRequestId) {
                // Definite rejection or an uncertain result keeps _draft.
                _sentRequestId = 0;
                erase_text(_sentDraft, sizeof(_sentDraft)); erase_text(_sentSession, sizeof(_sentSession));
            }
        }
        if (_authedBefore && !_snapshot->authed) {
            if (_view == View::Edit && _edit != EditKind::Password) { wipeEditor(); queueView(View::Chat); }
            erase_text(_draft, sizeof(_draft));
        }
        if (!_authedBefore && _snapshot->authed && _noticeIsLogin) {
            _notice[0] = 0; _noticeIsLogin = false;
        }
        _authedBefore = _snapshot->authed;
        if (_view == View::Pairing && !_viewPending && _snapshot->authed &&
            _snapshot->device_authenticated && _snapshot->pairing_state == hosino_console::PairState::Ready) {
            _notice[0] = 0; queueView(View::Chat);
        }
        if (_voiceAwaiting && _snapshot->voice_state == hosino_console::VoiceState::Ready &&
            _snapshot->voice_result_revision != _voiceResultSeen) {
            _voiceResultSeen = _snapshot->voice_result_revision;
            _voiceAwaiting = false;
            if (_snapshot->authed && _snapshot->selected &&
                !strcmp(_voiceKey, _snapshot->selected_key) &&
                !strcmp(_voiceKey, _snapshot->voice_session_key)) {
                const size_t previous = strlen(_draft), incoming = strlen(_snapshot->voice_text);
                const size_t separator = previous ? 1 : 0;
                if (previous + separator + incoming < sizeof(_draft)) {
                    if (separator) _draft[previous] = '\n';
                    memcpy(_draft + previous + separator, _snapshot->voice_text, incoming + 1);
                    _notice[0] = 0;
                    showEditor(EditKind::Message);
                } else {
                    setNotice("原草稿加识别文字过长。原草稿已保留，请分段输入。");
                    queueView(View::Chat);
                }
            } else { setNotice("会话已改变，本次识别已丢弃"); queueView(View::Chat); }
        } else if (_voiceAwaiting && !pendingWrite() &&
                   (_snapshot->voice_state == hosino_console::VoiceState::Error ||
                    _snapshot->voice_state == hosino_console::VoiceState::Cancelled)) {
            _voiceAwaiting = false;
            setNotice(_snapshot->status);
        }
        if (contentSignature() != _renderedContent) _renderPending = true;
    }
    if (_viewPending) {
        _viewPending = false;
        _view = _nextView;
        _savedScroll = 0;
        render();
        refreshControls();
        return;
    }
    if (_inputPending && _view == View::Edit) {
        if (_edit == EditKind::Password) updatePasswordKeyboard();
        else createKeyboard(_nextInputMode);
    }
    if (_view == View::Edit) { refreshControls(); return; } // Keep text/candidate composition intact.
    if (_renderPending) {
        if (_list) {
            _savedScroll = lv_obj_get_scroll_y(_list);
            if (_view == View::Chat) _followChat = lv_obj_get_scroll_bottom(_list) <= 8;
        }
        render();
    }
    refreshControls();
}

void HosinoConsole::eventCallback(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    auto *binding = static_cast<Binding *>(lv_event_get_user_data(event));
    if (binding && binding->owner) binding->owner->onAction(binding->action, binding->index);
}

void HosinoConsole::keyboardCallback(lv_event_t *event)
{
    auto *self = static_cast<HosinoConsole *>(lv_event_get_user_data(event));
    if (!self || !self->canNavigate()) return;
    if (self->_view == View::Edit && self->_edit == EditKind::Password &&
        lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        self->onPasswordKey(lv_keyboard_get_selected_button(self->_keyboard));
        return;
    }
    if (lv_event_get_code(event) == LV_EVENT_CANCEL) self->back();
    // Keyboard READY does not send/login: only the clearly labelled top button does.
}

void HosinoConsole::timerCallback(lv_timer_t *timer)
{
    auto *self = static_cast<HosinoConsole *>(lv_timer_get_user_data(timer));
    if (self) self->update();
}

void HosinoConsole::buttonTimerCallback(lv_timer_t *timer)
{
    auto *self = static_cast<HosinoConsole *>(lv_timer_get_user_data(timer));
    if (!self || timer != self->_buttonTimer || !self->_buttonsReady) return;
    if (!self->canNavigate()) {
        hosino_buttons_set_active(false);
        return;
    }
    hosino_buttons_set_active(true);
    hosino_buttons_poll(lv_tick_get());
    hosino_button_event_t event;
    if (self->_viewPending || touch_down()) {
        // Never carry a release into a different view or interrupt a touch edit.
        while (hosino_buttons_take_event(&event)) {}
        return;
    }
    if (hosino_buttons_take_event(&event)) {
        // Back can close the app and hand timer cleanup to Brookesia. Do not
        // access self, the timer, or any UI object after dispatching this event.
        self->onHardwareButton(event.button);
    }
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, HosinoConsole, "HosinoConsole", []() {
    return std::make_shared<HosinoConsole>();
})
} // namespace esp_brookesia::apps
