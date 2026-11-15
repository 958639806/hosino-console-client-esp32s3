/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Start once after NVS initialization. Reads only the physical USB-JTAG VFS;
 * never changes UART, GPIO, networking, BLE, or another NVS namespace. */
esp_err_t hosino_bili_provision_start(void);
#ifdef __cplusplus
}
#endif
