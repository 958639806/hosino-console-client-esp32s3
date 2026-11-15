#include <assert.h>
#include <stdio.h>
#include "sync_stubs.h"
#include "player_sync.h"
#define HOSINO_PLAYER_DIAGNOSTICS_PROVIDER 1
#include "hosino_player_diagnostics.h"

int64_t hosino_test_now;
static unsigned drop_count, pcm_gap_count, pcm_sync_drop_count;
static hosino_player_clock_sample_t latest_clock;
void hosino_player_clock_record(const hosino_player_clock_sample_t *sample)
{ latest_clock = *sample; }
void hosino_player_diagnostics_record(hosino_player_diag_event_t e,uint32_t us,uint32_t amount,int result)
{
    (void)us;(void)amount;(void)result;
    if(e==HOSINO_PLAYER_DIAG_DROP_LATE_WALL)++drop_count;
    if(e==HOSINO_PLAYER_DIAG_AUDIO_SYNC_GAP_ACCEPTED)++pcm_gap_count;
    if(e==HOSINO_PLAYER_DIAG_AUDIO_SYNC_DROP)++pcm_sync_drop_count;
}

static void test_audio_master_gap_recovery(void)
{
    esp_player_stream_t stream = {0};
    player_sync_config_t cfg = PLAYER_SYNC_CONFIG_DEFAULT();
    cfg.bit_ctx = &stream;
    player_sync_handle_t sync = NULL;
    assert(player_sync_create(&cfg, &sync) == ESP_PLAYER_ERR_OK);
    hosino_test_now = 0;
    pcm_gap_count = pcm_sync_drop_count = 0;
    const uint64_t pts[] = {1000,1020,1040,4000,4020,4040,100,120,140};
    unsigned committed = 0;
    for (unsigned i = 0; i < sizeof(pts) / sizeof(pts[0]); ++i) {
        const uint32_t generation = player_sync_audio_output_generation(sync);
        assert(player_sync_audio_render_frame(sync, pts[i]));
        /* Model successful PCM delivery at the actual production commit hook.
         * This does not claim an I2S write or audible playback on the host. */
        player_sync_note_audio_output(sync, pts[i], generation);
        ++committed;
        assert(player_sync_local_mjpeg_decode_frame(sync, pts[i], 30));
        assert(latest_clock.audio_pts_ms == pts[i]);
        if (i == 0 || i == 3 || i == 6) {
            assert(latest_clock.anchor_pts_ms == pts[i]);
            assert(latest_clock.anchor_elapsed_ms == 0);
        }
        hosino_test_now += 20000;
    }
    assert(committed == 9);
    assert(pcm_gap_count == 1 && pcm_sync_drop_count == 0);
    /* Admitting a PCM block must NOT commit an audio clock before a successful
     * sink write. Failed writes omit note_audio_output, just as production. */
    assert(player_sync_audio_render_frame(sync, 8000));
    assert(pcm_gap_count == 2);
    assert(player_sync_local_mjpeg_decode_frame(sync, 160, 30));
    assert(latest_clock.audio_pts_ms == 140);
    assert(latest_clock.anchor_pts_ms == 100);
    player_sync_note_audio_output(sync, 8020, player_sync_audio_output_generation(sync));
    assert(player_sync_local_mjpeg_decode_frame(sync, 8020, 30));
    assert(latest_clock.audio_pts_ms == 8020 && latest_clock.anchor_pts_ms == 8020);
    /* Seek and stop still reject delivery; an old generation cannot anchor a
     * new epoch even if a previously acquired write returns after seek. */
    uint32_t old_generation = player_sync_audio_output_generation(sync);
    player_sync_set_seek_in_progress(sync, true);
    assert(!player_sync_audio_render_frame(sync, 9000));
    player_sync_reset(sync);
    player_sync_set_seek_in_progress(sync, false);
    player_sync_set_render_pts(sync, 9000);
    player_sync_note_audio_output(sync, 8000, old_generation);
    assert(player_sync_local_mjpeg_decode_frame(sync, 9000, 30));
    assert(!(latest_clock.flags & HOSINO_PLAYER_CLOCK_AUDIO_VALID));
    assert(player_sync_audio_render_frame(sync, 9000));
    player_sync_note_audio_output(sync, 9000, player_sync_audio_output_generation(sync));
    assert(player_sync_local_mjpeg_decode_frame(sync, 9000, 30));
    assert(latest_clock.anchor_pts_ms == 9000);
    stream._is_stop = true;
    assert(!player_sync_audio_render_frame(sync, 9020));
    stream._is_stop = false;
    old_generation = player_sync_audio_output_generation(sync);
    player_sync_pause(sync);
    player_sync_resume(sync);
    player_sync_note_audio_output(sync, 9020, old_generation);
    assert(player_sync_local_mjpeg_decode_frame(sync, 9020, 30));
    assert(!(latest_clock.flags & HOSINO_PLAYER_CLOCK_AUDIO_VALID));
    assert(player_sync_audio_render_frame(sync, 9040));
    player_sync_note_audio_output(sync, 9040, player_sync_audio_output_generation(sync));
    assert(player_sync_local_mjpeg_decode_frame(sync, 9040, 30));
    assert(latest_clock.anchor_pts_ms == 9040);
    assert(player_sync_destroy(sync) == ESP_PLAYER_ERR_OK);
    /* Other master modes retain their prior delay rejection; NONE accepts. */
    cfg.sync_mode = ESP_PLAYER_SYNC_MODE_VIDEO;
    assert(player_sync_create(&cfg, &sync) == ESP_PLAYER_ERR_OK);
    player_sync_set_render_pts(sync, 100);
    assert(!player_sync_audio_render_frame(sync, 4000));
    assert(player_sync_destroy(sync) == ESP_PLAYER_ERR_OK);
    cfg.sync_mode = ESP_PLAYER_SYNC_MODE_SYSTEM;
    assert(player_sync_create(&cfg, &sync) == ESP_PLAYER_ERR_OK);
    player_sync_set_render_pts(sync, 100);
    hosino_test_now = 4000000;
    assert(!player_sync_audio_render_frame(sync, 4000));
    assert(player_sync_destroy(sync) == ESP_PLAYER_ERR_OK);
    cfg.sync_mode = ESP_PLAYER_SYNC_MODE_NONE;
    assert(player_sync_create(&cfg, &sync) == ESP_PLAYER_ERR_OK);
    assert(player_sync_audio_render_frame(sync, 100));
    assert(player_sync_audio_render_frame(sync, 8000));
    assert(player_sync_destroy(sync) == ESP_PLAYER_ERR_OK);
    assert(!player_sync_audio_render_frame(NULL, 1));
    assert(pcm_sync_drop_count == 2);
}

