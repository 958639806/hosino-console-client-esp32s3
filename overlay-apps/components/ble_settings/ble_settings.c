/* SPDX-License-Identifier: Apache-2.0
 * NimBLE lifecycle follows ESP-IDF 5.5.5's examples/bluetooth/nimble/blecent.
 * All public operations are asynchronous; no callback touches an LVGL object.
 */
#include "ble_settings.h"

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nvs_flash.h"

static const char *TAG = "ble_settings";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ble_settings_snapshot_t s_snapshot = {.state = BLE_SETTINGS_OFF, .revision = 1};
static TaskHandle_t s_worker;
static bool s_worker_starting;
static bool s_ready;
static bool s_scan_requested;
static bool s_cancel_requested;
static bool s_accept_reports;
static int64_t s_sync_deadline;
static int64_t s_stop_deadline;
static bool s_stop_timed_out;
static StaticSemaphore_t s_host_exited_storage;
static SemaphoreHandle_t s_host_exited;

static void wake_worker(void)
{
    portENTER_CRITICAL(&s_lock);
    TaskHandle_t worker = s_worker;
    portEXIT_CRITICAL(&s_lock);
    if (worker != NULL) {
        xTaskNotifyGive(worker);
    }
}

static void set_error(int error, bool restart_required)
{
    ESP_LOGE(TAG, "BLE operation failed: code=%d, restart_required=%d", error, restart_required);
    portENTER_CRITICAL(&s_lock);
    s_snapshot.last_error = error;
    /* Only confirmed successful shutdown may clear an uncertain lifetime. */
    s_snapshot.restart_required = s_snapshot.restart_required || restart_required;
    s_snapshot.state = BLE_SETTINGS_ERROR;
    s_snapshot.requested_enabled = false;
    s_scan_requested = false;
    s_accept_reports = false;
    ++s_snapshot.revision;
    portEXIT_CRITICAL(&s_lock);
}

/* Advertisements are untrusted length-delimited UTF-8. Preserve complete valid
 * codepoints; replace controls/invalid bytes and never leave a partial suffix. */
static void copy_name(char *out, const uint8_t *bytes, size_t length)
{
    size_t used = 0;
    for (size_t i = 0; i < length && used + 1 < BLE_SETTINGS_NAME_BYTES;) {
        const uint8_t first = bytes[i];
        size_t n = first < 0x80 ? 1 : (first >= 0xc2 && first <= 0xdf ? 2 :
                   (first >= 0xe0 && first <= 0xef ? 3 :
                   (first >= 0xf0 && first <= 0xf4 ? 4 : 0)));
        bool valid = n != 0 && i + n <= length;
        for (size_t j = 1; valid && j < n; ++j) {
            valid = (bytes[i + j] & 0xc0) == 0x80;
        }
        if (valid && n >= 3) {
            valid = !(first == 0xe0 && bytes[i + 1] < 0xa0) &&
                    !(first == 0xed && bytes[i + 1] >= 0xa0) &&
                    !(first == 0xf0 && bytes[i + 1] < 0x90) &&
                    !(first == 0xf4 && bytes[i + 1] >= 0x90);
        }
        if (!valid || first < 0x20 || first == 0x7f) {
            out[used++] = '?';
            ++i;
        } else {
            if (used + n >= BLE_SETTINGS_NAME_BYTES) break;
            memcpy(out + used, bytes + i, n);
            used += n;
            i += n;
        }
    }
    out[used] = '\0';
}

static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    if (event->type == BLE_GAP_EVENT_DISC) {
        struct ble_hs_adv_fields fields = {0};
        char name[BLE_SETTINGS_NAME_BYTES] = {0};
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) == 0 &&
            fields.name != NULL && fields.name_len > 0) {
            copy_name(name, fields.name, fields.name_len);
        }
        portENTER_CRITICAL(&s_lock);
        if (s_accept_reports) {
            unsigned index = 0;
            for (; index < s_snapshot.count; ++index) {
                if (s_snapshot.devices[index].address_type == event->disc.addr.type &&
                    memcmp(s_snapshot.devices[index].address, event->disc.addr.val, 6) == 0) break;
            }
            if (index < BLE_SETTINGS_MAX_DEVICES) {
                ble_settings_device_t *device = &s_snapshot.devices[index];
                if (index == s_snapshot.count) {
                    ++s_snapshot.count;
                    memcpy(device->address, event->disc.addr.val, 6);
                    device->address_type = event->disc.addr.type;
                }
                device->rssi = event->disc.rssi;
                if (name[0] != '\0') memcpy(device->name, name, sizeof(device->name));
                ++s_snapshot.revision;
            }
        }
        portEXIT_CRITICAL(&s_lock);
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        portENTER_CRITICAL(&s_lock);
        s_snapshot.scanning = false;
        s_accept_reports = false;
        if (s_ready && s_snapshot.requested_enabled) s_snapshot.state = BLE_SETTINGS_READY;
        ++s_snapshot.revision;
        portEXIT_CRITICAL(&s_lock);
    }
    return 0;
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset: %d", reason);
    portENTER_CRITICAL(&s_lock);
    s_ready = false;
    s_accept_reports = false;
    s_snapshot.scanning = false;
    s_snapshot.enabled = false;
    if (s_stop_deadline == 0 && !s_snapshot.restart_required) {
        s_snapshot.last_error = reason;
        s_snapshot.state = BLE_SETTINGS_STARTING;
        s_sync_deadline = esp_timer_get_time() + 8000000;
    }
    ++s_snapshot.revision;
    portEXIT_CRITICAL(&s_lock);
}

