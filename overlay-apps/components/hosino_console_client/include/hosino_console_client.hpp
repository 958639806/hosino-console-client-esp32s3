// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace hosino_console {

constexpr size_t kSessionKeySize = 256;
constexpr size_t kCommandTextSize = 1024;
constexpr size_t kSessionCount = 8;
constexpr size_t kMessageCount = 10;
constexpr size_t kModelCount = 32;
constexpr size_t kMemoryCount = 6;

enum class CommandType : uint8_t {
    Login, Logout, RefreshStatus, RefreshSessions, SelectSession, CreateSession,
    RefreshHistory, Send, Abort, Overview, Models, Usage, SearchMemory, TranscribeMic, SendQQ, ResolveUncertain,
    PairStart, PairCancel, ForgetDevice,
};
enum class VoiceState : uint8_t { Idle, Preparing, Recording, Uploading, Ready, Cancelled, Error };
enum class PairState : uint8_t { Idle, Starting, Waiting, Storing, Activating, Verifying, Ready, Denied, Expired, Error, Cancelled };

// value-initialize before filling. Login: text=password (never persisted).
// Send/SearchMemory: text. CreateSession: text is displayName (explicit button).
// SelectSession: session_key, optional label/channel.
// TranscribeMic: current explicitly selected session_key; explicit record action.
// RefreshSessions: offset is the number of entries to skip. All writes require
// an explicit submit; neither retries nor polling create any write command.
struct Command {
    CommandType type = CommandType::RefreshStatus;
    uint32_t request_id = 0; // UI correlation; zero is reserved for background polling.
    bool deliver = false; // Explicit choice to deliver an AI reply to the session's channel.
    bool confirm = false; // SendQQ target/text confirmation, or explicit local uncertainty acknowledgement.
    uint32_t offset = 0;
    char session_key[kSessionKeySize] = {};
    char text[kCommandTextSize] = {};
    char label[96] = {};
    char channel[40] = {};
};

struct Session {
    char key[kSessionKeySize];
    char name[96];
    char channel[40];
    char model[96];
    bool active;
};
struct Message {
    char id[96];
    char role[20];
    char text[768];
    char sender[64];
    bool truncated;
};
struct Model {
    char id[128];
    char name[96];
    char provider[48];
    char availability[24]; // unknown when absent; never fabricate availability
};
struct Memory {
    char text[512];
    char user[64];
    float score;
    bool has_score;
    bool truncated;
};

// Bounded POD. Allocate with allocSnapshot(), not on the small GUI task stack.
// Text remains UTF-8 and is truncated only on complete character boundaries.
// Management text is an explicit bounded summary; unsupported fields are not
// converted into invented zero-valued metrics.
struct Snapshot {
    uint32_t revision;
    uint32_t generation;
    uint32_t completed_request_id;
    esp_err_t completed_request_error;
    int completed_request_http_status;
    char completed_request_status[160]; // Result latched with request ID; background polling cannot replace it.
    uint32_t accepted_send_request_id; // Only a validated runId/status admission; never set by polling.
    bool foreground;
    bool busy;
    bool stopped;
    bool auth_known;
    bool authed;
    bool is_owner;
    PairState pairing_state;
    char user_code[10];
    char verification_uri[192];
    uint32_t pair_remaining_sec;
    bool device_authenticated;
    bool device_credential_busy;
    bool device_saved;
    char device_label[96];
    char device_id[96];
    char pair_status[160];
    bool remember_login;
    bool remember_busy;
    bool remember_saved;
    esp_err_t remember_error;
    bool quiet;
    bool features_available;
    bool send_available; // authenticated, explicitly selected nonempty key; server enforces route permissions
    bool qq_send_available; // server feature enabled, selected QQ session, outside quiet window
    bool send_uncertain;
    int send_http_status; // Last explicit send result; background GETs never overwrite these fields.
    char send_error_code[48];
    char send_status[160];
    char send_result_key[kSessionKeySize];
    bool selected;
    bool selected_active;
    int http_status;
    esp_err_t last_error;
    uint32_t retry_after_ms;
    char status[160];
    char error_code[48];
    char quiet_until[48];
    char selected_key[kSessionKeySize];
    char selected_name[96];
    char selected_channel[40];
    char last_run_id[128];
    uint32_t session_offset;
    uint32_t session_total;
    bool sessions_available;
    uint8_t session_count;
    bool sessions_have_more;
    Session sessions[kSessionCount];
    uint8_t message_count;
    bool history_available;
    bool history_has_more;
    Message messages[kMessageCount];
    uint8_t model_count;
    bool models_available;
    uint32_t model_total;
    bool models_truncated;
    Model models[kModelCount];
    bool overview_available;
    char overview[2048];
    bool usage_available;
    char usage[2048];
    bool memory_available;
    uint8_t memory_count;
    Memory memories[kMemoryCount];
    VoiceState voice_state;
    uint32_t voice_elapsed_ms;
    uint32_t voice_result_revision; // increases only for a newly accepted draft result
    char voice_text[kCommandTextSize];
    char voice_session_key[kSessionKeySize];
};

class Client final {
public:
    // Starts one worker. Client and all substantial buffers live in PSRAM.
    static Client *create();
    static Snapshot *allocSnapshot();
    static void freeSnapshot(Snapshot *snapshot);

    // Nonblocking queue operations; ESP_ERR_INVALID_STATE while background or
    // shutting down, ESP_ERR_TIMEOUT when the short queue/lock is full.
    esp_err_t submit(const Command &command);
    // Explicit user opt-in; password is never saved. NVS runs on a separate
    // internal-stack worker. Observe remember_* fields for completion/errors.
    void setRememberLogin(bool enabled);
    bool rememberLogin() const;
    // Cancels an unfinished pairing without sending messages or revoking an
    // already stored device credential. Explicit ForgetDevice removes it.
    void cancelPairing();
    // Nonblocking. false finishes capture and requests transcription; true
    // cancels queued/capturing/uploading ASR and discards its result. Audio
    // reads/HTTP must return to their owning worker before resources are freed.
    void stopVoice(bool cancel = false);
    // Background requests a worker-side trim after active HTTP cleanup. Cookie
    // and snapshots remain; large response/parser/session buffers are recreated
    // on the next foreground HTTP request, with recoverable allocation failure.
    void setForeground(bool foreground);
    // Returns false if a publish is in progress; caller keeps its old copy.
    bool copySnapshot(Snapshot &out) const;
    // Immediately cancels queued commands/polling and invalidates old results.
    // HTTP teardown and credential wiping happen on the worker, never GUI.
    void shutdown();
    bool isStopped() const;
    // Only call after isStopped()==true. Returns false while still active.
    bool destroy();
    // Transfer final cleanup to worker and invalidate this pointer immediately.
    // Use in an App destructor that cannot wait; no later calls are allowed.
    void release();

private:
    struct Impl;
    Impl *impl_ = nullptr;
    explicit Client(Impl *impl) : impl_(impl) {}
    ~Client() = default;
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
};

} // namespace hosino_console
