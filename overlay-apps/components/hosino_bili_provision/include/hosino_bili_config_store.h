/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"
#include "provision_parser.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Synchronous on caller's control worker, never GUI. All NVS calls run on a
 * serialized INTERNAL 4KiB task; caller/config remain valid until return.
 * Origin+token share one versioned blob hosino_bili/bridge_v1. Invalid or
 * partial records fail closed; obsolete separate string keys are not read. */
esp_err_t hosino_bili_config_load(bili_provision_config_t *config);
esp_err_t hosino_bili_config_save(const bili_provision_config_t *config);
#ifdef __cplusplus
}
#endif
