#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool ready, remember, busy, saved;
    esp_err_t error;
    uint32_t epoch, revision;
} hs_auth_store_status_t;
/* Singleton, one internal 4 KiB worker. No NVS operation occurs on the caller. */
esp_err_t hs_auth_store_init(void);
void hs_auth_store_status(hs_auth_store_status_t *out);
/* Explicit user preference. Optional cookie is exactly 64 hex characters. */
esp_err_t hs_auth_store_set_remember(bool enabled, const char *cookie);
/* Saves only while the authorization epoch still matches and remember is on. */
esp_err_t hs_auth_store_save(uint32_t epoch, const char *cookie);
/* UINT32_MAX means explicit logout; otherwise stale invalidations are ignored.
 * Clears the token but preserves the user's remember preference. */
void hs_auth_store_forget(uint32_t epoch);
/* One-shot restore. The result is wiped when consumed or cancelled. */
uint32_t hs_auth_store_restore(void);
bool hs_auth_store_take_restore(uint32_t ticket, char cookie[65], uint32_t *epoch, esp_err_t *error);
void hs_auth_store_cancel_restore(uint32_t ticket);
#ifdef HOSINO_AUTH_STORE_HOST_TEST
void hs_auth_store_test_boot(void);
bool hs_auth_store_test_step(void);
#endif
#ifdef __cplusplus
}
#endif
