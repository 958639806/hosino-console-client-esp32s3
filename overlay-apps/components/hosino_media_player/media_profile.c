/* SPDX-FileCopyrightText: 2026 Hosino contributors
 * SPDX-License-Identifier: Apache-2.0 */
#define HOSINO_PLAYER_DIAGNOSTICS_PROVIDER 1
#define HOSINO_FILE_DIAGNOSTICS_PROVIDER 1
#include "media_profile.h"
#include "hosino_media_player.h"

#include <string.h>

#include "esp_timer.h"
#include "esp_video_dec.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_profile_lock = portMUX_INITIALIZER_UNLOCKED;
static hosino_media_profile_snapshot_t s_profile;
static uint32_t s_generation;
static bool s_enabled;
static char s_video_path[HOSINO_MEDIA_PATH_MAX];

esp_vc_err_t __real_esp_video_dec_process(esp_video_dec_handle_t handle,
                                         esp_video_dec_in_frame_t *in_frame,
                                         esp_video_dec_out_frame_t *out_frame);

static void add_sample(uint32_t *count, uint64_t *total_us, uint32_t *max_us,
                       uint32_t elapsed_us)
{
    ++*count;
    *total_us += elapsed_us;
    if (elapsed_us > *max_us) *max_us = elapsed_us;
}

void hosino_media_profile_reset(const char *exact_video_path)
{
    portENTER_CRITICAL(&s_profile_lock);
    if (++s_generation == 0) ++s_generation;
    s_enabled = true;
    strlcpy(s_video_path, exact_video_path ? exact_video_path : "", sizeof(s_video_path));
    memset(&s_profile, 0, sizeof(s_profile));
    portEXIT_CRITICAL(&s_profile_lock);
}

void hosino_media_profile_stop(void)
{
    portENTER_CRITICAL(&s_profile_lock);
    s_enabled = false;
    if (++s_generation == 0) ++s_generation;
    s_video_path[0] = 0;
    portEXIT_CRITICAL(&s_profile_lock);
}

uint32_t hosino_file_diagnostics_bind(const char *exact_path)
{
    if (!exact_path) return 0;
    portENTER_CRITICAL(&s_profile_lock);
    const uint32_t generation = s_enabled && s_video_path[0] &&
        strcmp(exact_path, s_video_path) == 0 ? s_generation : 0;
    portEXIT_CRITICAL(&s_profile_lock);
    return generation;
}

void hosino_file_diagnostics_record(uint32_t generation, hosino_file_diag_event_t event,
                                   uint32_t elapsed_us, uint32_t actual_bytes,
                                   int error_code, bool eof)
{
    if (!generation || (unsigned)event >= HOSINO_FILE_DIAG_EVENT_COUNT) return;
    portENTER_CRITICAL(&s_profile_lock);
    if (s_enabled && generation == s_generation) {
        hosino_media_file_sample_t *sample = &s_profile.file[event];
        add_sample(&sample->count, &sample->total_us, &sample->max_us, elapsed_us);
        sample->bytes += actual_bytes;
        if (error_code) ++sample->errors;
        if (eof) ++sample->eof;
        if (elapsed_us > 20000) ++sample->over_20ms;
        if (elapsed_us > 40000) ++sample->over_40ms;
        if (elapsed_us > 80000) ++sample->over_80ms;
    }
    portEXIT_CRITICAL(&s_profile_lock);
}

void hosino_player_diagnostics_record(hosino_player_diag_event_t event,
                                      uint32_t elapsed_us, uint32_t amount, int result)
{
    if ((unsigned)event >= HOSINO_PLAYER_DIAG_EVENT_COUNT) return;
    portENTER_CRITICAL(&s_profile_lock);
    if (s_enabled) {
        hosino_media_wait_sample_t *sample = &s_profile.pipeline[event];
        add_sample(&sample->count, &sample->total_us, &sample->max_us, elapsed_us);
        sample->amount += amount;
        if (amount > sample->amount_max) sample->amount_max = amount;
        // GMF IO_OK and the wait-only instrumentation both report zero.
        if (result != 0) ++sample->non_ok;
    }
    portEXIT_CRITICAL(&s_profile_lock);
}

void hosino_media_profile_take(hosino_media_profile_snapshot_t *out)
{
    if (out == NULL) return;
    // Keep the caller's possibly external-RAM destination outside the lock.
    hosino_media_profile_snapshot_t snapshot;
    portENTER_CRITICAL(&s_profile_lock);
    snapshot = s_profile;
    memset(&s_profile, 0, sizeof(s_profile));
    portEXIT_CRITICAL(&s_profile_lock);
    *out = snapshot;
}

void hosino_player_clock_record(const hosino_player_clock_sample_t *sample)
{
    if (!sample) return;
    portENTER_CRITICAL(&s_profile_lock);
    if (s_enabled) {
        ++s_profile.clock_samples;
        s_profile.last_clock = *sample;
    }
    portEXIT_CRITICAL(&s_profile_lock);
}

void hosino_media_profile_record_video_sink(uint32_t elapsed_us)
{
    portENTER_CRITICAL(&s_profile_lock);
    if (s_enabled) add_sample(&s_profile.video_sink_count, &s_profile.video_sink_total_us,
                              &s_profile.video_sink_max_us, elapsed_us);
    portEXIT_CRITICAL(&s_profile_lock);
}

void hosino_media_profile_record_audio_write(uint32_t elapsed_us, uint32_t gap_us,
                                            uint32_t written, uint32_t requested, bool failed)
{
    portENTER_CRITICAL(&s_profile_lock);
    if (s_enabled) {
        add_sample(&s_profile.audio_write_count, &s_profile.audio_write_total_us,
                   &s_profile.audio_write_max_us, elapsed_us);
        s_profile.audio_written_bytes += written;
        if (written != requested) ++s_profile.audio_short_writes;
        if (failed) ++s_profile.audio_write_errors;
        if (gap_us != UINT32_MAX) {
            add_sample(&s_profile.audio_gap_count, &s_profile.audio_gap_total_us,
                       &s_profile.audio_gap_max_us, gap_us);
        }
    }
    portEXIT_CRITICAL(&s_profile_lock);
}

esp_vc_err_t __wrap_esp_video_dec_process(esp_video_dec_handle_t handle,
                                         esp_video_dec_in_frame_t *in_frame,
                                         esp_video_dec_out_frame_t *out_frame)
{
    uint32_t generation;
    portENTER_CRITICAL(&s_profile_lock);
    generation = s_generation;
    portEXIT_CRITICAL(&s_profile_lock);

    const int64_t start_us = esp_timer_get_time();
    const esp_vc_err_t result = __real_esp_video_dec_process(handle, in_frame, out_frame);
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    const uint32_t bounded_us = elapsed_us <= 0 ? 0 :
        (elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us);

    portENTER_CRITICAL(&s_profile_lock);
    if (s_enabled && generation == s_generation) {
        if (result == ESP_VC_ERR_OK) {
            add_sample(&s_profile.decode_ok, &s_profile.decode_total_us,
                       &s_profile.decode_max_us, bounded_us);
        } else {
            ++s_profile.decode_error;
        }
    }
    portEXIT_CRITICAL(&s_profile_lock);
    return result;
}
