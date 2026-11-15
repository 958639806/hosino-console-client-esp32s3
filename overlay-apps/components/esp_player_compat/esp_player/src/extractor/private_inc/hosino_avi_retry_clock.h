/* SPDX-FileCopyrightText: 2026 Hosino contributors
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Read only the header, using a second short-lived FILE so the demuxer's cache
 * position and payload ownership are never changed. Malformed/large headers
 * simply leave the vendor timestamps in use. */
#define HOSINO_AVI_HEADER_LIMIT (64U * 1024U)
#define HOSINO_AVI_CHUNK_LIMIT (512U)

typedef struct {
    uint32_t scale;
    uint32_t rate;
    uint16_t stream_id;
} hosino_avi_video_rate_t;

static inline uint32_t hosino_avi_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline bool hosino_avi_read_at(FILE *file, uint32_t pos, uint8_t *out, uint32_t bytes)
{
    return pos <= HOSINO_AVI_HEADER_LIMIT && bytes <= HOSINO_AVI_HEADER_LIMIT - pos &&
           fseek(file, (long)pos, SEEK_SET) == 0 && fread(out, 1, bytes, file) == bytes;
}

/* A strl's stream ordinal is its AVI stream id. The selected video ordinal is
 * independent of intervening audio streams. All chunk arithmetic is widened. */
static inline bool hosino_avi_read_video_rate(FILE *file, uint16_t selected_video,
                                             hosino_avi_video_rate_t *out)
{
    uint8_t head[32];
    if (!file || !out || !hosino_avi_read_at(file, 0, head, 12) ||
        memcmp(head, "RIFF", 4) || memcmp(head + 8, "AVI ", 4)) return false;
    const uint64_t riff_end = (uint64_t)hosino_avi_u32(head + 4) + 8U;
    uint32_t budget = HOSINO_AVI_CHUNK_LIMIT;
    uint32_t pos = 12;
    uint32_t hdrl_end = 0;
    while (budget && (uint64_t)pos + 12U <= riff_end) {
        budget--;
        if (!hosino_avi_read_at(file, pos, head, 12)) return false;
        const uint32_t size = hosino_avi_u32(head + 4);
        const uint64_t end = (uint64_t)pos + 8U + size;
        if (end > riff_end || end <= pos) return false;
        if (!memcmp(head, "LIST", 4) && size >= 4 && !memcmp(head + 8, "hdrl", 4)) {
            if (end > HOSINO_AVI_HEADER_LIMIT) return false;
            hdrl_end = (uint32_t)end;
            pos += 12;
            break;
        }
        if (end + (size & 1U) > HOSINO_AVI_HEADER_LIMIT) return false;
        pos = (uint32_t)(end + (size & 1U));
    }
    if (!hdrl_end) return false;
    uint16_t stream_id = 0;
    uint16_t video_ordinal = 0;
    while (budget && (uint64_t)pos + 8U <= hdrl_end) {
        budget--;
        if (!hosino_avi_read_at(file, pos, head, 8)) return false;
        const uint32_t size = hosino_avi_u32(head + 4);
        const uint64_t end = (uint64_t)pos + 8U + size;
        if (end > hdrl_end || end <= pos) return false;
        if (!memcmp(head, "LIST", 4) && size >= 4) {
            if (!hosino_avi_read_at(file, pos + 8, head, 4)) return false;
            if (!memcmp(head, "strl", 4)) {
                uint32_t child = pos + 12;
                bool found_header = false;
                while (budget && (uint64_t)child + 8U <= end) {
                    budget--;
                    if (!hosino_avi_read_at(file, child, head, 8)) return false;
                    const uint32_t child_size = hosino_avi_u32(head + 4);
                    const uint64_t child_end = (uint64_t)child + 8U + child_size;
                    if (child_end > end || child_end <= child) return false;
                    if (!memcmp(head, "strh", 4)) {
                        if (found_header || child_size < 32 ||
                            !hosino_avi_read_at(file, child + 8, head, 32)) return false;
                        found_header = true;
                        if (!memcmp(head, "vids", 4)) {
                            if (video_ordinal++ == selected_video) {
                                const uint32_t scale = hosino_avi_u32(head + 20);
                                const uint32_t rate = hosino_avi_u32(head + 24);
                                /* Keep the workaround specific to normal MJPEG
                                 * video, not unusual time bases or other codecs. */
                                if (memcmp(head + 4, "MJPG", 4) || !scale || !rate ||
                                    rate < scale || (uint64_t)rate > (uint64_t)scale * 240U) return false;
                                *out = (hosino_avi_video_rate_t){scale, rate, stream_id};
                                return true;
                            }
                        }
                    }
                    child = (uint32_t)(child_end + (child_size & 1U));
                }
                if (!found_header || stream_id == UINT16_MAX) return false;
                stream_id++;
            }
        }
        pos = (uint32_t)(end + (size & 1U));
    }
    return false;
}

