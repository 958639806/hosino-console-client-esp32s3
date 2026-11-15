// SPDX-License-Identifier: Apache-2.0
#include "hosino_console_client.hpp"
#include "console_protocol.h"
#include "console_voice_wav.h"
#include "bsp_board_extra.h"
#include "hosino_console_auth_store.h"
#include "hosino_console_device_store.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <climits>
#include <strings.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "lwip/sockets.h"

namespace hosino_console {
namespace {
constexpr size_t kResponseLimit = HS_CONSOLE_RESPONSE_LIMIT_BYTES;
constexpr size_t kTokenLimit = HS_CONSOLE_JSON_TOKEN_LIMIT;
constexpr size_t kQueueLimit = 4;
constexpr size_t kServerSessionPage = 200;
constexpr char kBaseUrl[] = "https://hosino.ume.wiki";
constexpr char kDashboardPrefix[] = "agent:main:dashboard:";
const char *TAG = "hosino_console";
portMUX_TYPE s_control_mux = portMUX_INITIALIZER_UNLOCKED;

void text(char *out, size_t cap, const char *in) {
    hs_utf8_copy(out, cap, in ? in : "", in ? strlen(in) : 0);
}
bool dashboard(const char *key) {
    return key && strncmp(key, kDashboardPrefix, sizeof(kDashboardPrefix) - 1) == 0 && key[sizeof(kDashboardPrefix) - 1];
}
bool explicit_post(CommandType t) {
    return t == CommandType::Login || t == CommandType::Logout || t == CommandType::Send ||
           t == CommandType::Abort || t == CommandType::CreateSession || t == CommandType::SearchMemory ||
           t == CommandType::TranscribeMic || t == CommandType::SendQQ || t == CommandType::PairStart;
}
uint32_t number(const hs_json &j, int token, uint32_t fallback = 0) {
    double value;
    return hs_json_number(&j, token, &value) && value >= 0 && value <= UINT32_MAX ? static_cast<uint32_t>(value) : fallback;
}
bool field(const hs_json &j, int object, const char *name, char *out, size_t cap, bool *cut = nullptr) {
    return hs_json_text(&j, hs_json_member(&j, object, name), out, cap, cut);
}
bool boolean(const hs_json &j, int object, const char *name, bool fallback = false) {
    bool value; return hs_json_bool(&j, hs_json_member(&j, object, name), &value) ? value : fallback;
}
void append(char *out, size_t cap, const char *value) {
    const size_t used = strlen(out); if (used < cap) text(out + used, cap - used, value);
}
void clear_private(Snapshot &s) {
    const uint32_t revision = s.revision;
    const uint32_t generation = s.generation;
    const bool foreground = s.foreground;
    hs_secure_zero(&s, sizeof(s)); s.revision = revision; s.generation = generation;
    s.foreground = foreground; s.auth_known = true;
}
} // namespace

struct Client::Impl {
    struct Queued { Command command; uint32_t generation; uint32_t voice_serial; uint32_t auth_epoch; char logout_cookie[65]; };
    struct Response {
        Impl *owner;
        uint32_t generation;
        size_t size;
        bool too_large, cancelled, headers_sent, timed_out, transport_aborted;
        int64_t deadline;
        bool json_type;
        bool voice_request;
        bool device_auth;
        int status;
        esp_err_t error;
        char cookie[65];
    } response{};

    Snapshot *working = nullptr, *published = nullptr;
    Session *session_cache = nullptr;
    uint32_t cached_page = 0, cached_count = 0, cached_total = 0;
    bool cache_valid = false;
    hs_json_token *tokens = nullptr;
    char *body = nullptr;
    SemaphoreHandle_t publish_lock = nullptr;
    TaskHandle_t worker = nullptr;
    Queued queue[kQueueLimit]{};
    unsigned head = 0, count = 0;
    uint32_t generation = 1, revision = 0;
    bool foreground = false, stopping = false, worker_done = false, released = false;
    bool active = false, active_post = false, clear_auth = false, status_due = false;
    bool trim_due = false; // GUI only requests; worker frees after HTTP cleanup
    bool qq_send_enabled = false;
    bool initial_sync_due = false; // Read-only bootstrap after authentication; never holds a Login write lock.
    bool voice_pending = false, voice_running = false, voice_finish = false, voice_cancel = false;
    uint32_t voice_generation = 0;
    uint32_t voice_serial = 0;
    uint32_t working_voice_generation = 0, working_voice_serial = 0;
    uint32_t published_voice_generation = 0, published_voice_serial = 0;
    // Only this worker owns the codec and these cleanup flags.
    bool voice_audio_owned = false, voice_codec_claimed = false;
    uint32_t voice_result_counter = 0;
    char cookie[65]{}; // guarded by s_control_mux; never copied to snapshots/logs
    bool cookie_verified = false; // guarded by s_control_mux; only /api/me validates a new cookie
    uint32_t cookie_store_epoch = 0;
    uint32_t restore_ticket = 0, restore_generation = 0;
    bool restore_due = true; // only a new Client restores; background keeps RAM login
    bool verify_restored = false; // worker only; restored OR freshly issued cookie needs read-only validation
    bool pending_login_save = false; // worker only; persist a fresh cookie once validation succeeds
    enum class DeviceStoreOp { None, Restore, SaveNew, SaveAck, Forget };
    DeviceStoreOp device_store_op = DeviceStoreOp::None;
    hs_device_store_record_t device_record{};
    uint32_t device_ticket = 0, device_epoch = 0;
    bool device_restore_due = true, device_saved = false, device_ready = false, device_suppressed = false;
    std::atomic_bool device_mode{false}; // GUI submit only inspects mode; secrets stay worker-owned
    bool pair_cancel_requested = false, pair_running = false, device_resume_due = false; // control mux
    PairState pair_state = PairState::Idle;
    char pair_id[33]{}, pair_secret[44]{}, pair_user_code[10]{}, pair_message[160]{};
    char device_label[96]{}, device_id[96]{};
    int64_t pair_expires = 0, pair_next = 0;
    uint32_t pair_interval = 3;
    uint32_t device_store_retry_ms = 3000;
    char request[8192]{}, quoted[6144]{}, quoted_key[1536]{}, path[1024]{};
    char uncertain_key[kSessionKeySize]{};
    int64_t next_history = 0, next_sessions = 0, next_me = 0, retry_reads_at = 0;
    uint32_t read_backoff_ms = 1000;

