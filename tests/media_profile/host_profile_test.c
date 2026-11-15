/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HOSINO_PLAYER_DIAGNOSTICS_PROVIDER 1
#define HOSINO_FILE_DIAGNOSTICS_PROVIDER 1
#include "media_profile.h"
#include "esp_video_dec.h"

static int critical_depth;
static int64_t clock_us;
static int64_t decode_elapsed;
static int decode_result, decode_transition;

void host_enter(void) { assert(critical_depth == 0); ++critical_depth; }
void host_exit(void) { assert(critical_depth == 1); --critical_depth; }
int64_t esp_timer_get_time(void) { return clock_us; }
size_t host_strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t copied = n < size - 1 ? n : size - 1;
        memcpy(dst, src, copied); dst[copied] = 0;
    }
    return n;
}
esp_vc_err_t __real_esp_video_dec_process(esp_video_dec_handle_t h,
                                        esp_video_dec_in_frame_t *in,
                                        esp_video_dec_out_frame_t *out)
{
    (void)h; (void)in; (void)out;
    assert(critical_depth == 0);
    clock_us += decode_elapsed;
    if (decode_transition == 1) hosino_media_profile_reset("/sdcard/clip.avi");
    if (decode_transition == 2) hosino_media_profile_stop();
    return decode_result;
}
extern esp_vc_err_t __wrap_esp_video_dec_process(esp_video_dec_handle_t,
    esp_video_dec_in_frame_t *, esp_video_dec_out_frame_t *);