typedef struct {
    hosino_avi_video_rate_t timebase;
    uint32_t wait_count;
    uint32_t pending_count;
    uint32_t pending_pos;
    uint32_t pending_size;
    uint32_t pending_pts;
    uint32_t last_raw_pts;
    uint32_t last_corrected_pts;
    bool enabled;
} hosino_avi_retry_clock_t;

static inline void hosino_avi_retry_clear(hosino_avi_retry_clock_t *clock)
{
    *clock = (hosino_avi_retry_clock_t){0};
}

static inline void hosino_avi_retry_seek_reset(hosino_avi_retry_clock_t *clock)
{
    const hosino_avi_video_rate_t timebase = clock->timebase;
    const bool enabled = clock->enabled;
    *clock = (hosino_avi_retry_clock_t){.timebase = timebase, .enabled = enabled};
}

/* Quotient/remainder avoids wait_count * scale * 1000 overflow. The remainder
 * product is at most (UINT32_MAX)^2 and therefore fits uint64_t. */
static inline uint64_t hosino_avi_retry_bias(const hosino_avi_retry_clock_t *clock,
                                           uint32_t count)
{
    const uint64_t period = (uint64_t)clock->timebase.scale * 1000U;
    return (uint64_t)count * (period / clock->timebase.rate) +
           ((uint64_t)count * (period % clock->timebase.rate)) / clock->timebase.rate;
}

/* The pinned AVI parser advances the video frame counter during preview,
 * before allocation. A WAITING_OUTPUT retry previews the identical chunk again.
 * Count only matching selected-video metadata and no buffer ownership. */
static inline bool hosino_avi_retry_wait(hosino_avi_retry_clock_t *clock,
                                       uint16_t stream_id, uint32_t pos,
                                       uint32_t size, uint32_t pts, bool has_buffer)
{
    if (!clock->enabled || stream_id != clock->timebase.stream_id) return true;
    if (!size || has_buffer || clock->pending_count == UINT32_MAX ||
        clock->wait_count > UINT32_MAX - clock->pending_count - 1U ||
        (clock->pending_count && (pos != clock->pending_pos || size != clock->pending_size ||
                                  pts < clock->pending_pts))) {
        clock->enabled = false;
        return false;
    }
    if (!clock->pending_count) {
        clock->pending_pos = pos;
        clock->pending_size = size;
        clock->pending_pts = pts;
    }
    clock->pending_count++;
    return true;
}

/* Do not sum independently rounded per-batch offsets. Subtracting the single
 * cumulative rational bias differs from the native no-retry PTS by at most 1ms,
 * even at 24000/1001 fps or after thousands of retries. Successful frames that
 * are intentionally shed by the audio-fairness policy still advance normally. */
static inline bool hosino_avi_retry_correct(hosino_avi_retry_clock_t *clock,
                                          uint16_t stream_id, uint32_t pos,
                                          uint32_t size, uint32_t *pts)
{
    if (!clock->enabled || stream_id != clock->timebase.stream_id) return true;
    const uint32_t raw = *pts;
    if (clock->pending_count) {
        const uint64_t expected = hosino_avi_retry_bias(clock, clock->pending_count);
        const uint64_t actual = raw >= clock->pending_pts ? raw - clock->pending_pts : UINT64_MAX;
        if (pos != clock->pending_pos || size != clock->pending_size ||
            actual == UINT64_MAX || (actual > expected ? actual - expected : expected - actual) > 1U) {
            clock->enabled = false;
            return false;
        }
        clock->wait_count += clock->pending_count;
        clock->pending_count = 0;
    }
    const uint64_t bias = hosino_avi_retry_bias(clock, clock->wait_count);
    if (bias > raw) {
        clock->enabled = false;
        return false;
    }
    *pts = raw - (uint32_t)bias;
    clock->last_raw_pts = raw;
    clock->last_corrected_pts = *pts;
    return true;
}
