/* SPDX-FileCopyrightText: 2026 Hosino contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    HOSINO_MJPEG_ANCHOR_FRESH = 1,
    HOSINO_MJPEG_ANCHOR_BACKWARD = 2,
    HOSINO_MJPEG_ANCHOR_STALL = 3,
    HOSINO_MJPEG_ANCHOR_TOO_LATE = 4,
    HOSINO_MJPEG_ANCHOR_FORWARD = 5,
};

typedef struct {
    uint32_t generation;
    bool audio_valid;
    bool video_seen;
    uint64_t anchor_pts_ms;
    int64_t anchor_wall_us;
    uint64_t audio_pts_ms;
    int64_t audio_wall_us;
    uint64_t video_pts_ms;
} hosino_mjpeg_clock_t;

typedef struct {
    bool drop_audio;
    bool drop_wall;
    uint32_t reanchor_reason;
    uint64_t late_ms;
} hosino_mjpeg_clock_decision_t;

static inline bool hosino_mjpeg_clock_source_eligible(bool local_file, bool mjpeg,
                                                      bool av_enabled, bool audio_track)
{
    return local_file && mjpeg && av_enabled && audio_track;
}

static inline bool hosino_mjpeg_clock_can_run(bool audio_mode, float speed,
                                              bool paused, bool seeking, bool stopping)
{
    return audio_mode && speed == 1.0f && !paused && !seeking && !stopping;
}

/* Caller serializes this state. Reset invalidates in-flight PCM commits. */
static inline void hosino_mjpeg_clock_reset(hosino_mjpeg_clock_t *clock)
{
    const uint32_t next = clock->generation + 1U;
    *clock = (hosino_mjpeg_clock_t){.generation = next};
}

/* A PCM block has successfully passed the audio renderer. pts_ms denotes its
 * start, deliberately excluding any guessed I2S latency or sample counter.
 * Never continuously re-anchor to slow audio: wall time must expose starvation.
 */
static inline uint32_t hosino_mjpeg_clock_audio_commit(hosino_mjpeg_clock_t *clock,
                                                      uint32_t generation,
                                                      uint64_t pts_ms, int64_t now_us)
{
    if (generation != clock->generation) return 0;
    uint32_t reason = 0;
    if (!clock->audio_valid) reason = HOSINO_MJPEG_ANCHOR_FRESH;
    else if (pts_ms < clock->audio_pts_ms) reason = HOSINO_MJPEG_ANCHOR_BACKWARD;
    else if (now_us < clock->audio_wall_us || now_us - clock->audio_wall_us > 1000000)
        reason = HOSINO_MJPEG_ANCHOR_STALL;
    else if (pts_ms - clock->audio_pts_ms > 1000U) reason = HOSINO_MJPEG_ANCHOR_FORWARD;
    if (reason) {
        clock->anchor_pts_ms = pts_ms;
        clock->anchor_wall_us = now_us;
        clock->video_seen = false;
    }
    clock->audio_valid = true;
    clock->audio_pts_ms = pts_ms;
    clock->audio_wall_us = now_us;
    return reason;
}

/* Only called for eligible local MJPEG A/V at normal speed. An explicit valid
 * bit makes PTS zero a real timestamp. Start/discontinuity always retains a
 * frame; a >1s stall invalidates the clock until fresh PCM arrives.
 */
static inline hosino_mjpeg_clock_decision_t hosino_mjpeg_clock_decide(
    hosino_mjpeg_clock_t *clock, uint64_t pts_ms, int64_t now_us, uint32_t frame_ms)
{
    hosino_mjpeg_clock_decision_t result = {0};
    if (!clock->audio_valid) return result;
    if (now_us < clock->audio_wall_us || now_us - clock->audio_wall_us > 1000000) {
        hosino_mjpeg_clock_reset(clock);
        result.reanchor_reason = HOSINO_MJPEG_ANCHOR_STALL;
        return result;
    }
    if (!clock->video_seen) {
        clock->video_seen = true;
        clock->video_pts_ms = pts_ms;
        return result;
    }
    if (pts_ms <= clock->video_pts_ms || pts_ms - clock->video_pts_ms > 1000U) {
        result.reanchor_reason = pts_ms <= clock->video_pts_ms
                                 ? HOSINO_MJPEG_ANCHOR_BACKWARD : HOSINO_MJPEG_ANCHOR_FORWARD;
        hosino_mjpeg_clock_reset(clock);
        return result;
    }
    clock->video_pts_ms = pts_ms;
    const uint64_t threshold_ms = frame_ms > 30U ? (uint64_t)frame_ms * 2U : 60U;
    const uint64_t elapsed_ms = now_us >= clock->anchor_wall_us
                                ? (uint64_t)(now_us - clock->anchor_wall_us) / 1000U : 0;
    const uint64_t wall_pts = UINT64_MAX - clock->anchor_pts_ms < elapsed_ms
                             ? UINT64_MAX : clock->anchor_pts_ms + elapsed_ms;
    const uint64_t audio_late = clock->audio_pts_ms > pts_ms ? clock->audio_pts_ms - pts_ms : 0;
    const uint64_t wall_late = wall_pts > pts_ms ? wall_pts - pts_ms : 0;
    result.late_ms = audio_late > wall_late ? audio_late : wall_late;
    if (result.late_ms > 1000U) {
        hosino_mjpeg_clock_reset(clock);
        result.reanchor_reason = HOSINO_MJPEG_ANCHOR_TOO_LATE;
    } else if (audio_late > threshold_ms) {
        result.drop_audio = true;
        result.late_ms = audio_late;
    } else if (wall_late > threshold_ms) {
        result.drop_wall = true;
        result.late_ms = wall_late;
    }
    return result;
}
