/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include "player_ports.h"
#include "player_events.h"
#include "player_submit_frame.h"
#include "player_url.h"
#include "hosino_player_diagnostics.h"
#include "hosino_mjpeg_clock.h"
#include "esp_gmf_io.h"
#include "esp_timer.h"
#include <strings.h>

#include "freertos/task.h"

/* Live TS often reports fps=0; without a floor, yield_ms is 0 and the extractor never yields. */
#define PLAYER_VIDEO_YIELD_FPS_FALLBACK  25U
#define PLAYER_VIDEO_YIELD_MIN_MS        20U
#define PLAYER_VIDEO_YIELD_NUM           70U
#define PLAYER_VIDEO_YIELD_DEN           100U

static const char *TAG = "ESP_PLAYER_PORTS";

static bool player_video_source_is_local_file(esp_player_stream_t *stream)
{
    if (stream->input_handle == NULL || stream->dec_frame_mode != ESP_PLAYER_DEC_FRAME_MODE_EXTRACTOR) {
        return false;
    }
    char *uri = NULL;
    if (esp_gmf_io_get_uri(stream->input_handle, &uri) != ESP_GMF_ERR_OK || uri == NULL) {
        return false;
    }
    /* File I/O accepts absolute VFS paths or a file URI. Do not treat unknown
     * or network schemes as local merely because they are not HTTP. */
    return uri[0] == '/' || strncasecmp(uri, "file://", 7) == 0 ||
           (stream->hosino_managed_mjpeg_http && strncasecmp(uri, "https://", 8) == 0);
}

esp_gmf_err_io_t extractor_video_out_release(void *handle, esp_gmf_payload_t *load, uint32_t wanted_size, int wait_ticks)
{
    ESP_LOGD(TAG, "extractor_video_out_release");
    esp_player_stream_t *stream = (esp_player_stream_t *)handle;
    (void)wanted_size;
    (void)wait_ticks;
    esp_gmf_err_io_t ret = player_ports_handle_stop_state(stream, load, "Video");
    if (ret != ESP_GMF_IO_OK) {
        return ret;
    }
    if (!stream->video_side || stream->video_side->frame_queue == NULL) {
        player_release_extractor_payload(stream, load);
        PLAYER_PORTS_EMPTY_LOAD(load);
        return ESP_GMF_IO_OK;
    }
    const uint32_t push_bytes = load->valid_size;
    const int64_t push_started = esp_timer_get_time();
    esp_gmf_err_io_t push_ret =
        player_ports_push_bounded(stream, stream->video_side->frame_queue, load, false);
    hosino_player_diagnostics_note(HOSINO_PLAYER_DIAG_VIDEO_QUEUE_PUSH,
                                  esp_timer_get_time() - push_started, push_bytes, push_ret);
    if (push_ret == ESP_GMF_IO_OK) {
        player_ports_buffer_note_queued(stream, false, load->pts);
        if (load->is_done) {
            player_ports_buffer_note_source_eos(stream);
        }
        PLAYER_PORTS_DETACH_BUF(load);
        esp_gmf_data_queue_t *aud_q = stream->audio_side ? stream->audio_side->frame_queue : NULL;
        bool aud_queue_idle = aud_q && (player_frame_queue_count(aud_q) == 0);
        const bool video_only = player_audio_track_idx(stream) < 0;
        if ((video_only || aud_queue_idle) && (!stream->is_seeking)) {
            const uint16_t fps = stream->video_side->track_info.video_info.fps;
            const uint32_t frame_period_ms = (fps > 0) ? (1000U / (uint32_t)fps) : (1000U / PLAYER_VIDEO_YIELD_FPS_FALLBACK);
            uint32_t yield_ms = frame_period_ms * PLAYER_VIDEO_YIELD_NUM / PLAYER_VIDEO_YIELD_DEN;
            /* Floor only when fps is unknown. A 60 fps source has a 16 ms period; clamping
             * to 20 ms would cap video-only extract at 50 fps. */
            if (fps == 0 && yield_ms < PLAYER_VIDEO_YIELD_MIN_MS) {
                yield_ms = PLAYER_VIDEO_YIELD_MIN_MS;
            }
            const bool local_av = !video_only && aud_queue_idle
                                  && stream->av_mask == ESP_PLAYER_MASK_AV
                                  && player_video_source_is_local_file(stream);
            if (local_av) {
                /* This demuxer supplies the missing audio too. Let local A/V
                 * read its next packet; bounded queues still apply backpressure. */
                hosino_player_diagnostics_note(HOSINO_PLAYER_DIAG_AV_YIELD_SKIPPED,
                                              0, yield_ms * 1000U, 0);
            } else {
                const int64_t yield_started = esp_timer_get_time();
                vTaskDelay(pdMS_TO_TICKS(yield_ms));
                const int64_t elapsed_us = esp_timer_get_time() - yield_started;
                if (video_only) {
                    hosino_player_diagnostics_note(HOSINO_PLAYER_DIAG_VIDEO_ONLY_YIELD,
                                                  elapsed_us, yield_ms * 1000U, 0);
                } else if (_player_is_network_source_uri(stream)) {
                    hosino_player_diagnostics_note(HOSINO_PLAYER_DIAG_AV_YIELD_NETWORK,
                                                  elapsed_us, yield_ms * 1000U, 0);
                }
            }
        }
        return ESP_GMF_IO_OK;
    }
    player_release_extractor_payload(stream, load);
    PLAYER_PORTS_EMPTY_LOAD(load);
    return push_ret;
}