static void on_sync(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool stopping = s_stop_deadline != 0 || s_snapshot.restart_required ||
                          !s_snapshot.requested_enabled;
    portEXIT_CRITICAL(&s_lock);
    if (stopping) return;
    const int result = ble_hs_util_ensure_addr(0);
    if (result != 0) {
        set_error(result, false);
        return;
    }
    portENTER_CRITICAL(&s_lock);
    /* The worker/observer may have entered shutdown during ensure_addr(). */
    if (s_stop_deadline != 0 || s_snapshot.restart_required || !s_snapshot.requested_enabled) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    s_ready = true;
    s_snapshot.enabled = true;
    s_snapshot.last_error = 0;
    s_snapshot.state = BLE_SETTINGS_READY;
    ++s_snapshot.revision;
    portEXIT_CRITICAL(&s_lock);
    wake_worker();
}

static void host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    /* After this signal the task will never touch NimBLE state again. The
     * worker also requires successful nimble_port_stop before deinitializing. */
    xSemaphoreGive(s_host_exited);
    vTaskDelete(NULL);
}

static void worker_task(void *argument)
{
    (void)argument;
    bool initialized = false;
    bool host_created = false;
    s_host_exited = xSemaphoreCreateBinaryStatic(&s_host_exited_storage);

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        portENTER_CRITICAL(&s_lock);
        const bool desired = s_snapshot.requested_enabled;
        const bool ready = s_ready;
        const bool fault = s_snapshot.restart_required;
        const bool timed_out = initialized && !ready && esp_timer_get_time() >= s_sync_deadline;
        portEXIT_CRITICAL(&s_lock);
        if (fault) continue; /* Never free uncertain live host/controller state. */

        if (!initialized && desired) {
            portENTER_CRITICAL(&s_lock);
            s_snapshot.state = BLE_SETTINGS_STARTING;
            s_sync_deadline = esp_timer_get_time() + 8000000;
            ++s_snapshot.revision;
            portEXIT_CRITICAL(&s_lock);
            esp_err_t result = nvs_flash_init(); /* Never erase existing NVS. */
            if (result != ESP_OK) {
                set_error(result, false);
                continue;
            }
            result = nimble_port_init();
            if (result != ESP_OK) {
                /* IDF rolls back the controller but may leave earlier host/NPL
                 * allocations after partial initialization. Do not blindly
                 * retry init or call deinit on an incompletely initialized host. */
                set_error(result, true);
                continue;
            }
            initialized = true;
            ble_hs_cfg.reset_cb = on_reset;
            ble_hs_cfg.sync_cb = on_sync;
            (void)xSemaphoreTake(s_host_exited, 0);
            if (xTaskCreatePinnedToCore(host_task, "hosino_ble_host",
                    CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE, NULL, configMAX_PRIORITIES - 4,
                    NULL, NIMBLE_CORE) != pdPASS) {
                const esp_err_t cleanup = nimble_port_deinit();
                initialized = cleanup != ESP_OK;
                set_error(cleanup != ESP_OK ? cleanup : ESP_ERR_NO_MEM, initialized);
                continue;
            }
            host_created = true;
            continue;
        }

        if (initialized && (!desired || timed_out)) {
            if (timed_out) set_error(ESP_ERR_TIMEOUT, false);
            portENTER_CRITICAL(&s_lock);
            const int shutdown_error = s_snapshot.last_error;
            s_snapshot.state = BLE_SETTINGS_STOPPING;
            s_stop_deadline = esp_timer_get_time() + 5000000;
            s_stop_timed_out = false;
            s_scan_requested = false;
            s_accept_reports = false;
            ++s_snapshot.revision;
            portEXIT_CRITICAL(&s_lock);
            int result = 0;
            if (host_created) {
                /* This IDF API can wait forever internally. get_snapshot()
                 * observes the overall deadline independently; it never kills
                 * this worker/host or frees their still-live resources. */
                result = nimble_port_stop();
                if (result == 0 && xSemaphoreTake(s_host_exited, pdMS_TO_TICKS(2000)) != pdTRUE) {
                    result = ESP_ERR_TIMEOUT;
                }
                if (result != 0) {
                    set_error(result, true);
                    continue;
                }
                host_created = false;
            }
            result = nimble_port_deinit();
            if (result != 0) {
                set_error(result, true);
                continue;
            }
            initialized = false;
            portENTER_CRITICAL(&s_lock);
            s_ready = false;
            s_snapshot.enabled = false;
            s_snapshot.scanning = false;
            s_cancel_requested = false;
            /* Stop and deinit both actually succeeded. An observer timeout is
             * now resolved, even if the blocking operation finished late. */
            if (s_stop_timed_out) s_snapshot.last_error = shutdown_error;
            s_snapshot.restart_required = false;
            s_stop_deadline = 0;
            s_stop_timed_out = false;
            s_snapshot.state = s_snapshot.last_error != 0 ? BLE_SETTINGS_ERROR : BLE_SETTINGS_OFF;
            ++s_snapshot.revision;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "BLE host/controller stopped and released");
            continue;
        }

        if (!initialized || !ready) continue;
        portENTER_CRITICAL(&s_lock);
        const bool cancel = s_cancel_requested;
        s_cancel_requested = false;
        portEXIT_CRITICAL(&s_lock);
        if (cancel) {
            const int result = ble_gap_disc_cancel();
            if (result != 0 && result != BLE_HS_EALREADY) {
                set_error(result, false);
                continue;
            }
            portENTER_CRITICAL(&s_lock);
            s_snapshot.scanning = false;
            s_snapshot.state = BLE_SETTINGS_READY;
            ++s_snapshot.revision;
            portEXIT_CRITICAL(&s_lock);
        }

        portENTER_CRITICAL(&s_lock);
        const bool scan = s_scan_requested && s_snapshot.requested_enabled &&
                          !s_snapshot.scanning && !s_cancel_requested;
        if (scan) {
            s_scan_requested = false;
            s_accept_reports = true;
            s_snapshot.scanning = true;
            s_snapshot.state = BLE_SETTINGS_SCANNING;
            s_snapshot.count = 0;
            memset(s_snapshot.devices, 0, sizeof(s_snapshot.devices));
            ++s_snapshot.revision;
        }
        portEXIT_CRITICAL(&s_lock);
        if (scan) {
            uint8_t address_type = 0;
            int result = ble_hs_id_infer_auto(0, &address_type);
            const struct ble_gap_disc_params params = {
                .itvl = 160, .window = 48, .filter_policy = 0,
                .limited = 0, .passive = 0, .filter_duplicates = 1,
            };
            if (result == 0) result = ble_gap_disc(address_type, BLE_SETTINGS_SCAN_MS,
                                                   &params, gap_event, NULL);
            if (result != 0) set_error(result, false);
        }
    }
}