int main(void)
{
    esp_player_stream_t stream={0};
    player_sync_config_t cfg=PLAYER_SYNC_CONFIG_DEFAULT();
    cfg.bit_ctx=&stream;
    player_sync_handle_t s=NULL;
    assert(player_sync_create(&cfg,&s)==ESP_PLAYER_ERR_OK);
    player_sync_set_video_fps(s,30);
    /* The actual generic AUDIO function must keep its prior accept behavior:
     * zero start, large PTS gaps, network/H264 callers never enter MJPEG policy. */
    assert(player_sync_video_decode_frame(s,0));
    assert(player_sync_video_decode_frame(s,50000));
    assert(player_sync_video_decode_frame(s,1));
    assert(player_sync_local_mjpeg_decode_frame(s,0,30)); /* no audio anchor */
    uint32_t epoch=player_sync_audio_output_generation(s);
    player_sync_note_audio_output(s,0,epoch);
    assert(player_sync_local_mjpeg_decode_frame(s,0,30));
    hosino_test_now=250000;
    assert(!player_sync_local_mjpeg_decode_frame(s,33,30));
    assert(drop_count==1);

    /* Execute real pause/resume entry points; old acquired PCM is rejected. */
    epoch=player_sync_audio_output_generation(s);
    player_sync_pause(s);
    hosino_test_now=5000000;
    player_sync_resume(s);
    player_sync_note_audio_output(s,20,epoch);
    assert(player_sync_local_mjpeg_decode_frame(s,66,30));
    epoch=player_sync_audio_output_generation(s);
    player_sync_note_audio_output(s,60,epoch);
    assert(player_sync_local_mjpeg_decode_frame(s,66,30));
    hosino_test_now+=33000;
    assert(player_sync_local_mjpeg_decode_frame(s,99,30));

    /* EOS, stop, speeds other than one, and mode changes cannot force skips. */
    epoch=player_sync_audio_output_generation(s);
    player_sync_audio_output_finished(s);
    player_sync_note_audio_output(s,80,epoch);
    hosino_test_now+=500000;
    assert(player_sync_local_mjpeg_decode_frame(s,132,30));
    player_sync_set_speed(s,2.0f);
    player_sync_note_audio_output(s,100,player_sync_audio_output_generation(s));
    assert(player_sync_local_mjpeg_decode_frame(s,165,30));
    player_sync_set_speed(s,1.0f);
    stream._is_stop=true;
    assert(player_sync_local_mjpeg_decode_frame(s,198,30));
    stream._is_stop=false;

    /* Exercise the same seek/reset/set-render-PTS ordering as the player. */
    epoch=player_sync_audio_output_generation(s);
    player_sync_set_seek_in_progress(s,true);
    player_sync_reset(s);
    player_sync_set_video_fps(s,30);
    player_sync_note_audio_output(s,200,epoch);
    assert(player_sync_local_mjpeg_decode_frame(s,10000,30));
    player_sync_set_seek_in_progress(s,false);
    player_sync_set_render_pts(s,10000);
    player_sync_note_audio_output(s,200,epoch);
    assert(player_sync_local_mjpeg_decode_frame(s,10000,30));
    player_sync_note_audio_output(s,10000,player_sync_audio_output_generation(s));
    assert(player_sync_local_mjpeg_decode_frame(s,10000,30));
    hosino_test_now+=33000;
    assert(player_sync_local_mjpeg_decode_frame(s,10033,30));
    player_sync_set_mode(s,ESP_PLAYER_SYNC_MODE_NONE);
    hosino_test_now+=500000;
    assert(player_sync_local_mjpeg_decode_frame(s,10066,30));
    assert(player_sync_destroy(s)==ESP_PLAYER_ERR_OK);

    /* Actual AVI setup never sets generic FPS pacing. The local policy must
     * still use track23 => 2*43ms, while an unknown FPS keeps the60ms fallback. */
    assert(player_sync_create(&cfg,&s)==ESP_PLAYER_ERR_OK);
    hosino_test_now=0;
    player_sync_note_audio_output(s,0,player_sync_audio_output_generation(s));
    assert(player_sync_local_mjpeg_decode_frame(s,0,23));
    assert(latest_clock.decision==HOSINO_PLAYER_CLOCK_FIRST_FRAME);
    hosino_test_now=122000;
    assert(player_sync_local_mjpeg_decode_frame(s,43,23));
    assert(latest_clock.threshold_ms==86 && latest_clock.source_fps==23);
    assert(latest_clock.anchor_elapsed_ms==122 && latest_clock.audio_pts_ms==0);
    assert(latest_clock.decision==HOSINO_PLAYER_CLOCK_KEEP_ON_TIME);
    hosino_test_now=174000;
    assert(!player_sync_local_mjpeg_decode_frame(s,86,23));
    assert(latest_clock.decision==HOSINO_PLAYER_CLOCK_DROP_WALL);
    hosino_test_now=1300000;
    assert(player_sync_local_mjpeg_decode_frame(s,129,23));
    assert(latest_clock.decision==HOSINO_PLAYER_CLOCK_REANCHOR);
    assert(latest_clock.reanchor_reason==3);
    assert(latest_clock.flags&HOSINO_PLAYER_CLOCK_AUDIO_VALID); /* before reset */
    assert(latest_clock.anchor_elapsed_ms==1300);
    player_sync_note_audio_output(s,0,player_sync_audio_output_generation(s));
    assert(player_sync_local_mjpeg_decode_frame(s,0,0));
    hosino_test_now+=94000;
    assert(!player_sync_local_mjpeg_decode_frame(s,33,0));
    assert(latest_clock.threshold_ms==60 && latest_clock.source_fps==0);
    assert(player_sync_destroy(s)==ESP_PLAYER_ERR_OK);
    test_audio_master_gap_recovery();
    puts("PASS: audio master forward/backward gap and PCM commit recovery; production player_sync.c lifecycle/generic AUDIO plus actual track FPS and pre-decision clock diagnostics");
    return 0;
}