static hosino_media_profile_snapshot_t take(void)
{
    hosino_media_profile_snapshot_t s;
    memset(&s, 0xA5, sizeof(s));
    hosino_media_profile_take(&s);
    assert(critical_depth == 0);
    return s;
}
static void assert_empty(hosino_media_profile_snapshot_t s)
{
    hosino_media_profile_snapshot_t zero = {0};
    assert(memcmp(&s, &zero, sizeof(s)) == 0);
}
static void test_exact_binding(void)
{
    char path[] = "/sdcard/clip.avi";
    hosino_media_profile_stop();
    assert(!hosino_file_diagnostics_bind(path));
    hosino_media_profile_reset(path);
    uint32_t token = hosino_file_diagnostics_bind(path);
    assert(token);
    assert(!hosino_file_diagnostics_bind(NULL));
    assert(!hosino_file_diagnostics_bind("/sdcard/clip.avi.extra"));
    assert(!hosino_file_diagnostics_bind("/sdcard/CLIP.avi"));
    path[8] = 'X';
    assert(hosino_file_diagnostics_bind("/sdcard/clip.avi") == token);
    assert(!hosino_file_diagnostics_bind(path));
    assert_empty(take());
    puts("PASS exact path binding and copied pathname");
}
static void test_read_seek_thresholds(void)
{
    static const uint32_t elapsed[] = {0, 19999, 20000, 20001, 40000, 40001, 80000, 80001, UINT32_MAX};
    hosino_media_profile_reset("/sdcard/clip.avi");
    uint32_t t = hosino_file_diagnostics_bind("/sdcard/clip.avi");
    for (unsigned i = 0; i < 9; ++i)
        hosino_file_diagnostics_record(t, HOSINO_FILE_DIAG_READ, elapsed[i],
            i == 8 ? UINT32_MAX : 100, i == 2 ? EIO : 0, i == 3 || i == 8);
    hosino_file_diagnostics_record(t, HOSINO_FILE_DIAG_SEEK, 40001, 0, EINVAL, false);
    hosino_file_diagnostics_record(0, HOSINO_FILE_DIAG_READ, 9, 9, 9, true);
    hosino_file_diagnostics_record(t, (hosino_file_diag_event_t)-1, 9, 9, 9, true);
    hosino_file_diagnostics_record(t, HOSINO_FILE_DIAG_EVENT_COUNT, 9, 9, 9, true);
    hosino_media_profile_take(NULL); /* Must not consume the interval. */
    hosino_media_profile_snapshot_t s = take();
    hosino_media_file_sample_t f = s.file[HOSINO_FILE_DIAG_READ];
    assert(f.count == 9 && f.errors == 1 && f.eof == 2);
    assert(f.over_20ms == 6 && f.over_40ms == 4 && f.over_80ms == 2);
    assert(f.total_us == UINT64_C(4295267297) && f.bytes == UINT64_C(4294968095));
    assert(f.max_us == UINT32_MAX);
    f = s.file[HOSINO_FILE_DIAG_SEEK];
    assert(f.count == 1 && f.errors == 1 && !f.eof && !f.bytes);
    assert(f.total_us == 40001 && f.over_20ms == 1 && f.over_40ms == 1 && !f.over_80ms);
    assert_empty(take());
    assert(hosino_file_diagnostics_bind("/sdcard/clip.avi") == t);
    puts("PASS read/seek counters, EOF/errors, threshold boundaries, 64-bit totals and take");
}
static void test_stale_generations(void)
{
    hosino_media_profile_reset("/sdcard/clip.avi");
    uint32_t old = hosino_file_diagnostics_bind("/sdcard/clip.avi");
    hosino_media_profile_stop();
    hosino_file_diagnostics_record(old, HOSINO_FILE_DIAG_READ, 100, 100, 0, false);
    assert_empty(take());
    hosino_media_profile_reset("/sdcard/clip.avi");
    uint32_t current = hosino_file_diagnostics_bind("/sdcard/clip.avi");
    assert(current && current != old);
    hosino_file_diagnostics_record(old, HOSINO_FILE_DIAG_READ, 100, 100, 0, false);
    hosino_file_diagnostics_record(current, HOSINO_FILE_DIAG_READ, 7, 13, 0, false);
    hosino_media_profile_snapshot_t s = take();
    assert(s.file[HOSINO_FILE_DIAG_READ].count == 1 && s.file[HOSINO_FILE_DIAG_READ].bytes == 13);
    hosino_media_profile_reset("/sdcard/other.avi");
    assert(!hosino_file_diagnostics_bind("/sdcard/clip.avi"));
    hosino_file_diagnostics_record(current, HOSINO_FILE_DIAG_SEEK, 100, 0, 1, false);
    assert_empty(take());
    hosino_media_profile_reset(NULL);
    assert(!hosino_file_diagnostics_bind("/sdcard/other.avi"));
    assert(!hosino_file_diagnostics_bind(""));
    puts("PASS stopped/stale tokens, same-path reopen and null reset");
}
static void test_decode_generation_and_clock(void)
{
    hosino_media_profile_reset("/sdcard/clip.avi");
    decode_result = ESP_VC_ERR_OK; decode_elapsed = 17; decode_transition = 1;
    assert(__wrap_esp_video_dec_process(NULL, NULL, NULL) == ESP_VC_ERR_OK);
    assert_empty(take()); /* Decoder crossed session reset. */
    decode_transition = 2;
    __wrap_esp_video_dec_process(NULL, NULL, NULL);
    assert_empty(take());
    hosino_media_profile_reset("/sdcard/clip.avi");
    decode_transition = 0; decode_elapsed = 17;
    __wrap_esp_video_dec_process(NULL, NULL, NULL);
    decode_elapsed = -7;
    __wrap_esp_video_dec_process(NULL, NULL, NULL);
    decode_elapsed = (int64_t)UINT32_MAX + 500;
    __wrap_esp_video_dec_process(NULL, NULL, NULL);
    decode_result = -7; decode_elapsed = 25;
    assert(__wrap_esp_video_dec_process(NULL, NULL, NULL) == -7);
    hosino_media_profile_snapshot_t s = take();
    assert(s.decode_ok == 3 && s.decode_error == 1);
    assert(s.decode_total_us == (uint64_t)UINT32_MAX + 17 && s.decode_max_us == UINT32_MAX);
    puts("PASS real decoder wrapper, in-flight generation changes and elapsed clamps");
}
static void test_other_metrics_and_stop(void)
{
    hosino_media_profile_reset("/sdcard/clip.avi");
    hosino_player_diagnostics_record(HOSINO_PLAYER_DIAG_VIDEO_QUEUE_PUSH, 33, 100, 0);
    hosino_player_diagnostics_record(HOSINO_PLAYER_DIAG_VIDEO_QUEUE_PUSH, 99, 200, -1);
    hosino_player_diagnostics_record(HOSINO_PLAYER_DIAG_DEMUX_VIDEO_PACKET, 0, 42000, 0);
    hosino_player_diagnostics_record(HOSINO_PLAYER_DIAG_DEMUX_VIDEO_PACKET, 0, 71000, 0);
    hosino_player_diagnostics_record(HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY, 0, 42000, 0);
    hosino_player_diagnostics_record(HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY, 0, 71000, -1);
    hosino_player_diagnostics_record((hosino_player_diag_event_t)-1, 1, 1, 0);
    hosino_media_profile_record_video_sink(90);
    hosino_media_profile_record_audio_write(30, UINT32_MAX, 1000, 1000, false);
    hosino_media_profile_record_audio_write(40, 51, 200, 300, true);
    hosino_media_profile_snapshot_t s = take();
    assert(s.pipeline[HOSINO_PLAYER_DIAG_VIDEO_QUEUE_PUSH].count == 2);
    assert(s.pipeline[HOSINO_PLAYER_DIAG_VIDEO_QUEUE_PUSH].non_ok == 1);
    assert(s.pipeline[HOSINO_PLAYER_DIAG_VIDEO_QUEUE_PUSH].total_us == 132);
    assert(s.pipeline[HOSINO_PLAYER_DIAG_VIDEO_QUEUE_PUSH].amount == 300);
    assert(s.pipeline[HOSINO_PLAYER_DIAG_VIDEO_QUEUE_PUSH].amount_max == 200);
    assert(s.pipeline[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_PACKET].amount_max == 71000);
    assert(s.pipeline[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_PACKET].amount == 113000);
    assert(s.pipeline[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY].count == 2);
    assert(s.pipeline[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY].non_ok == 1);
    assert(s.video_sink_count == 1 && s.video_sink_total_us == 90);
    assert(s.audio_write_count == 2 && s.audio_write_total_us == 70 && s.audio_written_bytes == 1200);
    assert(s.audio_short_writes == 1 && s.audio_write_errors == 1);
    assert(s.audio_gap_count == 1 && s.audio_gap_total_us == 51);
    hosino_media_profile_stop();
    hosino_player_diagnostics_record(HOSINO_PLAYER_DIAG_VIDEO_QUEUE_PUSH, 33, 100, 0);
    hosino_media_profile_record_video_sink(90);
    hosino_media_profile_record_audio_write(1, 1, 1, 1, true);
    decode_result = ESP_VC_ERR_OK;
    __wrap_esp_video_dec_process(NULL, NULL, NULL);
    assert_empty(take());
    puts("PASS pipeline/audio metrics and disabled-session suppression");
}
static void test_clock_snapshot_lifetime(void)
{
    hosino_player_clock_sample_t sample = {
        .video_pts_ms = UINT64_C(0x100000007),
        .audio_pts_ms = UINT64_C(0x200000009),
        .anchor_pts_ms = UINT64_C(0x30000000b),
        .anchor_elapsed_ms = UINT64_C(0x40000000d),
        .source_fps = 15, .threshold_ms = 134, .flags = 0xA5,
        .decision = HOSINO_PLAYER_CLOCK_DROP_WALL, .reanchor_reason = 5,
    };
    hosino_media_profile_stop();
    hosino_player_clock_record(&sample);
    hosino_player_clock_record(NULL);
    assert_empty(take());
    hosino_media_profile_reset("/sdcard/clip.avi");
    hosino_player_clock_record(NULL);
    hosino_player_clock_record(&sample);
    sample.video_pts_ms = UINT64_C(0x50000000f);
    sample.source_fps = 30; sample.threshold_ms = 68;
    sample.decision = HOSINO_PLAYER_CLOCK_KEEP_ON_TIME;
    hosino_player_clock_record(&sample);
    memset(&sample, 0, sizeof(sample)); /* Caller memory can immediately expire. */
    hosino_media_profile_take(NULL);
    hosino_media_profile_snapshot_t s = take();
    assert(s.clock_samples == 2);
    assert(s.last_clock.video_pts_ms == UINT64_C(0x50000000f));
    assert(s.last_clock.audio_pts_ms == UINT64_C(0x200000009));
    assert(s.last_clock.anchor_pts_ms == UINT64_C(0x30000000b));
    assert(s.last_clock.anchor_elapsed_ms == UINT64_C(0x40000000d));
    assert(s.last_clock.source_fps == 30 && s.last_clock.threshold_ms == 68);
    assert(s.last_clock.flags == 0xA5 && s.last_clock.reanchor_reason == 5);
    assert(s.last_clock.decision == HOSINO_PLAYER_CLOCK_KEEP_ON_TIME);
    assert_empty(take());
    hosino_media_profile_stop();
    hosino_player_clock_record(&s.last_clock);
    assert_empty(take());
    hosino_media_profile_reset("/sdcard/other.avi");
    assert_empty(take());
    puts("PASS clock hook snapshot ownership, count/take, null and disabled behavior");
}
int main(void)
{
    test_exact_binding(); test_read_seek_thresholds(); test_stale_generations();
    test_decode_generation_and_clock(); test_other_metrics_and_stop();
    test_clock_snapshot_lifetime();
    puts("ALL 6 REAL MEDIA_PROFILE TEST GROUPS PASSED");
    return 0;
}
