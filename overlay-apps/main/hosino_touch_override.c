/* SPDX-License-Identifier: Apache-2.0
 * Bounded CST92xx two-contact reports, one stable system pointer, native LVGL
 * gestures. The adapter continues to own IRQ registration and input lifetime.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_input.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hosino_touch_packet.h"

#define HOSINO_TOUCH_ADDRESS 0x5A
#define HOSINO_TOUCH_IRQ_GPIO 11
#define HOSINO_TOUCH_TIMEOUT_MS 20
#define HOSINO_TOUCH_LOG_US 5000000LL
#define HOSINO_TOUCH_SLOW_US 10000U
#define HOSINO_TOUCH_INPUT_PERIOD_MS 8U
#define HOSINO_TOUCH_RELEASE_US 32000LL
#define HOSINO_TOUCH_IRQ_SLOTS 4U

_Static_assert(CONFIG_ESP_LCD_TOUCH_MAX_POINTS >= HOSINO_TOUCH_CONTACTS,
               "Two-contact reports require at least two ESP LCD touch slots");

static const char *TAG = "touch_guard";
static portMUX_TYPE s_guard_mux = portMUX_INITIALIZER_UNLOCKED;

/* HOST_TOUCH_TELEMETRY_BEGIN
 * Observation only: no coordinate values, contact IDs or key locations leave
 * this module. Report gaps include ordinary IRQ silence; they are not proof
 * of a controller dropping a contact. */
typedef enum {
    TOUCH_FAULT_NONE,
    TOUCH_FAULT_ADDRESS,
    TOUCH_FAULT_READ,
    TOUCH_FAULT_ACK,
    TOUCH_FAULT_MARKER,
    TOUCH_FAULT_FORMAT,
    TOUCH_FAULT_BOUNDS,
    TOUCH_FAULT_OUTPUT,
    TOUCH_FAULT_COUNT
} touch_fault_t;

typedef struct {
    uint32_t reads, read_errors, slow_reads, read_max_us;
    uint32_t two_point_reads, max_points;
    uint32_t raw_zero, raw_nonzero, raw_zero_to_nonzero, raw_nonzero_to_zero;
    uint32_t raw_id_changes, primary_id_changes;
    uint32_t primary_missing, blocked_reads, block_enters;
    uint32_t reported_down, reported_up, zero_after_pressed, error_after_pressed;
    uint32_t pressed_report_gaps, pressed_report_gap_max_us;
    uint32_t exact_edge_output, offscreen_output;
    uint32_t raw_header_zero, raw_inactive_records;
    uint32_t zero_pending, zero_resumed, zero_confirmed, zero_synthetic;
    uint32_t faults[TOUCH_FAULT_COUNT];
} touch_stats_t;

typedef struct {
    bool raw_valid, primary_missing, blocked, reported_pressed, primary_valid;
    bool exact_edge_output, offscreen_output;
    bool header_zero, inactive_records;
    bool zero_pending, zero_resumed, zero_confirmed, zero_synthetic;
    uint8_t raw_count, raw_first_id, primary_id;
    touch_fault_t fault;
} touch_read_diag_t;

typedef struct {
    int64_t last_read_us;
    bool raw_id_valid, last_raw_valid, reported_pressed, blocked, primary_valid;
    uint8_t raw_first_id, primary_id, last_raw_count;
} touch_diag_state_t;

