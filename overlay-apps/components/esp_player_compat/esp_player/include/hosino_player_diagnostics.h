/* SPDX-FileCopyrightText: 2026 Hosino contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HOSINO_PLAYER_DIAG_AV_YIELD_SKIPPED = 0,
    HOSINO_PLAYER_DIAG_VIDEO_ONLY_YIELD,
    HOSINO_PLAYER_DIAG_AV_YIELD_NETWORK,
    HOSINO_PLAYER_DIAG_DEMUX_AUDIO_HOLD,
    HOSINO_PLAYER_DIAG_DEMUX_POOL_WAIT,
    HOSINO_PLAYER_DIAG_VIDEO_QUEUE_PUSH,
    HOSINO_PLAYER_DIAG_AUDIO_INPUT_ACQUIRE,
    HOSINO_PLAYER_DIAG_DROP_LATE_AUDIO,
    HOSINO_PLAYER_DIAG_DROP_LATE_WALL,
    HOSINO_PLAYER_DIAG_CLOCK_REANCHOR,
    HOSINO_PLAYER_DIAG_AUDIO_SYNC_GAP_ACCEPTED,
    HOSINO_PLAYER_DIAG_AUDIO_SYNC_DROP,
    HOSINO_PLAYER_DIAG_DEMUX_VIDEO_PACKET,
    HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY,
    HOSINO_PLAYER_DIAG_EVENT_COUNT,
} hosino_player_diag_event_t;

enum {
    HOSINO_PLAYER_CLOCK_AUDIO_VALID = 1u << 0,
    HOSINO_PLAYER_CLOCK_PAUSED = 1u << 1,
    HOSINO_PLAYER_CLOCK_SEEKING = 1u << 2,
    HOSINO_PLAYER_CLOCK_STOPPING = 1u << 3,
    HOSINO_PLAYER_CLOCK_AUDIO_MODE = 1u << 4,
    HOSINO_PLAYER_CLOCK_SPEED_ONE = 1u << 5,
};
enum {
    HOSINO_PLAYER_CLOCK_KEEP_ON_TIME = 0,
    HOSINO_PLAYER_CLOCK_DROP_AUDIO,
    HOSINO_PLAYER_CLOCK_DROP_WALL,
    HOSINO_PLAYER_CLOCK_NO_ANCHOR,
    HOSINO_PLAYER_CLOCK_FIRST_FRAME,
    HOSINO_PLAYER_CLOCK_GATE_CLOSED,
    HOSINO_PLAYER_CLOCK_REANCHOR,
};
/* State is sampled BEFORE the decision, including before any clock reset.
 * decision and reanchor_reason describe that decision. Anchor elapsed is zero
 * when no valid anchor exists or wall time is before it; inspect flags first.
 * Pointer is borrowed only for the call. This is not a hardware DAC clock. */
typedef struct {
    uint64_t video_pts_ms;
    uint64_t audio_pts_ms;
    uint64_t anchor_pts_ms;
    uint64_t anchor_elapsed_ms;
    uint32_t source_fps;
    uint32_t threshold_ms;
    uint32_t flags;
    uint32_t decision;
    uint32_t reanchor_reason;
} hosino_player_clock_sample_t;

/* Optional, non-blocking aggregation hook. No pointer/ownership is transferred.
 * elapsed_us is wall time, including preemption; skipped yield uses zero.
 * amount is requested wait in MICROSECONDS for yield/hold/pool events;
 * for VIDEO_QUEUE_PUSH it is the input payload bytes, and for
 * AUDIO_INPUT_ACQUIRE it is successful acquired bytes (zero on non-OK).
 * DROP_LATE events use elapsed_us for lateness (not execution time), amount=0.
 * CLOCK_REANCHOR uses elapsed_us=0 and amount=reason: 1 fresh PCM, 2 non-increasing
 * PTS, 3 long stall, 4 lateness over 1s, 5 forward discontinuity.
 * AUDIO_SYNC_GAP_ACCEPTED counts AUDIO-master packets admitted despite a PTS
 * gap beyond the configured threshold (not a sink success). AUDIO_SYNC_DROP
 * counts delay rejection in other master modes, excluding seek/stop gates.
 * Both use zero elapsed_us/amount/result; aggregate count only, no per-frame log.
 * result is the GMF I/O result for push/acquire, otherwise zero. A seek abort
 * is an expected non-OK I/O result, not by itself a fatal playback error.
 *
 * The provider must define HOSINO_PLAYER_DIAGNOSTICS_PROVIDER before including
 * this header so its implementation has strong rather than weak binding.
 * Without a provider diagnostics are no-ops; playback policy is unchanged by
 * the presence of a provider.
 * The provider must not log per event, allocate, block, or call back into player.
 */
#ifdef HOSINO_PLAYER_DIAGNOSTICS_PROVIDER
#define HOSINO_PLAYER_DIAGNOSTICS_WEAK
#else
#define HOSINO_PLAYER_DIAGNOSTICS_WEAK __attribute__((weak))
#endif
void hosino_player_diagnostics_record(hosino_player_diag_event_t event,
                                     uint32_t elapsed_us, uint32_t amount,
                                     int result) HOSINO_PLAYER_DIAGNOSTICS_WEAK;
void hosino_player_clock_record(const hosino_player_clock_sample_t *sample)
    HOSINO_PLAYER_DIAGNOSTICS_WEAK;
#undef HOSINO_PLAYER_DIAGNOSTICS_WEAK

#ifndef HOSINO_PLAYER_DIAGNOSTICS_PROVIDER
static inline void hosino_player_clock_note(const hosino_player_clock_sample_t *sample)
{
    if (hosino_player_clock_record) hosino_player_clock_record(sample);
}

static inline void hosino_player_diagnostics_note(hosino_player_diag_event_t event,
                                                  int64_t elapsed_us,
                                                  uint32_t amount, int result)
{
    if (hosino_player_diagnostics_record) {
        const uint32_t bounded_us = elapsed_us <= 0 ? 0 :
            (elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us);
        hosino_player_diagnostics_record(event, bounded_us, amount, result);
    }
}
#endif

#ifdef __cplusplus
}
#endif
