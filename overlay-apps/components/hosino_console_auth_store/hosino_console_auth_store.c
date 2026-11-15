// SPDX-License-Identifier: Apache-2.0
#include "hosino_console_auth_store.h"
#include "hosino_console_device_store.h"
#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "nvs.h"

/* One versioned blob keeps preference and token in the same NVS update.
 * No password, URL, account name, or session content is stored. */
typedef struct { uint8_t version, remember; char cookie[64]; } record_t;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t worker;
static bool starting, ready, remember, saved, write_pending, write_active, read_pending, read_active, reply_ready;
static esp_err_t last_error;
static uint32_t epoch = 1, revision, write_seq, read_seq, read_ticket, read_epoch, reply_ticket, reply_epoch;
static esp_err_t reply_error;
static record_t mailbox;
static char reply[65];
static void device_boot_store(void);
static bool device_process_one(void);

static void wipe(void *p, size_t n) { volatile unsigned char *s = p; while (n--) *s++ = 0; }
static bool valid_cookie(const char *s) {
    if (!s) return false;
    for (unsigned i = 0; i < 64; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return s[64] == 0;
}
static bool record_has_cookie(const record_t *r) {
    char value[65] = {0}; memcpy(value, r->cookie, 64);
    const bool valid = valid_cookie(value); wipe(value, sizeof(value)); return valid;
}
static esp_err_t read_record(record_t *r) {
    memset(r, 0, sizeof(*r)); r->version = 1;
    nvs_handle_t handle;
    esp_err_t error = nvs_open("hs_auth", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;
    size_t size = sizeof(*r);
    error = nvs_get_blob(handle, "session", r, &size); nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) { memset(r, 0, sizeof(*r)); r->version = 1; return ESP_OK; }
    if (error != ESP_OK) return error;
    bool empty = true; for (unsigned i = 0; i < 64; ++i) empty = empty && r->cookie[i] == 0;
    if (size != sizeof(*r) || r->version != 1 || r->remember > 1 ||
        (!empty && (!r->remember || !record_has_cookie(r)))) return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}
static esp_err_t write_record(const record_t *r) {
    nvs_handle_t handle;
    esp_err_t error = nvs_open("hs_auth", NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    if (!r->remember) {
        error = nvs_erase_key(handle, "session");
        if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    } else error = nvs_set_blob(handle, "session", r, sizeof(*r));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle); return error;
}
/* Called only under mux. A newer desired record supersedes an unsent save.
 * In-flight writes finish on this same worker before the newer clear is applied. */
static void schedule_write(const char *cookie) {
    wipe(&mailbox, sizeof(mailbox)); mailbox.version = 1; mailbox.remember = remember;
    if (remember && cookie) memcpy(mailbox.cookie, cookie, 64);
    write_pending = true; ++write_seq; ++revision; last_error = ESP_OK;
}
static void boot_store(void) {
    record_t r; const esp_err_t error = read_record(&r);
    portENTER_CRITICAL(&mux);
    /* A user action during startup always wins over disk state. */
    if (epoch == 1 && write_seq == 0) {
        remember = error == ESP_OK && r.remember;
        saved = error == ESP_OK && record_has_cookie(&r);
        last_error = error;
    }
    ready = true; ++revision;
    portEXIT_CRITICAL(&mux); wipe(&r, sizeof(r));
}
static bool process_one(void) {
    record_t r; memset(&r, 0, sizeof(r));
    uint32_t seq = 0, ticket = 0, requested_epoch = 0; bool do_write = false, do_read = false, allowed = false;
    portENTER_CRITICAL(&mux);
    if (write_pending) {
        do_write = true; seq = write_seq; r = mailbox; wipe(&mailbox, sizeof(mailbox));
        write_pending = false; write_active = true;
    } else if (read_pending) {
        do_read = true; ticket = read_ticket; requested_epoch = read_epoch; allowed = remember;
        read_pending = false; read_active = true;
    }
    portEXIT_CRITICAL(&mux);
    if (!do_write && !do_read) return false;
    const esp_err_t error = do_write ? write_record(&r) : allowed ? read_record(&r) : ESP_OK;
    portENTER_CRITICAL(&mux);
    if (do_write) {
        write_active = false;
        if (seq == write_seq) { last_error = error; if (error == ESP_OK) saved = r.remember && record_has_cookie(&r); }
    } else {
        read_active = false;
        if (ticket == read_ticket) {
            wipe(reply, sizeof(reply)); reply_ready = true; reply_ticket = ticket; reply_epoch = requested_epoch;
            reply_error = requested_epoch == epoch ? error : ESP_ERR_INVALID_STATE;
            if (reply_error == ESP_OK && allowed && remember && r.remember && record_has_cookie(&r)) memcpy(reply, r.cookie, 64);
            if (error != ESP_OK) last_error = error;
        }
    }
    ++revision; portEXIT_CRITICAL(&mux); wipe(&r, sizeof(r)); return true;
}
static void task(void *arg) {
    (void)arg; boot_store(); device_boot_store();
    for (;;) {
        const bool cookie_work = process_one();
        const bool device_work = device_process_one();
        if (!cookie_work && !device_work) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}
esp_err_t hs_auth_store_init(void) {
    portENTER_CRITICAL(&mux);
    if (worker || starting) { portEXIT_CRITICAL(&mux); return ESP_OK; }
    starting = true; ready = false; portEXIT_CRITICAL(&mux);
    TaskHandle_t created = NULL;
    const BaseType_t ok = xTaskCreateWithCaps(task, "hs_auth_nvs", 4096, NULL, 2, &created, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    portENTER_CRITICAL(&mux); starting = false;
    if (ok == pdPASS) worker = created;
    else { ready = true; last_error = ESP_ERR_NO_MEM; ++revision; }
    portEXIT_CRITICAL(&mux); return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
void hs_auth_store_status(hs_auth_store_status_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&mux);
    *out = (hs_auth_store_status_t){ready, remember, !ready || write_pending || write_active || read_pending || read_active,
        saved, last_error, epoch, revision};
    portEXIT_CRITICAL(&mux);
}
esp_err_t hs_auth_store_set_remember(bool enabled, const char *cookie) {
    if (cookie && *cookie && !valid_cookie(cookie)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&mux);
    if (!worker) { portEXIT_CRITICAL(&mux); return ESP_ERR_INVALID_STATE; }
    ++epoch; remember = enabled; schedule_write(enabled && cookie && *cookie ? cookie : NULL);
    wipe(reply, sizeof(reply)); if (reply_ready) reply_error = ESP_ERR_INVALID_STATE;
    portEXIT_CRITICAL(&mux); xTaskNotifyGive(worker); return ESP_OK;
}
esp_err_t hs_auth_store_save(uint32_t expected_epoch, const char *cookie) {
    if (!valid_cookie(cookie)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&mux);
    if (!worker || !remember || expected_epoch != epoch) { portEXIT_CRITICAL(&mux); return ESP_ERR_INVALID_STATE; }
    schedule_write(cookie); portEXIT_CRITICAL(&mux); xTaskNotifyGive(worker); return ESP_OK;
}
void hs_auth_store_forget(uint32_t expected_epoch) {
    portENTER_CRITICAL(&mux);
    if (!worker || (expected_epoch != UINT32_MAX && expected_epoch != epoch)) { portEXIT_CRITICAL(&mux); return; }
    ++epoch; schedule_write(NULL); wipe(reply, sizeof(reply)); if (reply_ready) reply_error = ESP_ERR_INVALID_STATE;
    portEXIT_CRITICAL(&mux); xTaskNotifyGive(worker);
}
uint32_t hs_auth_store_restore(void) {
    portENTER_CRITICAL(&mux);
    if (!worker || !ready) { portEXIT_CRITICAL(&mux); return 0; }
    if (++read_seq == 0) ++read_seq;
    read_ticket = read_seq; read_epoch = epoch; read_pending = true;
    wipe(reply, sizeof(reply)); reply_ready = false;
    const uint32_t result = read_ticket;
    portEXIT_CRITICAL(&mux); xTaskNotifyGive(worker); return result;
}
bool hs_auth_store_take_restore(uint32_t ticket, char cookie[65], uint32_t *out_epoch, esp_err_t *error) {
    if (!ticket || !cookie || !out_epoch || !error) return false;
    portENTER_CRITICAL(&mux);
    if (!reply_ready || reply_ticket != ticket) { portEXIT_CRITICAL(&mux); return false; }
    *error = reply_epoch == epoch ? reply_error : ESP_ERR_INVALID_STATE;
    *out_epoch = reply_epoch; memset(cookie, 0, 65);
    if (*error == ESP_OK && remember) memcpy(cookie, reply, 65);
    wipe(reply, sizeof(reply)); reply_ready = false; read_ticket = 0;
    portEXIT_CRITICAL(&mux); return true;
}
void hs_auth_store_cancel_restore(uint32_t ticket) {
    if (!ticket) return;
    portENTER_CRITICAL(&mux);
    if (read_ticket == ticket) { read_ticket = 0; read_pending = false; }
    if (reply_ticket == ticket) { wipe(reply, sizeof(reply)); reply_ready = false; }
    portEXIT_CRITICAL(&mux);
}
#ifdef HOSINO_AUTH_STORE_HOST_TEST
void hs_auth_store_test_boot(void) { boot_store(); }
bool hs_auth_store_test_step(void) { return process_one(); }
#endif

/* Device pairing persistence is deliberately separate from the cookie record.
 * It shares only the mutex, wakeup and internal-stack worker. */
typedef enum { DEVICE_NONE, DEVICE_LOAD, DEVICE_SAVE, DEVICE_FORGET } device_op_t;
enum { DEVICE_BLOB_SIZE = 4 + 1 + HS_DEVICE_TOKEN_CAP + HS_DEVICE_PAIR_ID_CAP };
static bool device_ready, device_saved, device_saved_known, device_pending, device_active, device_reply_ready;
static esp_err_t device_error, device_reply_error;
static uint32_t device_epoch = 1, device_revision, device_sequence, device_ticket, device_requested_epoch;
static uint32_t device_reply_ticket, device_reply_epoch;
static device_op_t device_op;
static hs_device_store_record_t device_mailbox, device_reply;

static void device_empty(hs_device_store_record_t *r) { wipe(r, sizeof(*r)); r->schema = HS_DEVICE_STORE_SCHEMA; }
static uint32_t device_next(uint32_t n) { ++n; if (!n || n == UINT32_MAX) n = 1; return n; }
static bool device_b64url(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}
static bool device_valid(const hs_device_store_record_t *r) {
    if (!r || r->schema != HS_DEVICE_STORE_SCHEMA || memcmp(r->token, "hsd_", 4) ||
        r->token[16] != '.' || r->token[60] != 0 || memcmp(r->pair_id, "hsp_", 4) || r->pair_id[26] != 0) return false;
    for (unsigned i = 4; i < 16; ++i) {
        unsigned char c = (unsigned char)r->token[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    for (unsigned i = 17; i < 60; ++i) if (!device_b64url((unsigned char)r->token[i])) return false;
    for (unsigned i = 4; i < 26; ++i) if (!device_b64url((unsigned char)r->pair_id[i])) return false;
    return true;
}
static void device_copy(hs_device_store_record_t *to, const hs_device_store_record_t *from) {
    device_empty(to); to->pending_ack = from->pending_ack;
    memcpy(to->token, from->token, 60); memcpy(to->pair_id, from->pair_id, 26);
}
static esp_err_t device_read_record(hs_device_store_record_t *r) {
    uint8_t blob[DEVICE_BLOB_SIZE] = {0}; device_empty(r);
    nvs_handle_t handle;
    esp_err_t error = nvs_open("hs_auth", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;
    size_t size = sizeof(blob);
    error = nvs_get_blob(handle, "device_v1", blob, &size); nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) { wipe(blob, sizeof(blob)); return ESP_OK; }
    if (error == ESP_ERR_NVS_INVALID_LENGTH) error = ESP_ERR_INVALID_RESPONSE;
    if (error == ESP_OK) {
        if (size != sizeof(blob) || blob[0] != HS_DEVICE_STORE_SCHEMA || blob[1] || blob[2] || blob[3] || blob[4] > 1) {
            error = ESP_ERR_INVALID_RESPONSE;
        } else {
            r->pending_ack = blob[4] != 0;
            memcpy(r->token, blob + 5, HS_DEVICE_TOKEN_CAP);
            memcpy(r->pair_id, blob + 5 + HS_DEVICE_TOKEN_CAP, HS_DEVICE_PAIR_ID_CAP);
            if (!device_valid(r)) error = ESP_ERR_INVALID_RESPONSE;
            for (unsigned i = 61; i < HS_DEVICE_TOKEN_CAP; ++i) if (r->token[i]) error = ESP_ERR_INVALID_RESPONSE;
            for (unsigned i = 27; i < HS_DEVICE_PAIR_ID_CAP; ++i) if (r->pair_id[i]) error = ESP_ERR_INVALID_RESPONSE;
        }
    }
    wipe(blob, sizeof(blob));
    if (error != ESP_OK) device_empty(r);
    return error;
}
static esp_err_t device_write_record(device_op_t op, const hs_device_store_record_t *r) {
    uint8_t blob[DEVICE_BLOB_SIZE] = {0};
    nvs_handle_t handle;
    esp_err_t error = nvs_open("hs_auth", NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    if (op == DEVICE_FORGET) {
        error = nvs_erase_key(handle, "device_v1");
        if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    } else {
        blob[0] = HS_DEVICE_STORE_SCHEMA; blob[4] = r->pending_ack ? 1 : 0;
        memcpy(blob + 5, r->token, 60);
        memcpy(blob + 5 + HS_DEVICE_TOKEN_CAP, r->pair_id, 26);
        error = nvs_set_blob(handle, "device_v1", blob, sizeof(blob));
    }
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle); wipe(blob, sizeof(blob)); return error;
}
static void device_boot_store(void) {
    hs_device_store_record_t r; const esp_err_t error = device_read_record(&r);
    portENTER_CRITICAL(&mux);
    device_saved_known = error == ESP_OK;
    if (device_saved_known) device_saved = r.token[0] != 0;
    if (device_epoch == 1 && !device_pending) device_error = error;
    device_ready = true; ++device_revision;
    portEXIT_CRITICAL(&mux); wipe(&r, sizeof(r));
}
static bool device_process_one(void) {
    hs_device_store_record_t r, observed; device_empty(&r); device_empty(&observed);
    uint32_t ticket = 0, requested_epoch = 0; device_op_t op = DEVICE_NONE;
    portENTER_CRITICAL(&mux);
    if (device_pending) {
        op = device_op; ticket = device_ticket; requested_epoch = device_requested_epoch;
        if (op == DEVICE_SAVE) r = device_mailbox;
        wipe(&device_mailbox, sizeof(device_mailbox));
        device_pending = false; device_active = true;
    }
    portEXIT_CRITICAL(&mux);
    if (op == DEVICE_NONE) return false;
    esp_err_t error = op == DEVICE_LOAD ? device_read_record(&r) : device_write_record(op, &r);
    /* Commit failure is not proof that flash retained the old value. Read back
     * through the worker to report presence honestly, but never ACK a failed save. */
    bool known = error == ESP_OK, present = known && op != DEVICE_FORGET && r.token[0];
    if (!known) {
        known = device_read_record(&observed) == ESP_OK;
        if (known) present = observed.token[0] != 0;
    }
    portENTER_CRITICAL(&mux);
    device_active = false; device_saved_known = known;
    if (known) device_saved = present;
    if (ticket == device_ticket && requested_epoch == device_epoch) {
        device_error = error;
        device_empty(&device_reply); device_reply_ready = true;
        device_reply_ticket = ticket; device_reply_epoch = requested_epoch; device_reply_error = error;
        if (op == DEVICE_LOAD && error == ESP_OK && r.token[0]) device_copy(&device_reply, &r);
    }
    ++device_revision;
    portEXIT_CRITICAL(&mux); wipe(&r, sizeof(r)); wipe(&observed, sizeof(observed)); return true;
}
esp_err_t hs_device_store_init(void) {
    const esp_err_t error = hs_auth_store_init();
    if (error != ESP_OK) {
        portENTER_CRITICAL(&mux); device_error = error; ++device_revision; portEXIT_CRITICAL(&mux);
    }
    return error;
}
void hs_device_store_status(hs_device_store_status_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&mux);
    *out = (hs_device_store_status_t){device_ready, !device_ready || device_pending || device_active,
        device_saved, device_saved_known, device_error, device_epoch, device_revision};
    portEXIT_CRITICAL(&mux);
}
static esp_err_t device_schedule(device_op_t op, uint32_t expected_epoch,
                                 const hs_device_store_record_t *record, uint32_t *out_ticket) {
    if (!out_ticket) return ESP_ERR_INVALID_ARG;
    *out_ticket = 0;
    if (op == DEVICE_SAVE && !device_valid(record)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&mux);
    const bool forget = op == DEVICE_FORGET;
    if (!worker || (!device_ready && !forget) ||
        (expected_epoch != device_epoch && !(forget && expected_epoch == UINT32_MAX)) ||
        (!forget && (device_pending || device_active))) {
        portEXIT_CRITICAL(&mux); return ESP_ERR_INVALID_STATE;
    }
    if (forget) device_epoch = device_next(device_epoch);
    device_sequence = device_next(device_sequence); device_ticket = device_sequence;
    device_requested_epoch = device_epoch; device_op = op; device_pending = true;
    device_empty(&device_mailbox);
    if (op == DEVICE_SAVE) device_copy(&device_mailbox, record);
    wipe(&device_reply, sizeof(device_reply)); device_reply_ready = false;
    device_error = ESP_OK; ++device_revision; *out_ticket = device_ticket;
    portEXIT_CRITICAL(&mux); xTaskNotifyGive(worker); return ESP_OK;
}
esp_err_t hs_device_store_load(uint32_t expected_epoch, uint32_t *ticket) {
    return device_schedule(DEVICE_LOAD, expected_epoch, NULL, ticket);
}
esp_err_t hs_device_store_save(uint32_t expected_epoch, const hs_device_store_record_t *record, uint32_t *ticket) {
    return device_schedule(DEVICE_SAVE, expected_epoch, record, ticket);
}
esp_err_t hs_device_store_forget(uint32_t expected_epoch, uint32_t *ticket) {
    return device_schedule(DEVICE_FORGET, expected_epoch, NULL, ticket);
}
bool hs_device_store_take(uint32_t ticket, hs_device_store_record_t *out, uint32_t *out_epoch, esp_err_t *error) {
    if (!ticket || !out || !out_epoch || !error) return false;
    portENTER_CRITICAL(&mux);
    if (!device_reply_ready || device_reply_ticket != ticket) { portEXIT_CRITICAL(&mux); return false; }
    device_empty(out); *out_epoch = device_reply_epoch;
    *error = device_reply_epoch == device_epoch ? device_reply_error : ESP_ERR_INVALID_STATE;
    if (*error == ESP_OK && device_reply.token[0]) device_copy(out, &device_reply);
    wipe(&device_reply, sizeof(device_reply)); device_reply_ready = false; device_ticket = 0;
    portEXIT_CRITICAL(&mux); return true;
}
void hs_device_store_cancel(uint32_t ticket) {
    if (!ticket) return;
    portENTER_CRITICAL(&mux);
    if (device_ticket == ticket) {
        device_epoch = device_next(device_epoch); device_ticket = 0; device_pending = false;
        wipe(&device_mailbox, sizeof(device_mailbox)); wipe(&device_reply, sizeof(device_reply));
        device_reply_ready = false; device_error = ESP_ERR_INVALID_STATE; ++device_revision;
    }
    portEXIT_CRITICAL(&mux);
}
#ifdef HOSINO_AUTH_STORE_HOST_TEST
void hs_device_store_test_boot(void) { device_boot_store(); }
bool hs_device_store_test_step(void) { return device_process_one(); }
#endif