static void touch_diag_note(touch_stats_t *stats, touch_diag_state_t *state,
                            const touch_read_diag_t *diag, int64_t now)
{
    if (diag->raw_valid) {
        if (state->last_raw_valid && !state->last_raw_count && diag->raw_count)
            ++stats->raw_zero_to_nonzero;
        if (state->last_raw_valid && state->last_raw_count && !diag->raw_count)
            ++stats->raw_nonzero_to_zero;
        if (diag->raw_count) {
            ++stats->raw_nonzero;
            if (state->raw_id_valid && state->raw_first_id != diag->raw_first_id)
                ++stats->raw_id_changes;
        } else ++stats->raw_zero;
    }
    if (diag->primary_missing) ++stats->primary_missing;
    if (diag->exact_edge_output) ++stats->exact_edge_output;
    if (diag->offscreen_output) ++stats->offscreen_output;
    if (diag->raw_valid && diag->header_zero) ++stats->raw_header_zero;
    if (diag->raw_valid && diag->inactive_records) ++stats->raw_inactive_records;
    if (diag->zero_pending) ++stats->zero_pending;
    if (diag->zero_resumed) ++stats->zero_resumed;
    if (diag->zero_confirmed) ++stats->zero_confirmed;
    if (diag->zero_synthetic) ++stats->zero_synthetic;
    if (diag->blocked) ++stats->blocked_reads;
    if (diag->blocked && !state->blocked) ++stats->block_enters;
    if (diag->reported_pressed && !state->reported_pressed) ++stats->reported_down;
    if (!diag->reported_pressed && state->reported_pressed) {
        ++stats->reported_up;
        if (diag->raw_valid && !diag->raw_count) ++stats->zero_after_pressed;
        if (diag->fault != TOUCH_FAULT_NONE) ++stats->error_after_pressed;
    }
    if (diag->reported_pressed && state->reported_pressed && diag->primary_valid &&
            state->primary_valid && state->primary_id != diag->primary_id)
        ++stats->primary_id_changes;
    if (state->reported_pressed && state->last_read_us && now > state->last_read_us) {
        uint64_t gap = (uint64_t)(now - state->last_read_us);
        uint32_t bounded_gap = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
        if (bounded_gap >= 50000U) ++stats->pressed_report_gaps;
        if (bounded_gap > stats->pressed_report_gap_max_us)
            stats->pressed_report_gap_max_us = bounded_gap;
    }
    if (diag->fault > TOUCH_FAULT_NONE && diag->fault < TOUCH_FAULT_COUNT)
        ++stats->faults[diag->fault];
    state->last_read_us = now;
    state->raw_id_valid = diag->raw_valid && diag->raw_count;
    state->last_raw_valid = diag->raw_valid;
    state->last_raw_count = diag->raw_count;
    state->raw_first_id = diag->raw_first_id;
    state->reported_pressed = diag->reported_pressed;
    state->blocked = diag->blocked;
    state->primary_valid = diag->primary_valid;
    state->primary_id = diag->primary_id;
}
/* HOST_TOUCH_TELEMETRY_END */

typedef struct touch_guard {
    struct touch_guard *next;
    lv_indev_t *indev;
    esp_lcd_touch_handle_t handle;
    touch_stats_t stats;
    touch_diag_state_t diag_state;
    hosino_touch_primary_t primary;
    hosino_touch_packet_t last_packet;
    lv_indev_read_cb_t adapter_read;
    uint32_t last_irq_generation;
    int64_t release_deadline_us;
    uint8_t irq_slot;
    bool release_due, debounce_enabled;
    int64_t last_log_us;
    bool unregistering;
} touch_guard_t;

static touch_guard_t *s_guards;
/* ISR observes only this static table, never a heap guard or LVGL object. The
 * adapter retains ownership of IRQ registration and callback lifetime. */
static struct {
    esp_lcd_touch_handle_t handle;
    uint32_t generation, owners;
} s_irq_sources[HOSINO_TOUCH_IRQ_SLOTS];