static esp_err_t ensure_worker(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_worker != NULL) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    if (s_worker_starting) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_worker_starting = true;
    portEXIT_CRITICAL(&s_lock);
    TaskHandle_t worker = NULL;
    const BaseType_t result = xTaskCreate(worker_task, "ble_settings", 4096, NULL, 3, &worker);
    portENTER_CRITICAL(&s_lock);
    s_worker = worker;
    s_worker_starting = false;
    portEXIT_CRITICAL(&s_lock);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ble_settings_set_enabled(bool enabled)
{
    if (enabled) {
        const esp_err_t result = ensure_worker();
        if (result != ESP_OK) {
            set_error(result, false);
            return result;
        }
    }
    portENTER_CRITICAL(&s_lock);
    if (s_snapshot.restart_required) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_snapshot.requested_enabled = enabled;
    s_snapshot.last_error = 0;
    s_scan_requested = false;
    s_accept_reports = false;
    s_cancel_requested = true;
    if (enabled && !s_snapshot.enabled) s_snapshot.state = BLE_SETTINGS_STARTING;
    if (!enabled && s_snapshot.enabled) s_snapshot.state = BLE_SETTINGS_STOPPING;
    if (!enabled && s_worker == NULL) s_snapshot.state = BLE_SETTINGS_OFF;
    ++s_snapshot.revision;
    portEXIT_CRITICAL(&s_lock);
    wake_worker();
    return ESP_OK;
}

esp_err_t ble_settings_start_scan(void)
{
    portENTER_CRITICAL(&s_lock);
    if (!s_snapshot.enabled || !s_snapshot.requested_enabled ||
        s_snapshot.restart_required || s_snapshot.scanning || s_scan_requested) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_requested = true;
    portEXIT_CRITICAL(&s_lock);
    wake_worker();
    return ESP_OK;
}

void ble_settings_cancel_scan(void)
{
    portENTER_CRITICAL(&s_lock);
    s_scan_requested = false;
    s_accept_reports = false;
    s_cancel_requested = true;
    portEXIT_CRITICAL(&s_lock);
    wake_worker();
}

void ble_settings_get_snapshot(ble_settings_snapshot_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&s_lock);
    if (s_stop_deadline != 0 && !s_snapshot.restart_required &&
        esp_timer_get_time() >= s_stop_deadline) {
        s_stop_timed_out = true;
        s_snapshot.last_error = ESP_ERR_TIMEOUT;
        s_snapshot.restart_required = true;
        s_snapshot.requested_enabled = false;
        s_snapshot.state = BLE_SETTINGS_ERROR;
        s_scan_requested = false;
        s_accept_reports = false;
        ++s_snapshot.revision;
    }
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}
