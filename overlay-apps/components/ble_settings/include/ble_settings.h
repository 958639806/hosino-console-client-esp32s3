/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_SETTINGS_MAX_DEVICES 20
#define BLE_SETTINGS_NAME_BYTES 64
#define BLE_SETTINGS_SCAN_MS 10000

typedef enum {
    BLE_SETTINGS_OFF,
    BLE_SETTINGS_STARTING,
    BLE_SETTINGS_READY,
    BLE_SETTINGS_SCANNING,
    BLE_SETTINGS_STOPPING,
    BLE_SETTINGS_ERROR,
} ble_settings_state_t;

typedef struct {
    uint8_t address[6]; /* NimBLE little-endian address; display bytes 5..0. */
    uint8_t address_type;
    int8_t rssi;
    char name[BLE_SETTINGS_NAME_BYTES];
} ble_settings_device_t;

typedef struct {
    ble_settings_state_t state;
    bool requested_enabled;
    bool enabled;
    bool scanning;
    bool restart_required; /* A failed shutdown retained resources for safety. */
    uint8_t count;
    int last_error;        /* Actual ESP-IDF or NimBLE return code. */
    uint32_t revision;
    ble_settings_device_t devices[BLE_SETTINGS_MAX_DEVICES];
} ble_settings_snapshot_t;

/* Nonblocking requests. The service starts disabled, owns the sole NimBLE
 * instance, and serializes its lifetime on a background worker. No NVS erase,
 * connections, pairing, advertising, classic audio, or PAN are performed. */
esp_err_t ble_settings_set_enabled(bool enabled);
esp_err_t ble_settings_start_scan(void);
void ble_settings_cancel_scan(void);
/* Also observes the overall stop deadline; timeout changes status only and
 * never frees resources or terminates the host/worker. */
void ble_settings_get_snapshot(ble_settings_snapshot_t *out);

#ifdef __cplusplus
}
#endif
