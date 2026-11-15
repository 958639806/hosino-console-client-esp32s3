// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lvgl.h"
#include "hosino_console_client.hpp"
#include "hosino_buttons.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class HosinoConsole final : public systems::phone::App {
public:
    HosinoConsole();
    ~HosinoConsole();
    bool run() override;
protected:
    bool init() override;
    bool deinit() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool resume() override;
    bool cleanResource() override;
private:
    enum class View { Chat, ChatMenu, Shortcuts, Voice, Sessions, Manage, Overview, Models, Usage, Memory, Edit, SendOptions, ConfirmSend, ReviewSend, Pairing, DeviceIdentity, ForgetDevice };
    enum class EditKind { Password, Message, Memory };
    enum class SendMode { DeviceAI, ChannelAI, DirectQQ };
    enum class InputMode { Chinese, English, Number };
    enum class KeyAction : uint8_t { None, Back, Menu, Compose, Voice, Count };
    enum class Action {
        Chat, Manage, Login, Logout, Sessions, CreateSession, SelectSession, Refresh, Prev, Next,
        Compose, ChatMenu, Voice, Abort, Overview, Models, Usage, Memory, Search, Back,
        EditAccept, Chinese, English, Number, PasswordVisibility,
        Shortcuts, KeyCycle, KeySave, VoiceFinish, VoiceCancel, RememberLogin,
        SendOptions, ChooseDeviceAI, ChooseChannelAI, ChooseDirectQQ, ConfirmSend, ReviewSend, ResolveSend, PairStart, PairCancel, DeviceIdentity, ForgetDevice, ConfirmForgetDevice,
    };
    struct Binding { HosinoConsole *owner; Action action; unsigned index; lv_obj_t *object; bool enabled; };
    static constexpr unsigned kBindings = 40;
    Binding _bindings[kBindings] = {};
    unsigned _bindingCount = 0;
    hosino_console::Client *_client = nullptr;
    hosino_console::Snapshot *_snapshot = nullptr;
    hosino_console::Command *_command = nullptr;
    lv_obj_t *_screen = nullptr, *_root = nullptr, *_status = nullptr, *_list = nullptr;
    lv_obj_t *_textarea = nullptr, *_keyboard = nullptr, *_ime = nullptr;
    lv_obj_t *_voiceElapsed = nullptr, *_pairCountdown = nullptr;
    lv_obj_t *_passwordInfo = nullptr, *_passwordReveal = nullptr;
    char _passwordKeys[18][2] = {};
    const char *_passwordMap[28] = {};
    lv_buttonmatrix_ctrl_t _passwordControls[24] = {};
    unsigned _passwordPage = 0;
    bool _passwordVisible = false;
    lv_timer_t *_timer = nullptr;
    lv_timer_t *_buttonTimer = nullptr;
    KeyAction _keyActions[3] = {KeyAction::Menu, KeyAction::Back, KeyAction::Voice};
    KeyAction _savedKeyActions[3] = {KeyAction::Menu, KeyAction::Back, KeyAction::Voice};
    bool _keysLoaded = false, _keysDirty = false, _buttonsReady = false;
    char _keyNotice[160] = {};
    View _view = View::Chat, _nextView = View::Chat;
    EditKind _edit = EditKind::Message;
    InputMode _inputMode = InputMode::Chinese, _nextInputMode = InputMode::Chinese;
    bool _foreground = false, _viewPending = false, _inputPending = false;
    bool _renderPending = false, _authedBefore = false;
    bool _clientReady = false, _closingClient = false;
    bool _requestPending = false, _noticeIsLogin = false;
    hosino_console::CommandType _pendingType = hosino_console::CommandType::RefreshStatus;
    uint32_t _nextRequestId = 0, _pendingRequestId = 0, _sentRequestId = 0;
    char _sentDraft[hosino_console::kCommandTextSize] = {};
    char _sentSession[hosino_console::kSessionKeySize] = {};
    char _selectingKey[hosino_console::kSessionKeySize] = {};
    SendMode _sendMode = SendMode::DeviceAI;
    char _confirmText[hosino_console::kCommandTextSize] = {};
    char _confirmSession[hosino_console::kSessionKeySize] = {};
    char _confirmDeviceId[96] = {};
    bool _voiceAwaiting = false;
    uint32_t _voiceResultSeen = 0;
    char _voiceKey[hosino_console::kSessionKeySize] = {};
    bool _followChat = true;
    uint32_t _nextClientAttempt = 0;
    uint64_t _renderedContent = 0;
    int _width = 400, _height = 364;
    int32_t _savedScroll = 0;
    char _draft[hosino_console::kCommandTextSize] = {};
    char _query[hosino_console::kCommandTextSize] = {};
    char _editSession[hosino_console::kSessionKeySize] = {};
    char _notice[160] = {};

    bool canNavigate() const;
    bool pendingWrite() const;
    const char *pendingWriteStatus() const;
    bool selectionReady() const;
    bool canResolveSend() const;
    bool canStartPairing() const;
    bool canForgetDevice() const;
    void cancelPairingUi();
    bool actionBlocked(Action action) const;
    uint64_t contentSignature() const;
    void refreshControls();
    void acceptSentDraft();
    bool ensureClient();
    void destroyUi();
    void destroyKeyboard();
    void wipeEditor();
    void preserveEditor();
    void queueView(View view);
    void showEditor(EditKind kind);
    void render();
    void renderChat(int y);
    void renderChatMenu();
    void renderShortcuts();
    void loadShortcuts();
    void saveShortcuts();
    void onHardwareButton(hosino_button_id_t button);
    void onVoiceRequested();
    void renderVoice();
    void renderSessions(int y);
    void renderManage(int y);
    void renderDetail(int y);
    void renderEditor();
    void renderSendOptions();
    void renderSendConfirmation();
    void renderSendReview();
    void renderPairing();
    void renderDeviceIdentity(bool confirm);
    void renderPasswordEditor();
    void updatePasswordKeyboard();
    void onPasswordKey(unsigned index);
    void createKeyboard(InputMode mode);
    void update();
    void onAction(Action action, unsigned index);
    bool submit(hosino_console::CommandType type, const char *text = nullptr, unsigned index = 0);
    void setNotice(const char *text);
    lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int w,
                    const lv_font_t *font, uint32_t color = 0xf4f5f7);
    lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                     Action action, unsigned index = 0, bool enabled = true, bool accent = false);
    lv_obj_t *scrollList(int y, int bottomReserve);
    void detailCard(lv_obj_t *parent, const char *title, const char *body, bool secondary = false);
    static void eventCallback(lv_event_t *event);
    static void keyboardCallback(lv_event_t *event);
    static void timerCallback(lv_timer_t *timer);
    static void buttonTimerCallback(lv_timer_t *timer);
};

} // namespace esp_brookesia::apps