static int claim_irq_source(esp_lcd_touch_handle_t handle)
{
    int slot=-1;
    portENTER_CRITICAL(&s_guard_mux);
    for (unsigned i=0;i<HOSINO_TOUCH_IRQ_SLOTS;++i)
        if (s_irq_sources[i].handle==handle) {slot=(int)i;break;}
    if (slot<0) for (unsigned i=0;i<HOSINO_TOUCH_IRQ_SLOTS;++i)
        if (!s_irq_sources[i].owners) {slot=(int)i;s_irq_sources[i].handle=handle;break;}
    if (slot>=0) ++s_irq_sources[slot].owners;
    portEXIT_CRITICAL(&s_guard_mux);
    return slot;
}
static void release_irq_source(uint8_t slot)
{
    portENTER_CRITICAL(&s_guard_mux);
    if (slot<HOSINO_TOUCH_IRQ_SLOTS && s_irq_sources[slot].owners && !--s_irq_sources[slot].owners)
        s_irq_sources[slot].handle=NULL;
    portEXIT_CRITICAL(&s_guard_mux);
}
static uint32_t irq_generation(uint8_t slot)
{
    uint32_t generation;
    portENTER_CRITICAL(&s_guard_mux);
    generation=s_irq_sources[slot].generation;
    portEXIT_CRITICAL(&s_guard_mux);
    return generation;
}
static void IRAM_ATTR observe_touch_irq(esp_lcd_touch_handle_t handle,void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL_ISR(&s_guard_mux);
    for (unsigned i=0;i<HOSINO_TOUCH_IRQ_SLOTS;++i)
        if (s_irq_sources[i].owners && s_irq_sources[i].handle==handle) {
            ++s_irq_sources[i].generation;break;
        }
    portEXIT_CRITICAL_ISR(&s_guard_mux);
}

static void guarded_input_read(lv_indev_t *indev,lv_indev_data_t *data)
{
    touch_guard_t *guard=NULL;
    portENTER_CRITICAL(&s_guard_mux);
    for (touch_guard_t *p=s_guards;p;p=p->next)
        if (p->indev==indev && !p->unregistering) {guard=p;break;}
    portEXIT_CRITICAL(&s_guard_mux);
    if (!guard || !guard->adapter_read || guard->adapter_read==guarded_input_read) {
        data->state=LV_INDEV_STATE_RELEASED;return;
    }
    /* Normal LVGL read service supplies the deadline even if true UP causes
     * no further hardware IRQ. Notify only schedules the existing adapter's
     * custom read; the synthetic path below performs NO speculative I2C IO. */
    if (guard->release_deadline_us && esp_timer_get_time()>=guard->release_deadline_us) {
        guard->release_due=true;
        (void)esp_lv_adapter_touch_notify_interrupt(indev);
    }
    guard->adapter_read(indev,data);
}

esp_err_t __real_esp_lcd_new_panel_io_i2c_v2(i2c_master_bus_handle_t bus,
    const esp_lcd_panel_io_i2c_config_t *cfg, esp_lcd_panel_io_handle_t *out);
lv_indev_t *__real_esp_lv_adapter_register_touch(const esp_lv_adapter_touch_config_t *cfg);
esp_err_t __real_esp_lv_adapter_unregister_touch(lv_indev_t *indev);

esp_err_t __wrap_esp_lcd_new_panel_io_i2c_v2(i2c_master_bus_handle_t bus,
    const esp_lcd_panel_io_i2c_config_t *cfg, esp_lcd_panel_io_handle_t *out)
{
    if (!cfg || cfg->dev_addr != HOSINO_TOUCH_ADDRESS) {
        return __real_esp_lcd_new_panel_io_i2c_v2(bus, cfg, out);
    }
    esp_lcd_panel_io_i2c_config_t adjusted = *cfg;
    /* Preserve an already shorter, explicitly bounded transaction timeout. */
    if (adjusted.transaction_timeout_ms <= 0 ||
        adjusted.transaction_timeout_ms > HOSINO_TOUCH_TIMEOUT_MS) {
        adjusted.transaction_timeout_ms = HOSINO_TOUCH_TIMEOUT_MS;
    }
    return __real_esp_lcd_new_panel_io_i2c_v2(bus, &adjusted, out);
}

