/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Control-worker only, BEFORE opening/sending HTTP. Resolves the configured
 * HTTPS host as IPv4; never substitutes an IP into URL/Host/SNI. The deadline
 * is the caller's existing total request deadline, not an extra timeout. */
esp_err_t bili_dns_prepare(const char *base_url,int64_t deadline_us,
                           bool (*active)(void *),void *context);
#ifdef __cplusplus
}
#endif
