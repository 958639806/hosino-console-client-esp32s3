// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

#define HS_DEVICE_STORE_SCHEMA 1U
#define HS_DEVICE_TOKEN_CAP 129U
#define HS_DEVICE_PAIR_ID_CAP 33U
typedef struct {
    uint32_t schema;
    bool pending_ack;
    char token[HS_DEVICE_TOKEN_CAP];
    char pair_id[HS_DEVICE_PAIR_ID_CAP];
} hs_device_store_record_t;
typedef struct {
    bool ready, busy, saved, saved_known;
    esp_err_t error;
    uint32_t epoch, revision;
} hs_device_store_status_t;

/* Reuses the single internal-stack auth worker. No caller performs NVS I/O.
 * Explicit device pairing authorizes persistence independently of remember-cookie.
 * Tokens/records are never logged. No automatic erase on network errors. */
esp_err_t hs_device_store_init(void);
void hs_device_store_status(hs_device_store_status_t *out);
/* At most one load/save is in flight. All outputs are tickets, zero on failure.
 * Epoch must match status. Read absence returns schema=1 and empty token/pair.
 * Save accepts schema=1, token hsd_<12 lowercase hex>.<43 base64url> (60 chars),
 * pair_id hsp_<22 base64url> (26 chars). A successful enqueue is NOT persistence.
 * Server ACK is permitted only after the matching save take returns ESP_OK. */
esp_err_t hs_device_store_load(uint32_t expected_epoch, uint32_t *ticket);
esp_err_t hs_device_store_save(uint32_t expected_epoch, const hs_device_store_record_t *record, uint32_t *ticket);
/* Explicit forget/revocation only. Advances epoch and supersedes pending work;
 * an active write finishes before this erase. UINT32_MAX is an explicit force
 * forget, otherwise a stale epoch is rejected. Await successful take for erase.
 * Failed erase or power loss can leave the previous record persisted. */
esp_err_t hs_device_store_forget(uint32_t expected_epoch, uint32_t *ticket);
/* One-shot. Load returns the record; save/forget return an empty schema=1 record.
 * On error outputs are empty. output epoch belongs to the completed operation.
 * Reply/worker temporary secrets are wiped; the caller owns and must wipe out. */
bool hs_device_store_take(uint32_t ticket, hs_device_store_record_t *out,
                          uint32_t *out_epoch, esp_err_t *error);
/* Cancels queued work or its result and advances epoch. Cannot undo an NVS write
 * already executing; no disk erase. Persisted pending_ack can resume on reopen.
 * A canceled or superseded ticket can never produce a successful take. */
void hs_device_store_cancel(uint32_t ticket);

#ifdef HOSINO_AUTH_STORE_HOST_TEST
void hs_device_store_test_boot(void);
bool hs_device_store_test_step(void);
#endif
#ifdef __cplusplus
}
#endif