static uint32_t elapsed_us(int64_t start, int64_t end)
{
    if (end <= start) return 0;
    const uint64_t elapsed = (uint64_t)(end - start);
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static void record_read(touch_guard_t *guard, uint32_t duration_us, esp_err_t result,
                        uint8_t hardware_points, const touch_read_diag_t *diag)
{
    touch_stats_t report = {0};
    bool log_due = false;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_guard_mux);
    ++guard->stats.reads;
    if (result != ESP_OK) ++guard->stats.read_errors;
    if (duration_us >= HOSINO_TOUCH_SLOW_US) ++guard->stats.slow_reads;
    if (duration_us > guard->stats.read_max_us) guard->stats.read_max_us = duration_us;
    if (hardware_points == HOSINO_TOUCH_CONTACTS) ++guard->stats.two_point_reads;
    if (hardware_points > guard->stats.max_points) guard->stats.max_points = hardware_points;
    touch_diag_note(&guard->stats, &guard->diag_state, diag, now);
    if (now - guard->last_log_us >= HOSINO_TOUCH_LOG_US) {
        report = guard->stats;
        memset(&guard->stats, 0, sizeof(guard->stats));
        guard->last_log_us = now;
        log_due = true;
    }
    portEXIT_CRITICAL(&s_guard_mux);
    if (log_due) {
        ESP_LOGI(TAG, "read_n=%" PRIu32 " errors=%" PRIu32 " slow_n=%" PRIu32
            " read_max_us=%" PRIu32 " two_point_n=%" PRIu32 " max_points=%" PRIu32,
            report.reads, report.read_errors, report.slow_reads, report.read_max_us,
            report.two_point_reads, report.max_points);
        ESP_LOGI(TAG, "raw0=%" PRIu32 " rawN=%" PRIu32
            " raw0toN=%" PRIu32 " rawNto0=%" PRIu32 " raw_idchg=%" PRIu32
            " primary_idchg=%" PRIu32 " primary_missing=%" PRIu32
            " blocked=%" PRIu32 " block_enter=%" PRIu32
            " down=%" PRIu32 " up=%" PRIu32 " zero_up=%" PRIu32 " error_up=%" PRIu32
            " pressed_gap_n=%" PRIu32 " pressed_gap_max_us=%" PRIu32
            " fault_addr=%" PRIu32 " fault_read=%" PRIu32 " fault_ack=%" PRIu32
            " fault_marker=%" PRIu32 " fault_format=%" PRIu32 " fault_bounds=%" PRIu32
            " fault_output=%" PRIu32 " exact_edge=%" PRIu32 " offscreen_output=%" PRIu32,
            report.raw_zero, report.raw_nonzero, report.raw_zero_to_nonzero, report.raw_nonzero_to_zero,
            report.raw_id_changes, report.primary_id_changes,
            report.primary_missing, report.blocked_reads, report.block_enters,
            report.reported_down, report.reported_up, report.zero_after_pressed, report.error_after_pressed,
            report.pressed_report_gaps, report.pressed_report_gap_max_us,
            report.faults[TOUCH_FAULT_ADDRESS], report.faults[TOUCH_FAULT_READ], report.faults[TOUCH_FAULT_ACK],
            report.faults[TOUCH_FAULT_MARKER], report.faults[TOUCH_FAULT_FORMAT],
            report.faults[TOUCH_FAULT_BOUNDS], report.faults[TOUCH_FAULT_OUTPUT],
            report.exact_edge_output, report.offscreen_output);
        ESP_LOGI(TAG,"header0=%" PRIu32 " inactive_records=%" PRIu32
            " zero_pending=%" PRIu32 " zero_resumed=%" PRIu32
            " zero_confirmed=%" PRIu32 " zero_synthetic=%" PRIu32,
            report.raw_header_zero,report.raw_inactive_records,report.zero_pending,
            report.zero_resumed,report.zero_confirmed,report.zero_synthetic);
    }
}

static void clear_driver_points(esp_lcd_touch_handle_t tp)
{
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    memset(tp->data.coords, 0, sizeof(tp->data.coords));
    portEXIT_CRITICAL(&tp->data.lock);
}