    bool current(uint32_t g) {
        portENTER_CRITICAL(&s_control_mux);
        const bool valid = !stopping && foreground && generation == g;
        portEXIT_CRITICAL(&s_control_mux); return valid;
    }
    uint32_t current_generation() {
        portENTER_CRITICAL(&s_control_mux); const uint32_t g = generation;
        portEXIT_CRITICAL(&s_control_mux); return g;
    }
    bool restore_login() {
        portENTER_CRITICAL(&s_control_mux);
        const bool needed = restore_due && foreground && !stopping;
        const uint32_t g = generation, ticket = restore_ticket;
        portEXIT_CRITICAL(&s_control_mux);
        if (!needed) return false;
        if (!ticket) {
            hs_auth_store_status_t auth{}; hs_auth_store_status(&auth);
            if (!auth.ready) return true;
            if (!auth.remember || auth.error != ESP_OK) {
                portENTER_CRITICAL(&s_control_mux); if (generation == g) restore_due = false; portEXIT_CRITICAL(&s_control_mux);
                if (auth.error != ESP_OK) set_status("保存的登录暂不可用，请重新登录", auth.error, "remember_read_failed");
                return false;
            }
            const uint32_t requested = hs_auth_store_restore();
            portENTER_CRITICAL(&s_control_mux);
            const bool accepted = restore_due && generation == g && foreground && !stopping;
            if (accepted) { restore_ticket = requested; restore_generation = g; }
            portEXIT_CRITICAL(&s_control_mux);
            if (!accepted) hs_auth_store_cancel_restore(requested);
            return accepted;
        }
        char restored[65]{}; uint32_t saved_epoch = 0; esp_err_t error = ESP_OK;
        if (!hs_auth_store_take_restore(ticket, restored, &saved_epoch, &error)) return true;
        portENTER_CRITICAL(&s_control_mux);
        const bool accepted = restore_due && restore_ticket == ticket && restore_generation == g &&
                              generation == g && foreground && !stopping;
        if (accepted) {
            restore_due = false; restore_ticket = 0;
            if (error == ESP_OK && restored[0]) { memcpy(cookie, restored, sizeof(cookie)); cookie_store_epoch = saved_epoch; cookie_verified = false; }
        }
        portEXIT_CRITICAL(&s_control_mux);
        if (accepted && error == ESP_OK && restored[0]) {
            verify_restored = true;
            set_status("正在验证已保存的登录"); publish(g);
        }
        hs_secure_zero(restored, sizeof(restored));
        return false;
    }
    bool voice_current(uint32_t g) {
        portENTER_CRITICAL(&s_control_mux);
        const bool valid = !stopping && foreground && generation == g &&
            voice_pending && voice_generation == g && !voice_cancel;
        portEXIT_CRITICAL(&s_control_mux); return valid;
    }
    bool voice_finish_requested() {
        portENTER_CRITICAL(&s_control_mux); const bool finish = voice_finish;
        portEXIT_CRITICAL(&s_control_mux); return finish;
    }
    void set_status(const char *message, esp_err_t error = ESP_OK, const char *code = "") {
        text(working->status, sizeof(working->status), message);
        text(working->error_code, sizeof(working->error_code), code); working->last_error = error;
    }
    void acknowledge_send(uint32_t g, uint32_t id) {
        if (!id) return;
        portENTER_CRITICAL(&s_control_mux);
        if (!stopping && foreground && generation == g) working->accepted_send_request_id = id;
        portEXIT_CRITICAL(&s_control_mux);
    }
    bool acknowledge_request(uint32_t g, uint32_t id) {
        if (!id) return false;
        portENTER_CRITICAL(&s_control_mux);
        const bool valid = !stopping && foreground && generation == g;
        if (valid) {
            working->completed_request_id = id;
            working->completed_request_error = working->last_error;
            working->completed_request_http_status = working->http_status;
            text(working->completed_request_status, sizeof(working->completed_request_status), working->status);
        }
        portEXIT_CRITICAL(&s_control_mux);
        return valid;
    }
    void publish(uint32_t g) {
        if (!current(g)) return;
        working->generation = g; working->revision = ++revision; working->foreground = true;
        working->pairing_state = pair_state;
        text(working->user_code, sizeof(working->user_code), pair_user_code);
        text(working->verification_uri, sizeof(working->verification_uri), "https://hosino.ume.wiki/pair.html");
        working->pair_remaining_sec = pair_expires > esp_timer_get_time() ? static_cast<uint32_t>((pair_expires - esp_timer_get_time() + 999999) / 1000000) : 0;
        working->device_authenticated = device_mode && device_ready && working->authed;
        working->device_saved = device_saved;
        working->device_credential_busy = device_store_op != DeviceStoreOp::None;
        text(working->device_label, sizeof(working->device_label), device_label);
        text(working->device_id, sizeof(working->device_id), device_id);
        text(working->pair_status, sizeof(working->pair_status), pair_message);
        working->send_uncertain = working->selected && uncertain_key[0] &&
            !strcmp(uncertain_key, working->selected_key);
        working->send_available = working->authed && working->selected && working->selected_key[0];
        working->qq_send_available = working->send_available && qq_send_enabled &&
            !strcmp(working->selected_channel, "qqbot") && !working->quiet;
        if (xSemaphoreTake(publish_lock, portMAX_DELAY) == pdTRUE) {
            if (current(g)) {
                memcpy(published, working, sizeof(*working));
                published_voice_generation = working_voice_generation;
                published_voice_serial = working_voice_serial;
            }
            xSemaphoreGive(publish_lock);
        }
    }
    void wipe_queue_locked() {
        if (voice_pending) {
            voice_cancel = true;
            if (!voice_running) voice_pending = false;
        }
        hs_secure_zero(queue, sizeof(queue)); head = count = 0;
    }
    bool post_pending_locked() const {
        if (active_post) return true;
        for (unsigned i = 0; i < count; ++i)
            if (explicit_post(queue[(head + i) % kQueueLimit].command.type)) return true;
        return false;
    }
    bool invalidate_auth(bool forget_saved = false, uint32_t expected_generation = 0) {
        portENTER_CRITICAL(&s_control_mux);
        if (expected_generation && (generation != expected_generation || stopping || !foreground)) {
            portEXIT_CRITICAL(&s_control_mux); return false;
        }
        const uint32_t saved_epoch = cookie_store_epoch;
        verify_restored = false; pending_login_save = false; qq_send_enabled = false;
        hs_secure_zero(cookie, sizeof(cookie)); cookie_verified = false; wipe_queue_locked();
        portEXIT_CRITICAL(&s_control_mux);
        if (forget_saved) hs_auth_store_forget(saved_epoch);
        clear_private(*working); hs_secure_zero(uncertain_key, sizeof(uncertain_key));
        if (session_cache) hs_secure_zero(session_cache, kServerSessionPage * sizeof(Session));
        cache_valid = false;
        return true;
    }
    static esp_err_t http_event(esp_http_client_event_t *event) {
        auto *r = static_cast<Response *>(event->user_data);
        if (!r || !r->owner) return ESP_FAIL;
        if (!r->owner->current(r->generation) ||
            (r->voice_request && !r->owner->voice_current(r->generation))) r->cancelled = true;
        if (r->deadline && esp_timer_get_time() >= r->deadline) r->timed_out = true;
        if (r->cancelled || r->timed_out || r->too_large) { abort_transport(event, r); return ESP_FAIL; }
        if (event->event_id == HTTP_EVENT_HEADERS_SENT) r->headers_sent = true;
        if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value) {
            if (!strcasecmp(event->header_key, "Set-Cookie")) hs_cookie_parse(event->header_value, r->cookie);
            else if (!strcasecmp(event->header_key, "Content-Type"))
                r->json_type = !strncasecmp(event->header_value, "application/json", 16);
            else if (!strcasecmp(event->header_key, "Content-Length")) {
                char *end; const unsigned long long length = strtoull(event->header_value, &end, 10);
                if (end != event->header_value && !*end && length > kResponseLimit) {
                    r->too_large = true; abort_transport(event, r);
                }
            }
        } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
            const size_t size = static_cast<size_t>(event->data_len);
            if (r->too_large || size > kResponseLimit - r->size) { r->too_large = true; abort_transport(event, r); return ESP_FAIL; }
            memcpy(r->owner->body + r->size, event->data, size); r->size += size;
        }
        return ESP_OK;
    }
    static void abort_transport(esp_http_client_event_t *event, Response *r) {
        if (r->transport_aborted) return;
        const int socket = esp_http_client_get_socket(event->client);
        if (socket >= 0) {
            r->transport_aborted = true;
            // IDF ignores event-handler return codes during body parsing. Shut
            // down only the socket to stop an oversized/continuous stream.
            // The parser/handle remains alive until perform returns to owner.
            ::shutdown(socket, SHUT_RDWR);
        }
    }
    bool call(const char *route, bool post, const char *payload, uint32_t g, hs_json &json,
              const char *override_cookie = nullptr, const void *binary = nullptr,
              size_t binary_bytes = 0, const char *content_type = "application/json",
              uint32_t timeout_ms = 0, bool voice_request = false) {
        json = {};
        if (body) hs_secure_zero(body, response.size + 1);
        response = {}; response.owner = this; response.generation = g; response.voice_request = voice_request;
        const int64_t begin = esp_timer_get_time();
        response.deadline = begin + (timeout_ms ? timeout_ms * 1000LL : post ? 35000000LL : 15000000LL);
        if (!current(g) || (voice_request && !voice_current(g))) {
            response.cancelled = true; response.error = ESP_ERR_INVALID_STATE; return false;
        }
        if (binary_bytes > INT_MAX || (binary_bytes && !binary)) {
            response.error = ESP_ERR_INVALID_SIZE; set_status("上传数据无效", response.error); return false;
        }
        if (!ensure_transport_buffers()) {
            response.error = ESP_ERR_NO_MEM;
            working->http_status = 0;
            set_status("网络缓冲内存不足，请关闭其他应用后刷新", ESP_ERR_NO_MEM, "memory_low");
            retry_reads_at = esp_timer_get_time() + 5000000;
            return false;
        }
        char url[1200], cookie_header[80]{}, authorization[140]{};
        const int n = snprintf(url, sizeof(url), "%s%s", kBaseUrl, route);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(url) || strncmp(route, "/api/", 5)) {
            response.error = ESP_ERR_INVALID_ARG; set_status("接口路径无效", response.error); return false;
        }
        if (override_cookie) { if (*override_cookie) snprintf(cookie_header, sizeof(cookie_header), "hs_sid=%s", override_cookie); }
        else if (device_mode && device_record.token[0]) {
            snprintf(authorization, sizeof(authorization), "Bearer %s", device_record.token);
            response.device_auth = true;
        } else {
            portENTER_CRITICAL(&s_control_mux);
            if (cookie[0]) snprintf(cookie_header, sizeof(cookie_header), "hs_sid=%s", cookie);
            portEXIT_CRITICAL(&s_control_mux);
        }
        esp_http_client_config_t cfg{};
        cfg.url = url; cfg.method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.skip_cert_common_name_check = false;
        cfg.disable_auto_redirect = true;
        cfg.timeout_ms = 1000; cfg.is_async = true;
        cfg.buffer_size = 2048; cfg.buffer_size_tx = 2048;
        cfg.event_handler = http_event; cfg.user_data = &response;
        esp_http_client_handle_t http = esp_http_client_init(&cfg);
        if (!http) { hs_secure_zero(cookie_header, sizeof(cookie_header)); hs_secure_zero(authorization, sizeof(authorization)); response.error = ESP_ERR_NO_MEM; set_status("网络内存不足", response.error); return false; }
        esp_http_client_set_header(http, "Accept", "application/json");
        esp_http_client_set_header(http, "User-Agent", "Hosino-ESP32/1");
        if (cookie_header[0]) esp_http_client_set_header(http, "Cookie", cookie_header);
        if (authorization[0]) esp_http_client_set_header(http, "Authorization", authorization);
        hs_secure_zero(cookie_header, sizeof(cookie_header));
        hs_secure_zero(authorization, sizeof(authorization));
        if (post) {
            esp_http_client_set_header(http, "X-HS", "1");
            esp_http_client_set_header(http, "Content-Type", content_type);
            const char *post_data = binary ? static_cast<const char *>(binary) : payload ? payload : "{}";
            const size_t post_size = binary ? binary_bytes : strlen(post_data);
            esp_http_client_set_post_field(http, post_data, static_cast<int>(post_size));
        }
        esp_err_t result;
        for (;;) {
            if (!current(g) || (voice_request && !voice_current(g))) {
                response.cancelled = true; result = ESP_ERR_INVALID_STATE; break;
            }
            if (response.too_large) { result = ESP_ERR_INVALID_SIZE; break; }
            if (esp_timer_get_time() >= response.deadline) { result = ESP_ERR_TIMEOUT; break; }
            result = esp_http_client_perform(http);
            if (result != ESP_ERR_HTTP_EAGAIN) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        response.status = esp_http_client_get_status_code(http);
        if (response.timed_out) result = ESP_ERR_TIMEOUT;
        response.error = result;
        // No one outside this worker ever closes or frees this handle.
        esp_http_client_close(http); esp_http_client_cleanup(http);
        body[response.size] = 0;
        ESP_LOGI(TAG, "HTTP %s status=%d result=%d bytes=%u elapsed_ms=%u",
                 post ? "POST" : "GET", response.status, result, static_cast<unsigned>(response.size),
                 static_cast<unsigned>((esp_timer_get_time() - begin) / 1000));
        if (!current(g) || response.cancelled || (voice_request && !voice_current(g))) {
            response.cancelled = true; return false;
        }
        working->http_status = response.status;
        if (response.too_large) {
            set_status("响应超过256KiB，请缩小查询或稍后重试", ESP_ERR_INVALID_SIZE, "response_too_large"); return false;
        }
        if (response.status >= 300 && response.status < 400) {
            set_status("服务器要求重定向，已停止以保护登录凭据", ESP_FAIL, "redirect_blocked"); return false;
        }
        if (response.size && response.json_type) hs_json_parse(&json, body, response.size, tokens, kTokenLimit);
        if (response.status == 401) {
            if (response.device_auth) {
                char code[48]{}; field(json, 0, "code", code, sizeof(code));
                if (!strcmp(code, "device_token_invalid") || !strcmp(code, "device_token_expired"))
                    forget_device(g, false);
                set_status("设备凭证暂不可用，请查看配对状态", ESP_ERR_INVALID_STATE, code);
                return false;
            }
            if (!invalidate_auth(true, g)) { response.cancelled = true; return false; }
            set_status("登录已失效，请重新登录", ESP_ERR_INVALID_STATE, "unauthorized"); return false;
        }
        if (result != ESP_OK) {
            set_status("网络请求未完成，请检查连接后刷新", result, "network_error");
            if (!post) { retry_reads_at = esp_timer_get_time() + read_backoff_ms * 1000LL; read_backoff_ms = std::min<uint32_t>(read_backoff_ms * 2, 30000); }
            return false;
        }
        if (response.status < 200 || response.status >= 300) {
            char error[160]{}; field(json, 0, "error", error, sizeof(error));
            field(json, 0, "code", working->error_code, sizeof(working->error_code));
            text(working->status, sizeof(working->status), error[0] ? error : "服务器拒绝请求"); working->last_error = ESP_FAIL;
            working->retry_after_ms = number(json, hs_json_member(&json, 0, "retryAfterMs"));
            if (!working->retry_after_ms) {
                uint32_t seconds = number(json, hs_json_member(&json, 0, "retryAfterSec"));
                working->retry_after_ms = seconds <= UINT32_MAX / 1000 ? seconds * 1000 : UINT32_MAX;
            }
            if (response.status == 423 || response.status == 429) {
                retry_reads_at = esp_timer_get_time() + std::max<uint32_t>(working->retry_after_ms, 3000) * 1000LL;
                if (response.status == 423) working->quiet = true;
            }
            return false;
        }
        if (!json.count || json.tokens[0].type != HS_JSON_OBJECT) {
            set_status("响应格式无效或内容过多", ESP_ERR_INVALID_RESPONSE, "invalid_json"); return false;
        }
        read_backoff_ms = 1000; retry_reads_at = 0; working->retry_after_ms = 0;
        set_status("已更新"); return true;
    }
    void wipe_response() {
        if (body) hs_secure_zero(body, response.size + 1);
        response.size = 0;
        hs_secure_zero(response.cookie, sizeof(response.cookie));
        hs_secure_zero(request, sizeof(request)); hs_secure_zero(quoted, sizeof(quoted)); hs_secure_zero(quoted_key, sizeof(quoted_key));
    }
    bool call_send(const char *route, uint32_t g, hs_json &json) {
        const bool ok = call(route, true, request, g, json);
        if (current(g)) {
            working->send_http_status = working->http_status;
            text(working->send_error_code, sizeof(working->send_error_code), working->error_code);
            text(working->send_status, sizeof(working->send_status), working->status);
        }
        return ok;
    }
    // Only the worker calls these after call() has closed its HTTP handle, or
    // before creating the next handle. Snapshots contain copies, not pointers
    // into these buffers. No GUI thread can read/write/free them.
    void release_transport_buffers(bool log_release = false) {
        size_t released_bytes = 0;
        if (body) {
            released_bytes += kResponseLimit + 1;
            hs_secure_zero(body, kResponseLimit + 1); heap_caps_free(body); body = nullptr;
        }
        if (tokens) {
            released_bytes += kTokenLimit * sizeof(hs_json_token);
            heap_caps_free(tokens); tokens = nullptr;
        }
        if (session_cache) {
            released_bytes += kServerSessionPage * sizeof(Session);
            hs_secure_zero(session_cache, kServerSessionPage * sizeof(Session));
            heap_caps_free(session_cache); session_cache = nullptr;
        }
        response.size = 0; cache_valid = false; cached_count = cached_total = 0;
        if (log_release && released_bytes) {
            ESP_LOGI(TAG, "Background buffers released=%u PSRAM free=%u largest=%u",
                     static_cast<unsigned>(released_bytes),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
        }
    }
    bool ensure_transport_buffers() {
        if (body && tokens && session_cache) return true;
        if (!body) body = static_cast<char *>(heap_caps_malloc(kResponseLimit + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!tokens) tokens = static_cast<hs_json_token *>(heap_caps_calloc(kTokenLimit, sizeof(hs_json_token), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!session_cache) session_cache = static_cast<Session *>(heap_caps_calloc(kServerSessionPage, sizeof(Session), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (body && tokens && session_cache) return true;
        // All-or-nothing allocation: failure keeps snapshots/login alive and
        // leaves a clean retry point, without retaining a partial large heap.
        release_transport_buffers();
        return false;
    }
    bool accept_login_cookie(uint32_t g, uint32_t auth_epoch) {
        portENTER_CRITICAL(&s_control_mux);
        const bool accepted = generation == g && foreground && !stopping && response.cookie[0];
        if (accepted) {
            memcpy(cookie, response.cookie, sizeof(cookie)); cookie_store_epoch = auth_epoch;
            cookie_verified = false; verify_restored = true; pending_login_save = true;
        }
        portEXIT_CRITICAL(&s_control_mux);
        return accepted;
    }
    bool finish_cookie_validation(uint32_t g) {
        char saved_cookie[65]{};
        portENTER_CRITICAL(&s_control_mux);
        const bool accepted = generation == g && foreground && !stopping && cookie[0];
        if (accepted) {
            cookie_verified = true; verify_restored = false;
            if (pending_login_save) memcpy(saved_cookie, cookie, sizeof(cookie));
        }
        portEXIT_CRITICAL(&s_control_mux);
        // An older invalidation can advance the store epoch after this Login
        // was queued. Rebind only the currently validated cookie, never an old
        // request's cookie or a remembered token that has not been verified.
        if (saved_cookie[0]) {
            hs_auth_store_status_t auth{}; hs_auth_store_status(&auth);
            portENTER_CRITICAL(&s_control_mux);
            const bool save_current = generation == g && foreground && !stopping && cookie_verified &&
                                      !strcmp(saved_cookie, cookie);
            if (save_current) cookie_store_epoch = auth.epoch;
            portEXIT_CRITICAL(&s_control_mux);
            if (save_current) {
                // A later opt-out/logout still advances the epoch and rejects
                // this save. Never enable remember or retry a login POST here.
                const esp_err_t saved = auth.remember ? hs_auth_store_save(auth.epoch, saved_cookie) : ESP_ERR_INVALID_STATE;
                if (current(g) && (saved == ESP_OK || !auth.remember)) pending_login_save = false;
            }
        }
        hs_secure_zero(saved_cookie, sizeof(saved_cookie));
        return accepted && current(g);
    }
    bool finish_login_request(uint32_t g, uint32_t id) {
        if (!current(g) || !working->authed) return false;
        set_status("已登录，正在后台同步会话");
        if (!acknowledge_request(g, id)) return false;
        portENTER_CRITICAL(&s_control_mux);
        const bool accepted = generation == g && foreground && !stopping;
        if (accepted) active_post = false;
        portEXIT_CRITICAL(&s_control_mux);
        if (!accepted) return false;
        working->busy = false; initial_sync_due = true; next_sessions = 0;
        ESP_LOGI(TAG, "Login verified: request=%u; continuing read-only sync", static_cast<unsigned>(id));
        publish(g); return true;
    }
    bool read_me(uint32_t g) {
        next_me = esp_timer_get_time() + 60000000;
        hs_json j{}; if (!call("/api/me", false, nullptr, g, j)) {
            if (current(g) && verify_restored) next_me = esp_timer_get_time() + 3000000;
            return false;
        }
        bool authed = false, owner = false;
        if (!hs_json_bool(&j, hs_json_member(&j, 0, "authed"), &authed)) { set_status("登录状态字段缺失", ESP_ERR_INVALID_RESPONSE); return false; }
        hs_json_bool(&j, hs_json_member(&j, 0, "isOwner"), &owner);
        if (device_mode) {
            char kind[24]{}; field(j, 0, "kind", kind, sizeof(kind));
            const int d = hs_json_member(&j, 0, "device");
            const bool activated = d >= 0 && boolean(j, d, "activated");
            working->auth_known = true; working->authed = authed && !strcmp(kind, "device") && activated;
            working->is_owner = false; device_ready = working->authed;
            if (d >= 0) { field(j, d, "id", device_id, sizeof(device_id)); field(j, d, "name", device_label, sizeof(device_label)); }
            if (!device_ready) {
                if (device_record.pending_ack) pair_state = PairState::Activating;
                set_status("设备尚未激活，等待配对确认", ESP_ERR_INVALID_STATE); return false;
            }
            pair_state = PairState::Ready; text(pair_message, sizeof(pair_message), "设备已连接，无需输入密码");
            set_status("设备已登录"); return true;
        }
        if (!authed) { if (!invalidate_auth(true, g)) return false; }
        else if (!finish_cookie_validation(g)) return false;
        verify_restored = false;
        working->auth_known = true; working->authed = authed; working->is_owner = owner;
        set_status(authed ? "已登录" : "请登录星野控制台"); next_me = esp_timer_get_time() + 60000000;
        return true;
    }
    void read_features(uint32_t g) {
        hs_json j{};
        if (call("/api/features", false, nullptr, g, j)) {
            working->features_available = true; working->quiet = boolean(j, 0, "quiet");
            qq_send_enabled = boolean(j, 0, "qqSend");
            field(j, 0, "quietUntil", working->quiet_until, sizeof(working->quiet_until));
        } else { working->features_available = false; qq_send_enabled = false; }
        if (!current(g) || !working->authed) return;
        // Capability discovery only. A missing route does not enable a feature.
        const bool features_ok = working->features_available;
        if (!call("/api/console-features", false, nullptr, g, j) && response.status == 404 && features_ok)
            set_status("基础功能可用，扩展能力清单不可用");
    }
    void read_sessions(uint32_t offset, uint32_t g) {
        next_sessions = esp_timer_get_time() + 60000000;
        const uint32_t page = offset / kServerSessionPage * kServerSessionPage;
        if (cache_valid && cached_page == page && offset != working->session_offset) {
            show_session_page(offset); set_status("会话列表已更新"); return;
        }
        hs_json j{}; snprintf(path, sizeof(path), "/api/sessions?offset=%u", static_cast<unsigned>(page));
        if (!call(path, false, nullptr, g, j)) return;
        const int array = hs_json_member(&j, 0, "sessions");
        if (array < 0 || j.tokens[array].type != HS_JSON_ARRAY) { set_status("会话列表格式暂不支持", ESP_ERR_INVALID_RESPONSE); return; }
        hs_secure_zero(session_cache, kServerSessionPage * sizeof(Session));
        cached_page = page; cached_total = number(j, hs_json_member(&j, 0, "totalCount")); cached_count = 0;
        unsigned received = 0;
        for (int item = hs_json_first(&j, array); item >= 0; item = hs_json_next(&j, array, item)) {
            ++received; char key[kSessionKeySize]; bool cut = false;
            if (!field(j, item, "key", key, sizeof(key), &cut) || cut || !key[0]) continue;
            if (working->selected && !strcmp(key, working->selected_key)) working->selected_active = boolean(j, item, "hasActiveRun");
            if (received > kServerSessionPage) continue;
            Session &s = session_cache[received - 1]; text(s.key, sizeof(s.key), key);
            if (!field(j, item, "displayName", s.name, sizeof(s.name)) || !s.name[0])
                if (!field(j, item, "alias", s.name, sizeof(s.name)) || !s.name[0]) text(s.name, sizeof(s.name), "未命名会话");
            if (!field(j, item, "channel", s.channel, sizeof(s.channel))) text(s.channel, sizeof(s.channel), "未知");
            field(j, item, "model", s.model, sizeof(s.model)); s.active = boolean(j, item, "hasActiveRun");
        }
        cached_count = std::min(received, static_cast<unsigned>(kServerSessionPage)); cache_valid = true;
        show_session_page(offset);
        next_sessions = esp_timer_get_time() + 60000000;
    }
    void show_session_page(uint32_t offset) {
        hs_secure_zero(working->sessions, sizeof(working->sessions)); working->session_count = 0;
        working->session_offset = offset; working->session_total = cached_total;
        const uint32_t start = offset - cached_page;
        for (uint32_t i = start; i < cached_count && i < start + kSessionCount; ++i) {
            if (session_cache[i].key[0]) working->sessions[working->session_count++] = session_cache[i];
        }
        working->sessions_available = true;
        working->sessions_have_more = static_cast<uint64_t>(offset) + kSessionCount < cached_total;
    }
    void read_usage(const hs_json &j) {
        working->usage[0] = 0;
        int array = hs_json_member(&j, 0, "providers");
        if (array < 0 || j.tokens[array].type != HS_JSON_ARRAY) {
            text(working->usage, sizeof(working->usage), "用量接口可访问，字段结构尚未适配"); return;
        }
        unsigned count = 0;
        for (int item = hs_json_first(&j, array); item >= 0; item = hs_json_next(&j, array, item)) {
            char name[80]{}, summary[320]{}, error[160]{};
            if (!field(j, item, "displayName", name, sizeof(name))) field(j, item, "provider", name, sizeof(name));
            field(j, item, "summary", summary, sizeof(summary)); field(j, item, "error", error, sizeof(error));
            append(working->usage, sizeof(working->usage), name[0] ? name : "供应商"); append(working->usage, sizeof(working->usage), "\n");
            if (summary[0]) { append(working->usage, sizeof(working->usage), summary); append(working->usage, sizeof(working->usage), "\n"); }
            if (error[0]) { append(working->usage, sizeof(working->usage), "暂不可用："); append(working->usage, sizeof(working->usage), error); append(working->usage, sizeof(working->usage), "\n"); }
            const int windows = hs_json_member(&j, item, "windows");
            for (int window = hs_json_first(&j, windows); window >= 0; window = hs_json_next(&j, windows, window)) {
                char label[48]{}; double used;
                field(j, window, "label", label, sizeof(label));
                if (!hs_json_number(&j, hs_json_member(&j, window, "usedPercent"), &used)) continue;
                char line[96]; snprintf(line, sizeof(line), "%s：%.1f%%\n", label[0] ? label : "使用量", used);
                append(working->usage, sizeof(working->usage), line);
            }
            ++count; append(working->usage, sizeof(working->usage), "\n");
            if (strlen(working->usage) + 480 >= sizeof(working->usage)) { append(working->usage, sizeof(working->usage), "（其余供应商未显示）"); break; }
        }
        if (!count) text(working->usage, sizeof(working->usage), "暂无供应商用量信息");
        working->usage_available = true;
    }
    void read_overview(const hs_json &j) {
        char *out = working->overview; const size_t cap = sizeof(working->overview); out[0] = 0;
        const int host = hs_json_member(&j, 0, "host"), gw = hs_json_member(&j, 0, "gw");
        auto metric = [&](int object, const char *key, const char *label, const char *unit, bool bytes = false) {
            double v; char line[160];
            if (!hs_json_number(&j, hs_json_member(&j, object, key), &v) || v < 0)
                snprintf(line, sizeof(line), "%s：不可用\n", label);
            else if (bytes && v >= 1073741824.0) snprintf(line, sizeof(line), "%s：%.2f GiB\n", label, v / 1073741824.0);
            else if (bytes) snprintf(line, sizeof(line), "%s：%.1f MiB\n", label, v / 1048576.0);
            else snprintf(line, sizeof(line), "%s：%.1f%s\n", label, v, unit);
            append(out, cap, line);
        };
        metric(host, "cpu", "主机 CPU", "%");
        metric(gw, "cpu", "网关平均 CPU", "%");
        int mem = hs_json_member(&j, host, "mem"), disk = hs_json_member(&j, host, "disk");
        metric(mem, "available", "可用内存", "", true); metric(mem, "total", "内存总量", "", true);
        metric(disk, "free", "根分区可用空间", "", true); metric(disk, "total", "根分区总量", "", true);
        metric(gw, "rssBytes", "网关内存", "", true);
        metric(host, "uptime", "主机运行时间", " 秒"); metric(gw, "uptimeSec", "网关运行时间", " 秒");
        append(out, cap, "主机 CPU 按缓存刷新间隔统计；网关为进程平均值。\n");
        working->overview_available = true;
    }
    void read_history(uint32_t g) {
        next_history = esp_timer_get_time() + 3000000;
        if (!working->selected || !working->selected_key[0]) { set_status("请先选择会话", ESP_ERR_INVALID_STATE); return; }
        char encoded[kSessionKeySize * 3];
        if (!hs_url_encode(working->selected_key, encoded, sizeof(encoded))) { set_status("会话标识过长", ESP_ERR_INVALID_SIZE); return; }
        snprintf(path, sizeof(path), "/api/history?key=%s&limit=10", encoded);
        hs_json j{}; if (!call(path, false, nullptr, g, j)) return;
        const int array = hs_json_member(&j, 0, "messages");
        if (array < 0 || j.tokens[array].type != HS_JSON_ARRAY) { set_status("消息格式暂不支持", ESP_ERR_INVALID_RESPONSE); return; }
        char returned_key[kSessionKeySize]; bool key_cut = false;
        if (field(j, 0, "sessionKey", returned_key, sizeof(returned_key), &key_cut) &&
            (key_cut || strcmp(returned_key, working->selected_key))) { set_status("会话响应不匹配，已丢弃", ESP_ERR_INVALID_RESPONSE); return; }
        char history_channel[sizeof(working->selected_channel)]{}; bool channel_cut = false;
        if (field(j, 0, "channel", history_channel, sizeof(history_channel), &channel_cut) &&
            !channel_cut && history_channel[0]) text(working->selected_channel, sizeof(working->selected_channel), history_channel);
        hs_secure_zero(working->messages, sizeof(working->messages)); working->message_count = 0;
        for (int item = hs_json_first(&j, array); item >= 0 && working->message_count < kMessageCount; item = hs_json_next(&j, array, item)) {
            Message &m = working->messages[working->message_count++];
            field(j, item, "id", m.id, sizeof(m.id)); field(j, item, "role", m.role, sizeof(m.role));
            field(j, item, "senderName", m.sender, sizeof(m.sender));
            const int content = hs_json_member(&j, item, "content");
            if (content >= 0 && j.tokens[content].type == HS_JSON_STRING) hs_json_text(&j, content, m.text, sizeof(m.text), &m.truncated);
            else if (content >= 0 && j.tokens[content].type == HS_JSON_ARRAY) {
                for (int block = hs_json_first(&j, content); block >= 0; block = hs_json_next(&j, content, block)) {
                    char type[24]; if (!field(j, block, "type", type, sizeof(type))) continue;
                    if (!strcmp(type, "text")) {
                        const size_t used = strlen(m.text); bool cut = false;
                        hs_json_text(&j, hs_json_member(&j, block, "text"), m.text + used, sizeof(m.text) - used, &cut);
                        m.truncated |= cut;
                    } else if (!strcmp(type, "image") || !strcmp(type, "image_url")) append(m.text, sizeof(m.text), "[图片]");
                    // Thinking/tool internals are not rendered as an answer.
                }
            }
            const int attachments = hs_json_member(&j, item, "attachments");
            if (hs_json_first(&j, attachments) >= 0) append(m.text, sizeof(m.text), " [附件]");
            char kind[48]{}; field(j, item, "kind", kind, sizeof(kind));
            if (boolean(j, item, "isError") || !strcmp(kind, "run-failed-before-reply")) {
                set_status("服务端本轮回复失败，请查看消息", ESP_FAIL, "run_failed");
                if (!m.text[0]) text(m.text, sizeof(m.text), "[本轮回复失败]");
            }
            if (!m.text[0]) text(m.text, sizeof(m.text), "（无可显示文本）");
        }
        working->history_available = true; working->history_has_more = boolean(j, 0, "hasMore");
        next_history = esp_timer_get_time() + (working->selected_active ? 3000000 : 15000000);
    }
    void select_session(const Command &c) {
        working->selected = true; text(working->selected_key, sizeof(working->selected_key), c.session_key);
        text(working->selected_name, sizeof(working->selected_name), c.label[0] ? c.label : "当前会话");
        text(working->selected_channel, sizeof(working->selected_channel), c.channel[0] ? c.channel : "未知");
        working->selected_active = false; working->message_count = 0; working->history_available = false;
        working->history_has_more = false; hs_secure_zero(working->messages, sizeof(working->messages));
        working->send_uncertain = uncertain_key[0] && !strcmp(uncertain_key, c.session_key);
        working->last_run_id[0] = 0; next_history = 0;
    }
    bool quoted_command(const Command &c) {
        return hs_json_quote(c.text, quoted, sizeof(quoted)) && hs_json_quote(c.session_key, quoted_key, sizeof(quoted_key));
    }
    bool release_voice_audio() {
        if (voice_codec_claimed) {
            const esp_err_t error = bsp_extra_codec_dev_stop();
            if (error != ESP_OK) return false;
            voice_codec_claimed = false;
        }
        if (voice_audio_owned) {
            const esp_err_t error = bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_CONSOLE);
            if (error != ESP_OK) return false;
            voice_audio_owned = false;
        }
        return true;
    }
    void transcribe_mic(Queued &q) {
        const uint32_t g = q.generation;
        // The selected key is captured by the explicit GUI command and never
        // inferred again after recording. This function never submits Send.
        esp_err_t error = ESP_OK;
        if (!working->authed || !working->selected || !q.command.session_key[0] ||
            strcmp(q.command.session_key, working->selected_key)) error = ESP_ERR_INVALID_STATE;
        if (!voice_current(g)) error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) { set_status("正在准备麦克风"); publish(g); }

        uint8_t *wav = nullptr;
        int16_t *raw = nullptr;
        constexpr size_t read_frames = 256;
        static_assert(CODEC_VOICE_INPUT_CHANNELS == 2, "ASR requires the 2.16 stereo STD capture layout");
        constexpr size_t raw_bytes = read_frames * CODEC_VOICE_INPUT_CHANNELS * sizeof(int16_t);
        uint32_t frames = 0;
        int64_t capture_started = 0;
        if (error == ESP_OK) {
            // No HTTP operation is active at this worker boundary. Avoid
            // retaining a full sessions response while allocating microphone PCM.
            wipe_response(); release_transport_buffers();
            wav = static_cast<uint8_t *>(heap_caps_malloc(HS_VOICE_WAV_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            raw = static_cast<int16_t *>(heap_caps_malloc(raw_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (!wav || !raw) error = ESP_ERR_NO_MEM;
        }
        if (error == ESP_OK && !voice_current(g)) error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) {
            error = bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_CONSOLE);
            if (error == ESP_OK) voice_audio_owned = true;
        }
        if (error == ESP_OK && !voice_current(g)) error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) {
            error = bsp_extra_codec_init();
            if (error == ESP_OK) voice_codec_claimed = true;
        }
        if (error == ESP_OK && !voice_current(g)) error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK) error = bsp_extra_codec_set_voice_fs(
            HS_VOICE_SAMPLE_RATE, 16, CODEC_VOICE_INPUT_CHANNELS,
            BSP_EXTRA_ES7210_TDM_ALL_SLOTS_MASK, BSP_EXTRA_ES7210_PHYSICAL_FRONT_MIC_MASK);
        // Capture still needs the shared TX clock, but no speaker monitoring.
        if (error == ESP_OK) error = bsp_extra_codec_mute_set(true);
        if (error == ESP_OK && voice_current(g)) {
            capture_started = esp_timer_get_time();
            int64_t next_publish = capture_started;
            working->voice_state = VoiceState::Recording;
            set_status("正在录音，停止后转为草稿，最长15秒"); publish(g);
            while (voice_current(g) && !voice_finish_requested() && frames < HS_VOICE_MAX_FRAMES &&
                   esp_timer_get_time() - capture_started < HS_VOICE_MAX_SECONDS * 1000000LL) {
                const size_t chunk_frames = std::min<size_t>(read_frames, HS_VOICE_MAX_FRAMES - frames);
                const size_t expected = chunk_frames * CODEC_VOICE_INPUT_CHANNELS * sizeof(int16_t);
                size_t received = 0;
                // The BSP ignores timeout_ms; the codec's current driver read
                // timeout is 1000ms. Never free or forcibly delete during a read.
                error = bsp_extra_i2s_read(raw, expected, &received, 1000);
                if (!voice_current(g)) break;
                if (error != ESP_OK || received != expected) {
                    if (error == ESP_OK) error = ESP_ERR_INVALID_SIZE;
                    break;
                }
                const size_t offset = HS_VOICE_WAV_HEADER_BYTES + static_cast<size_t>(frames) * 2u;
                if (hs_voice_mic1(wav + offset, HS_VOICE_WAV_CAPACITY - offset, raw, chunk_frames) != chunk_frames * 2u) {
                    error = ESP_ERR_INVALID_SIZE; break;
                }
                frames += static_cast<uint32_t>(chunk_frames);
                working->voice_elapsed_ms = frames * 1000u / HS_VOICE_SAMPLE_RATE;
                const int64_t now = esp_timer_get_time();
                if (now >= next_publish) { publish(g); next_publish = now + 250000; }
            }
        }
        const bool finished = capture_started && (voice_finish_requested() || frames >= HS_VOICE_MAX_FRAMES ||
            esp_timer_get_time() - capture_started >= HS_VOICE_MAX_SECONDS * 1000000LL);
        if (!release_voice_audio()) {
            error = ESP_FAIL; // Owner stays held; worker retries cleanup before another command/exit.
            ESP_LOGE(TAG, "ASR codec cleanup pending; retaining audio owner");
        }
        if (raw) { hs_secure_zero(raw, raw_bytes); heap_caps_free(raw); raw = nullptr; }

        if (!voice_current(g)) {
            working->voice_state = VoiceState::Cancelled;
            set_status("语音输入已取消");
        } else if (error != ESP_OK || !finished || frames < HS_VOICE_SAMPLE_RATE / 5u) {
            working->voice_state = VoiceState::Error;
            set_status(error == ESP_ERR_NO_MEM ? "录音内存不足，请关闭其他应用后重试" :
                !voice_audio_owned && error == ESP_ERR_INVALID_STATE ? "音频被占用或会话已改变，请稍后重试" :
                frames < HS_VOICE_SAMPLE_RATE / 5u && error == ESP_OK ? "录音少于0.2秒，请重试" : "录音未完成，请重试",
                error != ESP_OK ? error : ESP_ERR_INVALID_SIZE, "record_failed");
        } else if (!hs_voice_wav_header(wav, HS_VOICE_WAV_CAPACITY, frames)) {
            working->voice_state = VoiceState::Error; set_status("录音格式无效", ESP_ERR_INVALID_SIZE, "wav_invalid");
        } else {
            working->voice_state = VoiceState::Uploading;
            set_status("正在转写录音，请稍候"); publish(g);
            hs_json j{};
            const size_t bytes = HS_VOICE_WAV_HEADER_BYTES + static_cast<size_t>(frames) * 2u;
            const bool ok = call("/api/asr/transcribe", true, nullptr, g, j, nullptr,
                                 wav, bytes, "audio/wav", 60000, true);
            if (!voice_current(g)) {
                working->voice_state = VoiceState::Cancelled;
                set_status("语音输入已取消");
            } else if (ok) {
                bool cut = false;
                if (boolean(j, 0, "ok") && field(j, 0, "text", working->voice_text, sizeof(working->voice_text), &cut) &&
                    !cut && working->voice_text[0]) {
                    portENTER_CRITICAL(&s_control_mux);
                    const bool accepted = !stopping && foreground && generation == g &&
                        voice_pending && !voice_cancel && voice_generation == g && voice_serial == q.voice_serial;
                    if (accepted) {
                        working->voice_state = VoiceState::Ready;
                        working->voice_result_revision = ++voice_result_counter;
                    }
                    portEXIT_CRITICAL(&s_control_mux);
                    if (accepted) set_status("转写已完成，请检查草稿后再发送");
                    else {
                        hs_secure_zero(working->voice_text, sizeof(working->voice_text));
                        working->voice_state = VoiceState::Cancelled;
                        set_status("语音输入已取消");
                    }
                } else {
                    hs_secure_zero(working->voice_text, sizeof(working->voice_text));
                    working->voice_state = VoiceState::Error;
                    set_status(cut ? "转写文字过长，请分段录音" : "未识别出文字，请重试", ESP_ERR_INVALID_RESPONSE, "asr_text_invalid");
                }
            } else {
                working->voice_state = VoiceState::Error;
                if (response.status == 404) set_status("语音接口尚未部署，请稍后重试", ESP_ERR_NOT_SUPPORTED, "asr_unavailable");
                else if (response.status == 503) set_status("语音服务暂不可用，请稍后重试", ESP_FAIL, "asr_unavailable");
                // Other transport/server errors already have an explicit status. No POST retry.
            }
        }
        if (wav) { hs_secure_zero(wav, HS_VOICE_WAV_CAPACITY); heap_caps_free(wav); }
        portENTER_CRITICAL(&s_control_mux);
        if (voice_generation == g) { voice_pending = false; voice_running = false; }
        portEXIT_CRITICAL(&s_control_mux);
    }
    static bool base64url(const char *s, size_t length) {
        if (!s || strlen(s) != length) return false;
        for (size_t i = 0; i < length; ++i) {
            const char c = s[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        }
        return true;
    }
    static bool valid_pair_id(const char *s) { return s && !strncmp(s, "hsp_", 4) && base64url(s + 4, 22); }
    static bool valid_device_token(const char *s) {
        if (!s || strlen(s) != 60 || strncmp(s, "hsd_", 4) || s[16] != '.') return false;
        for (unsigned i = 4; i < 16; ++i) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
        return base64url(s + 17, 43);
    }
    void clear_pair_secret() {
        hs_secure_zero(pair_id, sizeof(pair_id)); hs_secure_zero(pair_secret, sizeof(pair_secret));
        hs_secure_zero(pair_user_code, sizeof(pair_user_code)); pair_expires = 0;
    }
    void pair_status(PairState state, const char *message) {
        pair_state = state; text(pair_message, sizeof(pair_message), message);
    }
    void stop_pair(bool cancelled) {
        clear_pair_secret();
        portENTER_CRITICAL(&s_control_mux); pair_running = false; portEXIT_CRITICAL(&s_control_mux);
        if (cancelled && pair_state != PairState::Ready) pair_status(PairState::Cancelled, "配对已停止；已保存的凭证可在重开时继续验证");
    }
    void forget_device(uint32_t g, bool explicit_user) {
        if (!current(g)) return;
        uint32_t ticket = 0;
        const esp_err_t err = hs_device_store_forget(explicit_user ? UINT32_MAX : device_epoch, &ticket);
        if (err != ESP_OK) { pair_status(PairState::Error, "设备凭证删除未完成，请重试解除绑定"); return; }
        device_ticket = ticket; device_store_op = DeviceStoreOp::Forget;
        const bool was_device = device_mode;
        device_mode = false; device_ready = false; device_restore_due = false; restore_due = false;
        stop_pair(false); if (was_device) clear_private(*working);
        pair_status(PairState::Storing, "正在删除本机设备凭证");
    }
    void start_pair(uint32_t g, const char *name) {
        hs_device_store_status_t storage{};
        if (hs_device_store_init() != ESP_OK) { pair_status(PairState::Error, "设备存储任务不可用，请关闭其他应用后重试"); return; }
        hs_device_store_status(&storage);
        if (!storage.ready || storage.busy) { pair_status(PairState::Error, "设备存储尚未准备好，请稍后重试"); return; }
        if (device_saved || (storage.saved_known && storage.saved) || device_store_op != DeviceStoreOp::None) {
            if (storage.saved_known && storage.saved) device_saved = true;
            set_status("请先解除已有设备绑定或等待保存完成", ESP_ERR_INVALID_STATE); return;
        }
        device_restore_due = false; restore_due = false; verify_restored = false; pending_login_save = false;
        device_mode = false; device_ready = false; device_suppressed = false; clear_private(*working); clear_pair_secret();
        portENTER_CRITICAL(&s_control_mux); pair_running = true; pair_cancel_requested = false; portEXIT_CRITICAL(&s_control_mux);
        pair_status(PairState::Starting, "正在申请配对码"); publish(g);
        if (!hs_json_quote(name[0] ? name : "星野桌面助手", quoted, sizeof(quoted))) {
            pair_status(PairState::Error, "设备名称无效"); stop_pair(false); return;
        }
        snprintf(request, sizeof(request), "{\"name\":%s}", quoted);
        hs_json j{};
        if (!call("/api/devices/pair/start", true, request, g, j, "", nullptr, 0, "application/json", 15000)) {
            if (current(g)) { pair_status(PairState::Error, "申请配对码失败，请检查网络后手动重试"); stop_pair(false); }
            return;
        }
        bool cut = false;
        if (!field(j, 0, "pairId", pair_id, sizeof(pair_id), &cut) || cut || !valid_pair_id(pair_id) ||
            !field(j, 0, "pollSecret", pair_secret, sizeof(pair_secret), &cut) || cut || !base64url(pair_secret, 43) ||
            !field(j, 0, "userCode", pair_user_code, sizeof(pair_user_code), &cut) || cut || strlen(pair_user_code) != 9 || pair_user_code[4] != '-') {
            pair_status(PairState::Error, "配对响应无效，请手动重新申请"); stop_pair(false); return;
        }
        const uint32_t ttl = number(j, hs_json_member(&j, 0, "expiresIn"));
        for (unsigned i = 0; i < 9; ++i) if (i != 4 && !strchr("ABCDEFGHJKLMNPQRSTUVWXYZ23456789", pair_user_code[i])) {
            pair_status(PairState::Error, "配对码格式无效"); stop_pair(false); return;
        }
        if (!ttl || ttl > 300) { pair_status(PairState::Error, "配对有效期无效"); stop_pair(false); return; }
        pair_interval = std::max<uint32_t>(3, std::min<uint32_t>(number(j, hs_json_member(&j, 0, "interval"), 3), 60));
        pair_expires = esp_timer_get_time() + ttl * 1000000LL; pair_next = esp_timer_get_time() + pair_interval * 1000000LL;
        pair_status(PairState::Waiting, "在网页登录后输入配对码并批准此设备"); set_status("等待网页批准设备");
    }
    void poll_pair(uint32_t g) {
        if (esp_timer_get_time() >= pair_expires) { pair_status(PairState::Expired, "配对码已过期，请手动重新申请"); stop_pair(false); return; }
        snprintf(request, sizeof(request), "{\"pairId\":\"%s\",\"pollSecret\":\"%s\"}", pair_id, pair_secret);
        hs_json j{};
        const bool ok = call("/api/devices/pair/poll", true, request, g, j, "", nullptr, 0, "application/json", 15000);
        if (!current(g)) return;
        pair_next = esp_timer_get_time() + pair_interval * 1000000LL;
        if (!ok) {
            if (response.status == 404 || response.status == 410) { pair_status(PairState::Expired, "配对已失效，请手动重新申请"); stop_pair(false); }
            else if (response.status == 429) {
                pair_interval = std::max<uint32_t>(pair_interval + 1, std::max<uint32_t>(number(j, hs_json_member(&j, 0, "interval"), 3), (working->retry_after_ms + 999) / 1000));
                pair_next = esp_timer_get_time() + pair_interval * 1000000LL;
                text(pair_message, sizeof(pair_message), "服务器要求稍后查询，正在等待");
            } else text(pair_message, sizeof(pair_message), "网络暂不可用，配对码有效期内继续等待");
            return;
        }
        char state[24]{}; field(j, 0, "status", state, sizeof(state));
        if (!strcmp(state, "pending")) {
            pair_interval = std::min<uint32_t>(60, std::max<uint32_t>(3, number(j, hs_json_member(&j, 0, "interval"), pair_interval)));
            pair_next = esp_timer_get_time() + pair_interval * 1000000LL; return;
        }
        if (!strcmp(state, "denied")) { pair_status(PairState::Denied, "网页已拒绝配对，请手动重新申请"); stop_pair(false); return; }
        bool cut = false;
        if (strcmp(state, "approved") || !field(j, 0, "token", device_record.token, sizeof(device_record.token), &cut) ||
            cut || !valid_device_token(device_record.token)) {
            hs_secure_zero(&device_record, sizeof(device_record)); pair_status(PairState::Error, "设备授权响应无效"); stop_pair(false); return;
        }
        device_record.schema = HS_DEVICE_STORE_SCHEMA; device_record.pending_ack = true;
        text(device_record.pair_id, sizeof(device_record.pair_id), pair_id);
        const int d = hs_json_member(&j, 0, "device");
        if (d >= 0) { field(j, d, "id", device_id, sizeof(device_id)); field(j, d, "name", device_label, sizeof(device_label)); }
        hs_device_store_status_t storage{}; hs_device_store_status(&storage); device_epoch = storage.epoch;
        const esp_err_t err = hs_device_store_save(device_epoch, &device_record, &device_ticket);
        if (err != ESP_OK) { hs_secure_zero(&device_record, sizeof(device_record)); pair_status(PairState::Error, "设备凭证保存未开始，请手动重试配对"); stop_pair(false); return; }
        device_store_op = DeviceStoreOp::SaveNew; pair_status(PairState::Storing, "正在保存设备凭证，保存成功后才激活");
    }
    bool service_device(uint32_t g) {
        bool cancel, resume, queued;
        portENTER_CRITICAL(&s_control_mux);
        cancel = pair_cancel_requested; pair_cancel_requested = false;
        resume = device_resume_due; device_resume_due = false; queued = count != 0;
        portEXIT_CRITICAL(&s_control_mux);
        if (cancel) stop_pair(true);
        if (!current(g)) return false;
        if (resume && device_mode && device_saved && pair_state == PairState::Cancelled) {
            pair_status(device_record.pending_ack ? PairState::Activating : PairState::Verifying, "正在恢复设备验证"); pair_next = 0;
        }
        if (queued) return false;
        if (device_store_op != DeviceStoreOp::None) {
            hs_device_store_record_t result{}; uint32_t epoch = 0; esp_err_t err = ESP_OK;
            if (!hs_device_store_take(device_ticket, &result, &epoch, &err)) return true;
            const DeviceStoreOp op = device_store_op; device_store_op = DeviceStoreOp::None; device_ticket = 0; device_epoch = epoch;
            if (err != ESP_OK) {
                hs_device_store_status_t storage{}; hs_device_store_status(&storage);
                if (storage.saved_known) device_saved = storage.saved;
                if (op == DeviceStoreOp::Restore) restore_due = false;
                hs_secure_zero(&result, sizeof(result));
                if (op == DeviceStoreOp::SaveAck && device_mode && device_saved && !device_suppressed) {
                    // SaveNew was confirmed before ACK. Until the cleared flag
                    // is durably confirmed, retain the recoverable ACK path.
                    device_record.pending_ack = true; device_ready = false; working->authed = false;
                    pair_status(PairState::Activating, "激活记录保存失败，将重试确认和保存");
                    pair_next = esp_timer_get_time() + device_store_retry_ms * 1000LL;
                    device_store_retry_ms = std::min<uint32_t>(device_store_retry_ms * 2, 30000);
                    publish(g); return true;
                }
                pair_status(PairState::Error, "设备存储操作失败，未发送激活确认"); publish(g); return false;
            }
            device_store_retry_ms = 3000;
            if (op == DeviceStoreOp::Restore) {
                device_restore_due = false;
                if (result.token[0]) {
                    device_record = result; device_saved = true;
                    if (!device_suppressed) {
                        device_mode = true; restore_due = false;
                        portENTER_CRITICAL(&s_control_mux); pair_running = true; portEXIT_CRITICAL(&s_control_mux);
                        pair_status(device_record.pending_ack ? PairState::Activating : PairState::Verifying, "正在恢复设备连接"); pair_next = 0;
                    }
                }
            } else if (op == DeviceStoreOp::Forget) {
                hs_secure_zero(&device_record, sizeof(device_record)); device_saved = false; device_mode = false; device_ready = false;
                device_label[0] = 0; device_id[0] = 0; pair_status(PairState::Idle, "本机设备凭证已删除，可重新配对或使用密码");
            } else {
                device_saved = true; device_mode = !device_suppressed;
                clear_pair_secret();
                if (pair_state != PairState::Cancelled && !device_suppressed) {
                    pair_status(op == DeviceStoreOp::SaveNew ? PairState::Activating : PairState::Verifying, "正在验证已保存设备凭证"); pair_next = 0;
                }
            }
            hs_secure_zero(&result, sizeof(result)); publish(g);
        }
        if (device_restore_due) {
            hs_device_store_status_t storage{}; hs_device_store_status(&storage);
            if (!storage.ready) return true;
            device_restore_due = false; device_epoch = storage.epoch;
            if (storage.error != ESP_OK || hs_device_store_load(device_epoch, &device_ticket) != ESP_OK) {
                restore_due = false;
                if (storage.saved_known) device_saved = storage.saved;
                pair_status(PairState::Error, "设备凭证读取失败，可使用密码登录"); publish(g); return false;
            }
            device_store_op = DeviceStoreOp::Restore; publish(g); return true;
        }
        if (pair_state == PairState::Waiting && esp_timer_get_time() >= pair_next) {
            poll_pair(g); publish(g); wipe_response(); return true;
        }
        if (device_mode && device_saved && pair_state == PairState::Activating && esp_timer_get_time() >= pair_next) {
            snprintf(request, sizeof(request), "{\"pairId\":\"%s\"}", device_record.pair_id);
            hs_json j{}; const bool ok = call("/api/devices/pair/ack", true, request, g, j, nullptr, nullptr, 0, "application/json", 15000);
            if (!current(g)) return true;
            pair_next = esp_timer_get_time() + 3000000;
            if (ok && boolean(j, 0, "ok")) {
                pair_status(PairState::Verifying, "激活响应已收到，正在验证设备身份"); pair_next = 0;
            } else if (device_mode) text(pair_message, sizeof(pair_message), "激活暂未确认，将重试幂等确认，不会重新申请配对");
            publish(g); wipe_response(); return true;
        }
        if (device_mode && device_saved && pair_state == PairState::Verifying && esp_timer_get_time() >= pair_next) {
            pair_next = esp_timer_get_time() + 3000000;
            if (read_me(g) && working->authed) {
                if (device_record.pending_ack) {
                    device_record.pending_ack = false;
                    if (hs_device_store_save(device_epoch, &device_record, &device_ticket) == ESP_OK) {
                        device_store_op = DeviceStoreOp::SaveAck; pair_status(PairState::Storing, "身份已验证，正在保存激活状态");
                    } else { device_record.pending_ack = true; pair_status(PairState::Verifying, "正在等待本机存储可写"); }
                    working->authed = false; device_ready = false;
                } else { stop_pair(false); initial_sync_due = true; next_sessions = 0; }
            }
            publish(g); wipe_response(); return true;
        }
        return pair_state == PairState::Waiting || pair_state == PairState::Storing || pair_state == PairState::Activating || pair_state == PairState::Verifying ||
               (pair_state == PairState::Cancelled && device_mode && !device_ready);
    }
    void execute(Queued &q) {
        const uint32_t g = q.generation; Command &c = q.command;
        if (!current(g)) return;
        if (c.type == CommandType::Login) set_status("正在验证密码，请稍候");
        const bool sending = c.type == CommandType::Send || c.type == CommandType::SendQQ;
        const bool blocked_uncertain = sending && uncertain_key[0] && !strcmp(uncertain_key, c.session_key);
        if (sending && !blocked_uncertain) {
            working->http_status = 0;
            working->send_http_status = 0;
            working->send_error_code[0] = 0;
            working->send_status[0] = 0;
            text(working->send_result_key, sizeof(working->send_result_key), c.session_key);
        }
        if (c.type == CommandType::TranscribeMic) {
            // The first busy snapshot belongs to this request, never an old
            // Ready/Error result left over from the previous recording.
            working_voice_generation = g; working_voice_serial = q.voice_serial;
            working->voice_state = VoiceState::Preparing;
            working->voice_elapsed_ms = 0;
            hs_secure_zero(working->voice_text, sizeof(working->voice_text));
            text(working->voice_session_key, sizeof(working->voice_session_key), c.session_key);
        }
        working->busy = true; publish(g);
        hs_json j{};
        if (c.type != CommandType::Login && c.type != CommandType::Logout && c.type != CommandType::RefreshStatus &&
            c.type != CommandType::PairStart && c.type != CommandType::PairCancel && c.type != CommandType::ForgetDevice && !working->authed) {
            set_status("请先登录", ESP_ERR_INVALID_STATE, "unauthorized");
            if (c.type == CommandType::TranscribeMic) working->voice_state = VoiceState::Error;
        } else if (blocked_uncertain) {
            set_status("请先核对上次发送结果，不会自动重发", ESP_ERR_INVALID_STATE, "send_uncertain");
        } else switch (c.type) {
        case CommandType::PairStart: start_pair(g, c.text); break;
        case CommandType::PairCancel: stop_pair(true); set_status("配对已停止"); break;
        case CommandType::ForgetDevice:
            if (!c.confirm) set_status("请先确认解除本机设备绑定", ESP_ERR_INVALID_ARG);
            else { forget_device(g, true); set_status("正在删除本机设备凭证"); }
            break;
        case CommandType::Login:
            stop_pair(false); device_restore_due = false; device_mode = false; device_ready = false; device_suppressed = true;
            pair_status(PairState::Idle, "已切换为密码登录");
            if (!hs_json_quote(c.text, quoted, sizeof(quoted))) { set_status("密码格式或长度无效", ESP_ERR_INVALID_ARG); break; }
            snprintf(request, sizeof(request), "{\"password\":%s}", quoted);
            if (call("/api/login", true, request, g, j, "")) {
                if (!response.cookie[0] || !boolean(j, 0, "ok")) { set_status("登录响应缺少有效会话", ESP_ERR_INVALID_RESPONSE); break; }
                const bool accepted = accept_login_cookie(g, q.auth_epoch);
                hs_secure_zero(c.text, sizeof(c.text)); wipe_response();
                if (!accepted) break;
                set_status("密码已受理，正在验证登录状态"); publish(g);
                if (read_me(g) && working->authed) {
                    if (finish_login_request(g, c.request_id)) c.request_id = 0;
                }
            }
            break;
        case CommandType::Logout: {
            if (device_mode) { set_status("设备登录请使用解除本机绑定", ESP_ERR_NOT_SUPPORTED); break; }
            const bool remote = q.logout_cookie[0] && call("/api/logout", true, "{}", g, j, q.logout_cookie);
            if (invalidate_auth(false, g)) set_status(remote ? "已退出登录" : "本机凭据已清除；服务器退出未确认", remote ? ESP_OK : ESP_FAIL);
            break;
        }
        case CommandType::RefreshStatus:
            if (device_mode && pair_state == PairState::Cancelled && !device_ready) {
                set_status("设备验证已暂停，重新打开应用可继续"); break;
            }
            if (read_me(g) && working->authed) read_features(g);
            break;
        case CommandType::RefreshSessions: read_sessions(c.offset, g); break;
        case CommandType::SelectSession:
            select_session(c); publish(g); read_history(g); break;
        case CommandType::CreateSession:
            if (!hs_json_quote(c.text[0] ? c.text : "星野桌面助手", quoted_key, sizeof(quoted_key))) { set_status("会话名称无效", ESP_ERR_INVALID_ARG); break; }
            snprintf(request, sizeof(request), "{\"label\":%s,\"displayName\":%s}", quoted_key, quoted_key);
            if (call("/api/sessions/create", true, request, g, j)) {
                char key[kSessionKeySize]; bool cut;
                bool started;
                if (!boolean(j, 0, "ok") || !hs_json_bool(&j, hs_json_member(&j, 0, "runStarted"), &started) || started ||
                    !field(j, 0, "key", key, sizeof(key), &cut) || cut || !dashboard(key)) { set_status("新建结果未确认，请检查会话列表后再建", ESP_ERR_INVALID_RESPONSE, "create_uncertain"); break; }
                text(c.session_key, sizeof(c.session_key), key); text(c.label, sizeof(c.label), c.text[0] ? c.text : "星野桌面助手");
                text(c.channel, sizeof(c.channel), "dashboard"); select_session(c); publish(g); read_history(g);
            } else if (hs_failed_post_is_uncertain(response.headers_sent, response.cancelled, response.error == ESP_OK, response.status))
                set_status("新建结果未确认，请检查会话列表后再建", ESP_FAIL, "create_uncertain");
            break;
        case CommandType::RefreshHistory: read_history(g); break;
        case CommandType::ResolveUncertain:
            if (!c.confirm || !working->selected || strcmp(c.session_key, working->selected_key) ||
                !uncertain_key[0] || strcmp(c.session_key, uncertain_key)) {
                set_status("会话或待核对状态已变化，请重新查看", ESP_ERR_INVALID_STATE);
                break;
            }
            // Local acknowledgement only: no network, no accepted-send ACK, no draft mutation.
            hs_secure_zero(uncertain_key, sizeof(uncertain_key));
            working->send_uncertain = false;
            set_status("已恢复发送；未重发任何消息");
            break;
        case CommandType::SendQQ:
            if (!working->selected || strcmp(c.session_key, working->selected_key) ||
                strcmp(working->selected_channel, "qqbot") || !c.confirm) {
                set_status("请确认当前 QQ 会话和发送内容", ESP_ERR_INVALID_STATE); break;
            }
            if (!qq_send_enabled || working->quiet) { set_status("QQ 直发暂不可用，请查看功能开关或静默时段", ESP_ERR_NOT_SUPPORTED); break; }
            if (!quoted_command(c)) { set_status("消息内容或会话标识过长", ESP_ERR_INVALID_SIZE); break; }
            snprintf(request, sizeof(request), "{\"sessionKey\":%s,\"text\":%s,\"confirm\":true}", quoted_key, quoted);
            if (call_send("/api/qq/send", g, j)) {
                char message_id[160]{}, channel[24]{}; bool cut = false;
                if (!field(j, 0, "messageId", message_id, sizeof(message_id), &cut) || cut || !message_id[0] ||
                    !field(j, 0, "channel", channel, sizeof(channel)) || strcmp(channel, "qqbot")) {
                    text(uncertain_key, sizeof(uncertain_key), c.session_key); working->send_uncertain = true;
                    set_status("QQ 发送结果待确认，不会自动重发", ESP_ERR_INVALID_RESPONSE, "send_uncertain"); break;
                }
                working->send_uncertain = false; uncertain_key[0] = 0;
                acknowledge_send(g, c.request_id);
                set_status("QQ 消息已发送"); next_history = esp_timer_get_time() + 3000000;
            } else if (hs_failed_post_is_uncertain(response.headers_sent, response.cancelled, response.error == ESP_OK, response.status)) {
                text(uncertain_key, sizeof(uncertain_key), c.session_key);
                if (current(g)) { working->send_uncertain = true; set_status("QQ 发送结果待确认，不会自动重发", ESP_FAIL, "send_uncertain"); }
            }
            break;
        case CommandType::Send:
        case CommandType::Abort:
            if (!working->selected || strcmp(c.session_key, working->selected_key)) { set_status("会话已切换，请重新确认操作", ESP_ERR_INVALID_STATE); break; }
            if (!c.session_key[0]) { set_status("请先选择会话", ESP_ERR_INVALID_STATE); break; }
            if (!quoted_command(c)) { set_status("消息内容或会话标识过长", ESP_ERR_INVALID_SIZE); break; }
            if (c.type == CommandType::Send) snprintf(request, sizeof(request), "{\"key\":%s,\"message\":%s,\"deliver\":%s}", quoted_key, quoted, c.deliver ? "true" : "false");
            else snprintf(request, sizeof(request), "{\"key\":%s}", quoted_key);
            if (c.type == CommandType::Send ? call_send("/api/send", g, j) : call("/api/abort", true, request, g, j)) {
                if (c.type == CommandType::Send) {
                    char accepted[24]{}; bool cut;
                    field(j, 0, "status", accepted, sizeof(accepted));
                    if (!field(j, 0, "runId", working->last_run_id, sizeof(working->last_run_id), &cut) || cut ||
                        !working->last_run_id[0] || strcmp(accepted, "started")) {
                        text(uncertain_key, sizeof(uncertain_key), c.session_key); working->send_uncertain = true;
                        set_status("发送响应无法确认，请查历史；不会重发", ESP_ERR_INVALID_RESPONSE, "send_uncertain"); break;
                    }
                    working->selected_active = true; working->send_uncertain = false; uncertain_key[0] = 0;
                    acknowledge_send(g, c.request_id);
                    set_status("请求已受理，等待回复"); next_history = esp_timer_get_time() + 3000000;
                } else {
                    bool aborted = false;
                    if (!boolean(j, 0, "ok") || !hs_json_bool(&j, hs_json_member(&j, 0, "aborted"), &aborted))
                        set_status("中止结果未确认，请刷新状态", ESP_ERR_INVALID_RESPONSE);
                    else set_status(aborted ? "已中止当前回复" : "当前没有可中止的回复");
                }
            } else if (c.type == CommandType::Send && hs_failed_post_is_uncertain(response.headers_sent,
                       response.cancelled, response.error == ESP_OK, response.status)) {
                text(uncertain_key, sizeof(uncertain_key), c.session_key);
                if (current(g)) { working->send_uncertain = true; set_status("发送结果待确认，请查历史；不会自动重发", ESP_FAIL, "send_uncertain"); }
            }
            break;
        case CommandType::Overview:
        case CommandType::Usage: {
            const bool overview = c.type == CommandType::Overview;
            if (call(overview ? "/api/overview" : "/api/usage", false, nullptr, g, j)) {
                if (overview) {
                    read_overview(j);
                } else read_usage(j);
            }
            break;
        }
        case CommandType::Models:
            if (call("/api/models", false, nullptr, g, j)) {
                int array = hs_json_member(&j, 0, "models");
                if (array < 0 || j.tokens[array].type != HS_JSON_ARRAY) { set_status("模型列表格式暂不支持", ESP_ERR_INVALID_RESPONSE); break; }
                working->model_count = 0; working->model_total = 0; hs_secure_zero(working->models, sizeof(working->models));
                for (int item = hs_json_first(&j, array); item >= 0; item = hs_json_next(&j, array, item)) {
                    ++working->model_total; if (working->model_count >= kModelCount) continue;
                    Model &m = working->models[working->model_count++];
                    field(j, item, "id", m.id, sizeof(m.id)); field(j, item, "name", m.name, sizeof(m.name));
                    field(j, item, "provider", m.provider, sizeof(m.provider));
                    if (!field(j, item, "availability", m.availability, sizeof(m.availability))) text(m.availability, sizeof(m.availability), "unknown");
                }
                working->models_available = true; working->models_truncated = working->model_total > working->model_count;
            }
            break;
        case CommandType::SearchMemory:
            if (!hs_json_quote(c.text, quoted, sizeof(quoted))) { set_status("搜索词无效", ESP_ERR_INVALID_ARG); break; }
            snprintf(request, sizeof(request), "{\"q\":%s,\"all\":true,\"limit\":6}", quoted);
            if (call("/api/memory/search", true, request, g, j)) {
                int array = hs_json_member(&j, 0, "results");
                if (array < 0 || j.tokens[array].type != HS_JSON_ARRAY) { set_status("记忆搜索格式暂不支持", ESP_ERR_INVALID_RESPONSE); break; }
                working->memory_count = 0; hs_secure_zero(working->memories, sizeof(working->memories));
                for (int item = hs_json_first(&j, array); item >= 0 && working->memory_count < kMemoryCount; item = hs_json_next(&j, array, item)) {
                    Memory &m = working->memories[working->memory_count++]; double score;
                    field(j, item, "memory", m.text, sizeof(m.text), &m.truncated); field(j, item, "user_id", m.user, sizeof(m.user));
                    m.has_score = hs_json_number(&j, hs_json_member(&j, item, "score"), &score); if (m.has_score) m.score = static_cast<float>(score);
                }
                working->memory_available = true;
            }
            break;
        case CommandType::TranscribeMic:
            transcribe_mic(q);
            break;
        }
        if (sending && !blocked_uncertain && current(g)) {
            if (!working->send_status[0] || (working->send_http_status >= 200 && working->send_http_status < 300)) {
                text(working->send_error_code, sizeof(working->send_error_code), working->error_code);
                text(working->send_status, sizeof(working->send_status), working->status);
            }
            ESP_LOGI(TAG, "Send result: mode=%s http=%d uncertain=%d",
                c.type == CommandType::SendQQ ? "direct_qq" : c.deliver ? "ai_channel" : "ai_device",
                working->send_http_status, uncertain_key[0] && !strcmp(uncertain_key, c.session_key));
        }
        // Polls must not overwrite a UI command's completion. A generation
        // replaced by navigation/pause cannot acknowledge an unrelated view.
        if (acknowledge_request(g, c.request_id)) {
            ESP_LOGI(TAG, "Request done: id=%u command=%u error=%s send_accepted=%d",
                static_cast<unsigned>(c.request_id), static_cast<unsigned>(c.type),
                esp_err_to_name(working->last_error), working->accepted_send_request_id == c.request_id);
        }
        working->busy = false; publish(g); wipe_response();
    }
    void release_buffers() {
        if (working) { hs_secure_zero(working, sizeof(Snapshot)); heap_caps_free(working); working = nullptr; }
        if (publish_lock) xSemaphoreTake(publish_lock, portMAX_DELAY);
        if (published) { hs_secure_zero(published, sizeof(Snapshot)); heap_caps_free(published); published = nullptr; }
        if (publish_lock) xSemaphoreGive(publish_lock);
        release_transport_buffers();
    }
    static void free_impl(Impl *p) {
        if (!p) return;
        p->release_buffers();
        if (p->publish_lock) vSemaphoreDelete(p->publish_lock);
        p->~Impl(); hs_secure_zero(p, sizeof(*p)); heap_caps_free(p);
    }
    static void worker_task(void *arg) {
        auto *p = static_cast<Impl *>(arg);
        for (;;) {
            if (p->service_device(p->current_generation())) { vTaskDelay(pdMS_TO_TICKS(40)); continue; }
            if (!p->device_mode && p->restore_login()) { vTaskDelay(pdMS_TO_TICKS(40)); continue; }
            Queued q{}; bool have = false, stop, foreground, clear, status, trim;
            uint32_t g;
            portENTER_CRITICAL(&s_control_mux);
            stop = p->stopping; foreground = p->foreground; clear = p->clear_auth;
            status = p->status_due; p->status_due = false; g = p->generation;
            trim = p->trim_due; p->trim_due = false;
            if (p->count) {
                q = p->queue[p->head]; hs_secure_zero(&p->queue[p->head], sizeof(Queued));
                p->head = (p->head + 1) % kQueueLimit; --p->count; have = true;
                p->active = true; p->active_post = explicit_post(q.command.type);
                if (q.command.type == CommandType::TranscribeMic) p->voice_running = true;
            }
            portEXIT_CRITICAL(&s_control_mux);
            // Reaching this boundary means any prior call() already cleaned
            // up HTTP. A rapid resume does not cancel a pending trim request.
            if (trim || (!foreground && (p->body || p->tokens || p->session_cache))) {
                p->wipe_response();
                p->release_transport_buffers(true);
            }
            // A failed codec close cannot surrender the shared owner or free
            // this worker. Retry safely; the GUI remains asynchronous.
            if ((p->voice_audio_owned || p->voice_codec_claimed) && !p->release_voice_audio()) {
                // Keep an already-popped command owned by the worker until
                // cleanup is safe; do not lose or execute it prematurely.
                while (!p->release_voice_audio()) vTaskDelay(pdMS_TO_TICKS(1000));
            }
            if (clear) {
                p->verify_restored = false; p->pending_login_save = false;
                clear_private(*p->working); p->set_status("本机登录信息已清除"); p->publish(g);
                portENTER_CRITICAL(&s_control_mux); if (p->generation == g) p->clear_auth = false; portEXIT_CRITICAL(&s_control_mux);
            }
            if (stop) { hs_secure_zero(&q, sizeof(q)); break; }
            if (have) {
                if (q.generation == g && foreground) p->execute(q);
                portENTER_CRITICAL(&s_control_mux);
                p->active = false; p->active_post = false;
                if (q.command.type == CommandType::TranscribeMic && p->voice_generation == q.generation) {
                    p->voice_pending = false; p->voice_running = false;
                }
                portEXIT_CRITICAL(&s_control_mux);
                hs_secure_zero(&q, sizeof(q)); continue;
            }
            if (foreground) {
                const int64_t now = esp_timer_get_time();
                if (p->initial_sync_due && p->working->authed && now >= p->retry_reads_at) {
                    p->initial_sync_due = false;
                    p->read_features(g); p->publish(g); p->wipe_response();
                } else if (status || ((p->working->authed || p->verify_restored) && now >= p->next_me && now >= p->retry_reads_at)) {
                    q.generation = g; q.command.type = CommandType::RefreshStatus; p->execute(q);
                } else if (p->working->authed && now >= p->retry_reads_at) {
                    if (p->working->selected && now >= p->next_history) {
                        q.generation = g; q.command.type = CommandType::RefreshHistory; p->execute(q);
                    } else if (now >= p->next_sessions) {
                        q.generation = g; q.command.type = CommandType::RefreshSessions; q.command.offset = p->working->session_offset; p->execute(q);
                    }
                }
            }
            hs_secure_zero(&q, sizeof(q)); vTaskDelay(pdMS_TO_TICKS(40));
        }
        p->wipe_response(); p->invalidate_auth();
        p->clear_pair_secret(); hs_secure_zero(&p->device_record, sizeof(p->device_record));
        if (p->device_ticket) hs_device_store_cancel(p->device_ticket);
        p->release_buffers();
        bool release;
        portENTER_CRITICAL(&s_control_mux); p->worker_done = true; release = p->released;
        portEXIT_CRITICAL(&s_control_mux);
        // No further access to p after this point if ownership stayed with UI.
        if (release) free_impl(p);
        vTaskDeleteWithCaps(nullptr);
    }
};

Client *Client::create() {
    (void)hs_auth_store_init(); // creates internal-stack NVS worker; caller never accesses flash
    const esp_err_t device_init = hs_device_store_init(); // same worker; separate from remember-cookie
    void *memory = heap_caps_calloc(1, sizeof(Impl), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) return nullptr;
    auto *p = new(memory) Impl;
    if (device_init != ESP_OK) {
        p->device_restore_due = false;
        p->pair_status(PairState::Error, "设备存储任务不可用，可使用密码登录");
    }
    p->working = allocSnapshot(); p->published = allocSnapshot();
    p->publish_lock = xSemaphoreCreateMutex();
    void *client_memory = heap_caps_malloc(sizeof(Client), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p->working || !p->published || !p->publish_lock || !client_memory) {
        heap_caps_free(client_memory); Impl::free_impl(p); return nullptr;
    }
    auto *client = new(client_memory) Client(p); p->set_status("请登录星野控制台");
    memcpy(p->published, p->working, sizeof(Snapshot));
    // ESP-IDF DEBUG HTTP header traces include Set-Cookie. Keep that library's
    // normal log level below DEBUG while this credential-bearing client exists.
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    if (xTaskCreateWithCaps(Impl::worker_task, "hosino_http", 12288, p, 3, &p->worker,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        client->~Client(); heap_caps_free(client); Impl::free_impl(p); return nullptr;
    }
    return client;
}
Snapshot *Client::allocSnapshot() { return static_cast<Snapshot *>(heap_caps_calloc(1, sizeof(Snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); }
void Client::freeSnapshot(Snapshot *p) { if (p) { hs_secure_zero(p, sizeof(*p)); heap_caps_free(p); } }
esp_err_t Client::submit(const Command &command) {
    if (!impl_ || static_cast<unsigned>(command.type) > static_cast<unsigned>(CommandType::ForgetDevice) || strnlen(command.text, sizeof(command.text)) == sizeof(command.text) ||
        strnlen(command.session_key, sizeof(command.session_key)) == sizeof(command.session_key) ||
        strnlen(command.label, sizeof(command.label)) == sizeof(command.label) ||
        strnlen(command.channel, sizeof(command.channel)) == sizeof(command.channel)) return ESP_ERR_INVALID_ARG;
    if ((command.type == CommandType::Login || command.type == CommandType::Send || command.type == CommandType::SendQQ || command.type == CommandType::SearchMemory) && !command.text[0]) return ESP_ERR_INVALID_ARG;
    if ((command.type == CommandType::SendQQ || command.type == CommandType::ResolveUncertain || command.type == CommandType::ForgetDevice) && !command.confirm) return ESP_ERR_INVALID_ARG;
    if (command.type == CommandType::Login && strlen(command.text) > 256) return ESP_ERR_INVALID_SIZE;
    if (command.type == CommandType::CreateSession && strlen(command.text) > 120) return ESP_ERR_INVALID_SIZE;
    if ((command.type == CommandType::SelectSession || command.type == CommandType::Send || command.type == CommandType::Abort ||
         command.type == CommandType::TranscribeMic || command.type == CommandType::SendQQ || command.type == CommandType::ResolveUncertain) && !command.session_key[0]) return ESP_ERR_INVALID_ARG;
    Impl *p = impl_; esp_err_t result = ESP_OK;
    hs_auth_store_status_t auth{}; hs_auth_store_status(&auth);
    uint32_t cancel_restore = 0; bool forget_saved = false;
    portENTER_CRITICAL(&s_control_mux);
    const bool pending_post = p->post_pending_locked();
    // User operations supersede read-only work, but never an admitted/queued
    // write. The sole worker still owns HTTP cleanup before the new request.
    const bool local_device = command.type == CommandType::PairCancel || command.type == CommandType::ForgetDevice;
    const bool cancel_pair = command.type == CommandType::PairCancel && p->pair_running;
    const bool preempt_read = (explicit_post(command.type) || command.type == CommandType::ResolveUncertain || local_device) && !pending_post;
    const bool replace = command.type == CommandType::SelectSession || command.type == CommandType::Login ||
                         command.type == CommandType::Logout || preempt_read || cancel_pair;
    if (p->stopping || !p->foreground) result = ESP_ERR_INVALID_STATE;
    else if (command.type == CommandType::Logout && p->device_mode) result = ESP_ERR_NOT_SUPPORTED;
    else if ((p->count == kQueueLimit && !replace) ||
             ((explicit_post(command.type) || command.type == CommandType::ResolveUncertain || local_device) && pending_post && command.type != CommandType::Logout && !cancel_pair)) result = ESP_ERR_TIMEOUT;
    else {
        if (replace) { ++p->generation; p->wipe_queue_locked(); }
        Impl::Queued &q = p->queue[(p->head + p->count) % kQueueLimit];
        q.command = command; q.generation = p->generation; q.auth_epoch = auth.epoch;
        if (command.type == CommandType::Login || command.type == CommandType::Logout) {
            p->restore_due = false; cancel_restore = p->restore_ticket; p->restore_ticket = 0;
        }
        if (command.type == CommandType::Logout) {
            forget_saved = true;
            memcpy(q.logout_cookie, p->cookie, sizeof(q.logout_cookie)); hs_secure_zero(p->cookie, sizeof(p->cookie)); p->cookie_verified = false; p->clear_auth = true;
        } else if (command.type == CommandType::Login) {
            hs_secure_zero(p->cookie, sizeof(p->cookie)); p->cookie_verified = false; p->clear_auth = true;
        } else if (command.type == CommandType::TranscribeMic) {
            p->voice_pending = true; p->voice_running = false;
            p->voice_finish = false; p->voice_cancel = false; p->voice_generation = q.generation;
            q.voice_serial = ++p->voice_serial;
        }
        ++p->count;
    }
    portEXIT_CRITICAL(&s_control_mux);
    hs_auth_store_cancel_restore(cancel_restore);
    if (forget_saved) hs_auth_store_forget(UINT32_MAX);
    return result;
}
void Client::setRememberLogin(bool enabled) {
    Impl *p = impl_; if (!p) return;
    (void)hs_auth_store_init(); // allow retry after an earlier internal-heap shortage
    char current_cookie[65]{}; uint32_t cancel_restore = 0;
    portENTER_CRITICAL(&s_control_mux);
    const bool allowed = !p->stopping;
    if (allowed) {
        if (enabled && p->cookie_verified) memcpy(current_cookie, p->cookie, sizeof(current_cookie));
        p->restore_due = false; cancel_restore = p->restore_ticket; p->restore_ticket = 0;
    }
    portEXIT_CRITICAL(&s_control_mux);
    hs_auth_store_cancel_restore(cancel_restore);
    if (allowed && hs_auth_store_set_remember(enabled, current_cookie) == ESP_OK) {
        hs_auth_store_status_t auth{}; hs_auth_store_status(&auth);
        portENTER_CRITICAL(&s_control_mux); p->cookie_store_epoch = auth.epoch; portEXIT_CRITICAL(&s_control_mux);
    }
    hs_secure_zero(current_cookie, sizeof(current_cookie));
}
bool Client::rememberLogin() const {
    hs_auth_store_status_t auth{}; hs_auth_store_status(&auth); return auth.remember;
}
void Client::cancelPairing() {
    Impl *p = impl_; if (!p) return;
    portENTER_CRITICAL(&s_control_mux);
    if (!p->stopping && p->pair_running) {
        p->pair_cancel_requested = true; ++p->generation;
        p->wipe_queue_locked(); // cancels only the explicit unfinished pairing view's queued work
    }
    portEXIT_CRITICAL(&s_control_mux);
}
void Client::stopVoice(bool cancel) {
    Impl *p = impl_; if (!p) return;
    portENTER_CRITICAL(&s_control_mux);
    // Cancellation also fences a just-completed result not yet copied by GUI.
    if (cancel) p->voice_cancel = true;
    else if (p->voice_pending) p->voice_finish = true;
    portEXIT_CRITICAL(&s_control_mux);
}
void Client::setForeground(bool foreground) {
    if (!impl_) return;
    Impl *p = impl_;
    uint32_t cancel_restore = 0;
    portENTER_CRITICAL(&s_control_mux);
    if (!p->stopping && p->foreground != foreground) {
        p->foreground = foreground; ++p->generation;
        if (!foreground) {
            p->trim_due = true;
            if (p->pair_running) p->pair_cancel_requested = true;
            cancel_restore = p->restore_ticket; p->restore_ticket = 0;
        } else p->device_resume_due = true;
        p->wipe_queue_locked(); p->status_due = foreground;
    }
    portEXIT_CRITICAL(&s_control_mux);
    hs_auth_store_cancel_restore(cancel_restore);
}
bool Client::copySnapshot(Snapshot &out) const {
    Impl *p = impl_; if (!p) return false;
    if (xSemaphoreTake(p->publish_lock, 0) != pdTRUE) return false;
    if (!p->published) { xSemaphoreGive(p->publish_lock); return false; }
    memcpy(&out, p->published, sizeof(out));
    const uint32_t voice_generation = p->published_voice_generation;
    const uint32_t voice_serial = p->published_voice_serial;
    xSemaphoreGive(p->publish_lock);
    portENTER_CRITICAL(&s_control_mux);
    const bool clear = p->clear_auth; out.foreground = p->foreground; out.stopped = p->worker_done;
    out.busy = out.busy || p->active || p->count != 0;
    const bool stale_voice = voice_generation != p->generation || voice_serial != p->voice_serial || p->voice_cancel;
    const bool queued_voice = p->voice_pending && !p->voice_running && !p->voice_cancel;
    portEXIT_CRITICAL(&s_control_mux);
    if (stale_voice && out.voice_state != VoiceState::Idle) {
        out.voice_state = queued_voice ? VoiceState::Preparing : VoiceState::Cancelled;
        out.voice_elapsed_ms = 0;
        hs_secure_zero(out.voice_text, sizeof(out.voice_text));
        hs_secure_zero(out.voice_session_key, sizeof(out.voice_session_key));
    }
    if (clear) { clear_private(out); text(out.status, sizeof(out.status), "正在退出登录"); }
    hs_auth_store_status_t auth{}; hs_auth_store_status(&auth);
    out.remember_login = auth.remember; out.remember_busy = auth.busy;
    out.remember_saved = auth.saved; out.remember_error = auth.error;
    out.revision += auth.revision;
    return true;
}
void Client::shutdown() {
    if (!impl_) return;
    Impl *p = impl_;
    portENTER_CRITICAL(&s_control_mux);
    p->stopping = true; p->foreground = false; ++p->generation; p->wipe_queue_locked(); hs_secure_zero(p->cookie, sizeof(p->cookie));
    const uint32_t ticket = p->restore_ticket; p->restore_ticket = 0; p->restore_due = false;
    portEXIT_CRITICAL(&s_control_mux);
    hs_auth_store_cancel_restore(ticket);
}
bool Client::isStopped() const {
    if (!impl_) return true;
    portENTER_CRITICAL(&s_control_mux); const bool done = impl_->worker_done; portEXIT_CRITICAL(&s_control_mux); return done;
}
bool Client::destroy() {
    if (!impl_) return false;
    Impl *p = impl_;
    portENTER_CRITICAL(&s_control_mux); const bool done = p->worker_done;
    if (done) p->released = true;
    portEXIT_CRITICAL(&s_control_mux);
    if (!done) return false;
    impl_ = nullptr; Impl::free_impl(p); this->~Client(); heap_caps_free(this); return true;
}
void Client::release() {
    Impl *p = impl_; if (!p) return;
    portENTER_CRITICAL(&s_control_mux);
    p->released = true; p->stopping = true; p->foreground = false; ++p->generation;
    p->wipe_queue_locked(); hs_secure_zero(p->cookie, sizeof(p->cookie)); const bool done = p->worker_done;
    const uint32_t ticket = p->restore_ticket; p->restore_ticket = 0; p->restore_due = false;
    portEXIT_CRITICAL(&s_control_mux);
    hs_auth_store_cancel_restore(ticket);
    impl_ = nullptr; this->~Client(); heap_caps_free(this);
    if (done) Impl::free_impl(p);
}

} // namespace hosino_console
