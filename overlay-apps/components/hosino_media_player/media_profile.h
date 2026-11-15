/* SPDX-FileCopyrightText: 2026 Hosino contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "hosino_player_diagnostics.h"
#include "hosino_file_diagnostics.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Aggregated wall time in microseconds, including preemption and waits.
 * decode_error counts every non-OK decoder call, including the normal initial
 * output-size/header probe; it does not imply a fatal playback error.
 * Decode total/max include successful calls only. No per-frame logging or
 * allocation is performed by the profiler.
 */
typedef struct {
    uint32_t count, non_ok;
    uint64_t total_us, amount;
    uint32_t max_us, amount_max;
} hosino_media_wait_sample_t;

typedef struct {
    uint32_t count, errors, eof, over_20ms, over_40ms, over_80ms;
    uint64_t total_us, bytes;
    uint32_t max_us;
} hosino_media_file_sample_t;

typedef struct {
    uint32_t decode_ok;
    uint32_t decode_error;
    uint64_t decode_total_us;
    uint32_t decode_max_us;
    uint32_t video_sink_count;
    uint64_t video_sink_total_us;
    uint32_t video_sink_max_us;
    uint32_t audio_write_count;
    uint64_t audio_write_total_us;
    uint32_t audio_write_max_us;
    uint32_t audio_gap_count;
    uint64_t audio_gap_total_us;
    uint32_t audio_gap_max_us;
    uint64_t audio_written_bytes;
    uint32_t audio_short_writes, audio_write_errors;
    hosino_media_wait_sample_t pipeline[HOSINO_PLAYER_DIAG_EVENT_COUNT];
    hosino_media_file_sample_t file[HOSINO_FILE_DIAG_EVENT_COUNT];
    uint32_t clock_samples;
    hosino_player_clock_sample_t last_clock;
} hosino_media_profile_snapshot_t;

/** Start a new video-owner session; call after the previous owner has stopped.
 * A decoder call already in progress when reset occurs is excluded afterwards.
 */
void hosino_media_profile_reset(const char *exact_video_path);
void hosino_media_profile_stop(void);

/** Atomically copy and clear the current interval. NULL leaves it unchanged.
 * A decoder call spanning intervals is counted when it returns.
 */
void hosino_media_profile_take(hosino_media_profile_snapshot_t *out);

/** Optional sink instrumentation, supplied by the sole video-session owner. */
void hosino_media_profile_record_video_sink(uint32_t elapsed_us);

/** gap_us is the time from the preceding write's end to this write's start.
 * Pass UINT32_MAX for the first write / unknown gap; it will not be counted.
 * Audio writes normally block to pace PCM output, so elapsed time alone does
 * not establish audio starvation.
 */
void hosino_media_profile_record_audio_write(uint32_t elapsed_us, uint32_t gap_us,
                                            uint32_t written, uint32_t requested, bool failed);

#ifdef __cplusplus
}
#endif