static esp_err_t read_packet(esp_lcd_touch_handle_t tp, hosino_touch_packet_t *packet,
                             touch_fault_t *fault,touch_read_diag_t *diag)
{
    const uint8_t address_low = 0x00;
    uint8_t data[HOSINO_TOUCH_PACKET_BYTES] = {0};
    esp_err_t result = esp_lcd_panel_io_tx_param(tp->io, 0xD0, &address_low, 1);
    if (result != ESP_OK) { *fault = TOUCH_FAULT_ADDRESS; return result; }
    /* Retain the board IDF driver's address-to-read delay while changing only
     * report length/ACK. Every individual IO transaction has the 20ms bound. */
    vTaskDelay(pdMS_TO_TICKS(2));
    result = esp_lcd_panel_io_rx_param(tp->io, -1, data, sizeof(data));
    if (result != ESP_OK) { *fault = TOUCH_FAULT_READ; return result; }
    /* Match SensorLib: acknowledge the consumed report before validating its
     * device marker, including an empty or malformed report. Wire: D0 00 AB. */
    const uint8_t ack[] = {0x00, 0xAB};
    result = esp_lcd_panel_io_tx_param(tp->io, 0xD0, ack, sizeof(ack));
    if (result != ESP_OK) { *fault = TOUCH_FAULT_ACK; return result; }
    if (!hosino_touch_parse_packet(data, packet)) {
        *fault = data[6] == 0xAB ? TOUCH_FAULT_FORMAT : TOUCH_FAULT_MARKER;
        return ESP_ERR_INVALID_RESPONSE;
    }
    diag->header_zero=(data[5]&0x7F)==0;
    diag->inactive_records=(data[5]&0x7F)>0 && packet->count==0;
    for (uint8_t i = 0; i < packet->count; ++i) {
        if ((tp->config.x_max && packet->contact[i].x > tp->config.x_max) ||
            (tp->config.y_max && packet->contact[i].y > tp->config.y_max))
            { *fault = TOUCH_FAULT_BOUNDS; return ESP_ERR_INVALID_RESPONSE; }
    }
    return ESP_OK;
}