esp_gmf_err_io_t decoder_video_in_acquire(void *handle, esp_gmf_payload_t *load, uint32_t wanted_size, int wait_ticks)
{
    esp_player_stream_t *stream = (esp_player_stream_t *)handle;
    uint32_t consecutive_drops = 0;
    /* esp_gmf_data_queue timeouts are milliseconds. */
    uint32_t recv_wait_ms = ESP_GMF_DATA_QUEUE_WAIT_FOREVER;
    if (stream->buffer_ctrl && stream->main_state == ESP_PLAYER_STATE_PLAYING
        && stream->buffer_ctrl->gate_state == ESP_PLAYER_BUFFER_GATE_NONE
        && _player_is_network_source_uri(stream)) {
        recv_wait_ms = 10;
    }
_rec_dec_video_in_frame:
    if (stream->_is_stop || stream->error_source == ESP_PLAYER_ERROR_SOURCE_EXTRACTOR || stream->error_source == ESP_PLAYER_ERROR_SOURCE_VIDEO_RENDER) {
        ESP_LOGD(TAG, "Video queue receive abort, line: %d", __LINE__);
        PLAYER_PORTS_EMPTY_LOAD(load);
        esp_gmf_db_handle_t vid_db = player_video_db(stream);
        if (vid_db) {
            esp_gmf_db_abort(vid_db);
        }
        return ESP_GMF_IO_ABORT;
    }
    if (player_ports_buffer_gate_wait(stream, false) == false) {
        goto _rec_dec_video_in_frame;
    }
    if (player_frame_queue_acquire(stream->video_side->frame_queue, &stream->video_side->read_node,
                                   load, recv_wait_ms) == ESP_GMF_IO_OK) {
        if (stream->is_seeking
            && load->pts >= player_sync_get_seek_target(stream->sync_handle)) {
            player_set_events(stream, _CTRL_PLAYER_DECODER_VIDEO_SEEK_DONE);
        }
        if (stream->_is_stop) {
            player_frame_queue_release(stream, stream->video_side->frame_queue,
                                       &stream->video_side->read_node);
            PLAYER_PORTS_EMPTY_LOAD(load);
            esp_gmf_db_handle_t vid_db = player_video_db(stream);
            if (vid_db) {
                esp_gmf_db_done_write(vid_db);
            }
            return ESP_GMF_IO_ABORT;
        }
        if (load->is_done == true) {
            esp_gmf_db_handle_t vid_db = player_video_db(stream);
            if (vid_db) {
                esp_gmf_db_done_write(vid_db);
            }
        }
        if (load->valid_size == 0) {
            player_frame_queue_release(stream, stream->video_side->frame_queue,
                                       &stream->video_side->read_node);
            return ESP_GMF_IO_OK;
        }
        /* Count as consumed even if the frame is dropped below. */
        player_ports_buffer_note_consumed(stream, false, load->pts);
        if (stream->sync_handle && stream->main_state == ESP_PLAYER_STATE_PLAYING && !stream->is_seeking) {
            bool decode_frame = true;
            if (!load->is_done) {
                const bool local_mjpeg_av = hosino_mjpeg_clock_source_eligible(
                    player_video_source_is_local_file(stream),
                    stream->video_side->track_info.video_info.format == ESP_FOURCC_MJPG,
                    stream->av_mask == ESP_PLAYER_MASK_AV,
                    stream->audio_side && player_audio_track_idx(stream) >= 0);
                decode_frame = local_mjpeg_av
                               ? player_sync_local_mjpeg_decode_frame(stream->sync_handle, load->pts,
                                   stream->video_side->track_info.video_info.fps)
                               : player_sync_video_decode_frame(stream->sync_handle, load->pts);
            }
            if (!decode_frame && !load->is_done) {
                ESP_LOGD(TAG, "Drop video frame");
                if (player_frame_queue_release(stream, stream->video_side->frame_queue,
                                               &stream->video_side->read_node) != ESP_GMF_IO_OK) {
                    return ESP_GMF_IO_FAIL;
                }
                load->pts = 0;
                load->meta_flag = 0;
                PLAYER_PORTS_EMPTY_LOAD(load);
                if (++consecutive_drops >= 8U) {
                    vTaskDelay(1);
                    consecutive_drops = 0;
                }
                goto _rec_dec_video_in_frame;
            }
        }
        return ESP_GMF_IO_OK;
    } else {
        if (recv_wait_ms != ESP_GMF_DATA_QUEUE_WAIT_FOREVER) {
            goto _rec_dec_video_in_frame;
        }
        ESP_LOGW(TAG, "Video queue receive failed, line: %d", __LINE__);
        esp_gmf_db_handle_t vid_db = player_video_db(stream);
        if (vid_db) {
            esp_gmf_db_abort(vid_db);
        }
        PLAYER_PORTS_EMPTY_LOAD(load);
        return ESP_GMF_IO_ABORT;
    }
}

esp_gmf_err_io_t decoder_video_in_release(void *handle, esp_gmf_payload_t *load, uint32_t wanted_size, int wait_ticks)
{
    esp_player_stream_t *stream = (esp_player_stream_t *)handle;
    (void)wanted_size;
    (void)wait_ticks;
    if (stream->video_side == NULL || stream->video_side->read_node == NULL) {
        return ESP_GMF_IO_OK;
    }
    esp_gmf_db_handle_t vid_db = player_video_db(stream);
    if (load->is_done == true && vid_db != NULL) {
        esp_gmf_db_done_write(vid_db);
    }
    return player_frame_queue_release(stream, stream->video_side->frame_queue,
                                      &stream->video_side->read_node);
}
