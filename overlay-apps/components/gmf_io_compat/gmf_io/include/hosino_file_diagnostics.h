/* SPDX-FileCopyrightText: 2026 Hosino contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HOSINO_FILE_DIAG_READ = 0,
    HOSINO_FILE_DIAG_SEEK,
    HOSINO_FILE_DIAG_EVENT_COUNT,
} hosino_file_diag_event_t;

/* Optional hooks with no ESP Player dependency. The provider defines
 * HOSINO_FILE_DIAGNOSTICS_PROVIDER before including this header for strong
 * symbols. It must neither block, allocate, log per call nor access the file.
 *
 * bind receives the reader's exact mounted path once after successful fopen.
 * Return a nonzero current-video-session generation, or zero to disable all
 * timing for this reader. The provider must reject stale generations after
 * stop/reopen, including when the same pathname is opened again.
 *
 * record measures each individual fread/fseek call using elapsed wall time;
 * this includes stdio, filesystem and scheduling, not only SD hardware time.
 * actual_bytes is the successful fread byte count (zero for seek).
 * error_code is zero without an error, otherwise errno or EIO if errno is zero.
 * eof reports the stream EOF indicator after fread; it is false for seek.
 * No pointer or file ownership is transferred to either hook.
 */
#ifdef HOSINO_FILE_DIAGNOSTICS_PROVIDER
#define HOSINO_FILE_DIAGNOSTICS_WEAK
#else
#define HOSINO_FILE_DIAGNOSTICS_WEAK __attribute__((weak))
#endif
uint32_t hosino_file_diagnostics_bind(const char *exact_path)
    HOSINO_FILE_DIAGNOSTICS_WEAK;
void hosino_file_diagnostics_record(uint32_t generation,
                                    hosino_file_diag_event_t event,
                                    uint32_t elapsed_us, uint32_t actual_bytes,
                                    int error_code, bool eof)
    HOSINO_FILE_DIAGNOSTICS_WEAK;
#undef HOSINO_FILE_DIAGNOSTICS_WEAK

#ifdef __cplusplus
}
#endif