static esp_err_t touch_read(esp_lcd_touch_handle_t tp,
    esp_lcd_touch_point_data_t *points, uint8_t *count, uint8_t max_count, void *ctx)
{
    touch_guard_t *guard = ctx;
    if (count) *count = 0;
    if (!guard || tp != guard->handle || !points || !count || !max_count) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The adapter consumes points only on ESP_OK. Clear output as well so a
     * caller cannot accidentally use a pressed coordinate left by an old read. */
    memset(points, 0, sizeof(*points) * max_count);
    const int64_t start = esp_timer_get_time();
    hosino_touch_packet_t packet = {0};
    touch_read_diag_t diag = {0};
    const uint32_t generation=irq_generation(guard->irq_slot);
    const bool synthetic=guard->release_due && guard->release_deadline_us &&
        start>=guard->release_deadline_us && generation==guard->last_irq_generation;
    guard->release_due=false;
    esp_err_t result=ESP_OK;
    if (synthetic) {
        guard->release_deadline_us=0;
        diag.zero_confirmed=diag.zero_synthetic=true;
    } else {
        guard->last_irq_generation=generation;
        result=read_packet(tp,&packet,&diag.fault,&diag);
    }
    const uint8_t hardware_points = result == ESP_OK ? packet.count : 0;
    diag.raw_valid = result == ESP_OK && !synthetic;
    diag.raw_count = hardware_points;
    if (diag.raw_valid && packet.count) {
        diag.raw_first_id = packet.contact[0].id;
        if (guard->primary.primary_valid) {
            diag.primary_missing = true;
            for (uint8_t i = 0; i < packet.count; ++i)
                if (packet.contact[i].id == guard->primary.primary_id) diag.primary_missing = false;
        }
    }
    if (result == ESP_OK) {
        if (!synthetic && guard->debounce_enabled && !packet.count &&
                guard->last_packet.count==1 && guard->primary.primary_valid &&
                !guard->primary.block_until_all_up) {
            if (!guard->release_deadline_us) {
                guard->release_deadline_us=start+HOSINO_TOUCH_RELEASE_US;
                diag.zero_pending=true;
            }
            /* Further zero packets never extend the original deadline. */
            if (esp_timer_get_time()<guard->release_deadline_us) packet=guard->last_packet;
            else {guard->release_deadline_us=0;diag.zero_confirmed=true;}
        } else if (!synthetic && guard->release_deadline_us) {
            for (uint8_t i=0;i<packet.count;++i)
                if (packet.contact[i].id==guard->primary.primary_id) diag.zero_resumed=true;
            guard->release_deadline_us=0;
        }
        hosino_touch_primary_filter(&guard->primary, &packet);
        portENTER_CRITICAL(&tp->data.lock);
        tp->data.points = packet.count;
        for (uint8_t i = 0; i < packet.count; ++i) {
            tp->data.coords[i].x = packet.contact[i].x;
            tp->data.coords[i].y = packet.contact[i].y;
            tp->data.coords[i].strength = 1;
        }
        portEXIT_CRITICAL(&tp->data.lock);
        /* Reuse the existing get_xy and generic mirror/swap processing. The
         * board get_xy accepts data.points rather than hard-clamping to one. */
        result = esp_lcd_touch_get_data(tp, points, count, max_count);
        if (result == ESP_OK) {
            if (*count > packet.count || *count > max_count) result = ESP_ERR_INVALID_RESPONSE;
            else for (uint8_t i = 0; i < *count; ++i) {
                points[i].track_id = packet.contact[i].id;
                /* Count only, after the existing mirror/swap transformation.
                 * Bounds remain untouched. Exact maximum may originate from
                 * the generic mirror's max-minus-zero coordinate. */
                if ((tp->config.x_max && points[i].x == tp->config.x_max) ||
                    (tp->config.y_max && points[i].y == tp->config.y_max))
                    diag.exact_edge_output = true;
                if ((tp->config.x_max && points[i].x >= tp->config.x_max) ||
                    (tp->config.y_max && points[i].y >= tp->config.y_max))
                    diag.offscreen_output = true;
            }
        }
    }
    if (result != ESP_OK) {
        guard->release_deadline_us=0;
        if (diag.fault == TOUCH_FAULT_NONE) diag.fault = TOUCH_FAULT_OUTPUT;
        hosino_touch_primary_error(&guard->primary);
        clear_driver_points(tp);
        *count = 0;
        memset(points, 0, sizeof(*points) * max_count);
    }
    if (result==ESP_OK) guard->last_packet=packet;
    else memset(&guard->last_packet,0,sizeof(guard->last_packet));
    diag.blocked = guard->primary.block_until_all_up;
    diag.primary_valid = guard->primary.primary_valid;
    diag.primary_id = guard->primary.primary_id;
    diag.reported_pressed = result == ESP_OK && *count > 0;
    record_read(guard, elapsed_us(start, esp_timer_get_time()), result, hardware_points, &diag);
    return result;
}

lv_indev_t *__wrap_esp_lv_adapter_register_touch(const esp_lv_adapter_touch_config_t *cfg)
{
    if (!cfg || !cfg->handle || cfg->handle->config.int_gpio_num != HOSINO_TOUCH_IRQ_GPIO ||
        cfg->callbacks.custom_touch_read || cfg->callbacks.on_interrupt ||
        cfg->multi_touch.mode != ESP_LV_ADAPTER_TOUCH_MODE_SINGLE) {
        return __real_esp_lv_adapter_register_touch(cfg);
    }
    touch_guard_t *guard = heap_caps_calloc(1, sizeof(*guard), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!guard) {
        ESP_LOGE(TAG, "Touch guard allocation failed");
        return NULL;
    }
    guard->handle = cfg->handle;
    const int irq_slot=claim_irq_source(cfg->handle);
    if (irq_slot<0) {heap_caps_free(guard);return __real_esp_lv_adapter_register_touch(cfg);}
    guard->irq_slot=(uint8_t)irq_slot;
    guard->last_irq_generation=irq_generation(guard->irq_slot);
    guard->last_log_us = esp_timer_get_time();
    esp_lv_adapter_touch_config_t adjusted = *cfg;
    adjusted.callbacks.custom_touch_read = touch_read;
    /* Observer ignores user_ctx: no heap guard is referenced from ISR. */
    adjusted.callbacks.on_interrupt=observe_touch_irq;
    adjusted.callbacks.user_ctx = guard;
    /* register_touch copies callback values and owns IRQ cleanup, including
     * failure paths. No pointer to the local adjusted configuration escapes. */
    lv_indev_t *indev = __real_esp_lv_adapter_register_touch(&adjusted);
    if (!indev) { release_irq_source(guard->irq_slot);heap_caps_free(guard);return NULL; }
    guard->indev = indev;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        guard->adapter_read=lv_indev_get_read_cb(indev);
        /* Link before installing the wrapper, while the GUI is locked. */
        portENTER_CRITICAL(&s_guard_mux);
        guard->next=s_guards;s_guards=guard;
        portEXIT_CRITICAL(&s_guard_mux);
        if (guard->adapter_read && guard->adapter_read!=guarded_input_read) {
            guard->debounce_enabled=true;
            lv_indev_set_read_cb(indev,guarded_input_read);
        }
        lv_timer_t *timer = lv_indev_get_read_timer(indev);
        if (timer) lv_timer_set_period(timer, HOSINO_TOUCH_INPUT_PERIOD_MS);
#if LV_USE_GESTURE_RECOGNITION
        lv_indev_set_pinch_up_threshold(indev, 1.15f);
        lv_indev_set_pinch_down_threshold(indev, 0.85f);
#endif
        esp_lv_adapter_unlock();
        ESP_LOGI(TAG, "two-contact parser, single system pointer; input_poll_ms=%u (not measured hardware Hz)",
                 (unsigned)HOSINO_TOUCH_INPUT_PERIOD_MS);
    } else {
        ESP_LOGW(TAG, "Input timer lock unavailable; original period retained");
        portENTER_CRITICAL(&s_guard_mux);
        guard->next=s_guards;s_guards=guard;
        portEXIT_CRITICAL(&s_guard_mux);
    }
    return indev;
}

esp_err_t __wrap_esp_lv_adapter_unregister_touch(lv_indev_t *indev)
{
    touch_guard_t *guard = NULL;
    portENTER_CRITICAL(&s_guard_mux);
    for (touch_guard_t *item = s_guards; item; item = item->next) {
        if (item->indev == indev) { guard = item; break; }
    }
    const bool busy = guard && guard->unregistering;
    if (guard && !busy) guard->unregistering = true;
    portEXIT_CRITICAL(&s_guard_mux);
    if (busy) return ESP_ERR_INVALID_STATE;
    const esp_err_t result = __real_esp_lv_adapter_unregister_touch(indev);
    if (!guard) return result;
    if (result != ESP_OK) {
        portENTER_CRITICAL(&s_guard_mux);
        guard->unregistering = false;
        portEXIT_CRITICAL(&s_guard_mux);
        return result;
    }
    /* A successful adapter unregister takes the LVGL lock and deletes its
     * input, so the task-context custom read has ended before guard release.
     * The IRQ path never references this guard. */
    portENTER_CRITICAL(&s_guard_mux);
    touch_guard_t **link = &s_guards;
    while (*link && *link != guard) link = &(*link)->next;
    if (*link) *link = guard->next;
    portEXIT_CRITICAL(&s_guard_mux);
    release_irq_source(guard->irq_slot);
    heap_caps_free(guard);
    return result;
}
