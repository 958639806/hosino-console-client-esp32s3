/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <string.h>
#include "esp_extractor.h"
#include "hosino_mjpeg_metadata.h"

/* The pinned FLV parser returns from parse_stream after its first media packet.
 * Discover BOTH managed tracks before the player emits its one-shot track event.
 * Entries retain native-pool references; popping transfers ownership to the
 * existing port/decoder path, while clear releases only untransferred entries.
 * This structure itself is allocated in PSRAM by the player adapter. */
#define HOSINO_FLV_STARTUP_PACKETS 8U
#define HOSINO_FLV_STARTUP_BYTES (96U * 1024U)
#define HOSINO_FLV_STARTUP_INPUT_BYTES (128U * 1024U)
#define HOSINO_FLV_STARTUP_US 3000000LL

typedef struct {
    esp_extractor_frame_info_t frames[HOSINO_FLV_STARTUP_PACKETS];
    uint32_t bytes;
    uint32_t input_bytes;
    unsigned count;
    unsigned next;
    int64_t deadline_us;
    esp_extractor_err_t error;
    const char *reason;
} hosino_flv_startup_t;

typedef int64_t (*hosino_flv_now_fn)(void *ctx);
typedef bool (*hosino_flv_stop_fn)(void *ctx);

static inline uint32_t hosino_flv_startup_read_limit(hosino_flv_startup_t *q,int64_t now_us,
                                                    uint32_t wanted,uint32_t *wait_ms)
{
    if (!q || !q->deadline_us) return wanted;
    if (now_us>=q->deadline_us || q->input_bytes>=HOSINO_FLV_STARTUP_INPUT_BYTES) return 0;
    const uint32_t left=HOSINO_FLV_STARTUP_INPUT_BYTES-q->input_bytes;
    const uint32_t time_left_ms=(uint32_t)((q->deadline_us-now_us)/1000);
    if (!time_left_ms) return 0;
    if (*wait_ms>time_left_ms) *wait_ms=time_left_ms;
    return wanted<left ? wanted : left;
}

static inline bool hosino_flv_startup_enabled(bool managed, bool https,
        uint8_t player_mask, uint8_t extractor_mask, uint16_t w, uint16_t h, uint16_t fps)
{
    return managed && https && player_mask == ESP_EXTRACT_MASK_AV &&
           extractor_mask == ESP_EXTRACT_MASK_AV && hosino_mjpeg_profile_valid(w,h,fps);
}

static inline bool hosino_flv_startup_pop(hosino_flv_startup_t *q, esp_extractor_frame_info_t *frame)
{
    if (!q || q->next >= q->count) return false;
    *frame = q->frames[q->next];
    memset(&q->frames[q->next++],0,sizeof(*frame));
    return true;
}

static inline void hosino_flv_startup_release_local(esp_extractor_handle_t ext,
        esp_extractor_frame_info_t *frame,bool *owned)
{
    if (*owned && frame->frame_buffer) esp_extractor_release_frame(ext,frame);
    *owned=false;
}

static inline void hosino_flv_startup_clear(hosino_flv_startup_t *q, esp_extractor_handle_t ext)
{
    if (!q) return;
    for (unsigned i=q->next;i<q->count;i++) {
        if (q->frames[i].frame_buffer) esp_extractor_release_frame(ext,&q->frames[i]);
        memset(&q->frames[i],0,sizeof(q->frames[i]));
    }
    q->next=q->count=0;
    q->bytes=0;
    q->input_bytes=0;
    q->deadline_us=0;
}

static inline esp_extractor_err_t hosino_flv_startup_count(esp_extractor_handle_t ext,
        esp_extractor_stream_type_t type,uint16_t *count)
{
    *count=0;
    esp_extractor_err_t err=esp_extractor_get_stream_num(ext,type,count);
    return err==ESP_EXTRACTOR_ERR_NOT_FOUND ? ESP_EXTRACTOR_ERR_OK : err;
}

static inline esp_extractor_err_t hosino_flv_startup_fail(hosino_flv_startup_t *q,
        esp_extractor_err_t error,const char *reason)
{
    q->error=error;
    q->reason=reason;
    q->deadline_us=0;
    return error;
}

static inline esp_extractor_err_t hosino_flv_startup_probe(hosino_flv_startup_t *q,
        esp_extractor_handle_t ext,hosino_flv_now_fn now,hosino_flv_stop_fn stopped,void *ctx,
        uint16_t profile_w,uint16_t profile_h,uint16_t profile_fps,
        uint16_t *audio_count,uint16_t *video_count)
{
    q->deadline_us=now(ctx)+HOSINO_FLV_STARTUP_US;
    q->error=ESP_EXTRACTOR_ERR_OK;
    q->reason="ready";
    esp_extractor_type_t type=ESP_EXTRACTOR_TYPE_NONE;
    if (esp_extractor_get_extractor_type(ext,&type)!=ESP_EXTRACTOR_ERR_OK || type!=ESP_EXTRACTOR_TYPE_FLV)
        return hosino_flv_startup_fail(q,ESP_EXTRACTOR_ERR_NOT_SUPPORTED,"container");
    for (;;) {
        if (stopped(ctx)) return hosino_flv_startup_fail(q,ESP_EXTRACTOR_ERR_ABORTED,"cancelled");
        if (now(ctx)>=q->deadline_us) return hosino_flv_startup_fail(q,ESP_EXTRACTOR_ERR_READ,"deadline");
        esp_extractor_err_t err=hosino_flv_startup_count(ext,ESP_EXTRACTOR_STREAM_TYPE_AUDIO,audio_count);
        if (err==ESP_EXTRACTOR_ERR_OK) err=hosino_flv_startup_count(ext,ESP_EXTRACTOR_STREAM_TYPE_VIDEO,video_count);
        if (err!=ESP_EXTRACTOR_ERR_OK) return hosino_flv_startup_fail(q,err,"track_count");
        if (*audio_count>1 || *video_count>1) return hosino_flv_startup_fail(q,ESP_EXTRACTOR_ERR_NOT_SUPPORTED,"multiple_tracks");
        if (*audio_count==1 && *video_count==1) break;
        if (q->count>=HOSINO_FLV_STARTUP_PACKETS) return hosino_flv_startup_fail(q,ESP_EXTRACTOR_ERR_NOT_FOUND,"packet_limit_missing_track");
        esp_extractor_frame_info_t frame={0};
        err=esp_extractor_read_frame(ext,&frame);
        if (err!=ESP_EXTRACTOR_ERR_OK) {
            if (frame.frame_buffer && (err==ESP_EXTRACTOR_ERR_EOS)) esp_extractor_release_frame(ext,&frame);
            return hosino_flv_startup_fail(q,err,err==ESP_EXTRACTOR_ERR_WAITING_OUTPUT?"native_pool_full":"read_frame");
        }
        if (!frame.frame_buffer || !frame.frame_size || frame.stream_idx!=0 || EXTRACTOR_IS_EOS(frame.frame_flag) ||
                (frame.stream_type!=ESP_EXTRACTOR_STREAM_TYPE_AUDIO && frame.stream_type!=ESP_EXTRACTOR_STREAM_TYPE_VIDEO)) {
            if (frame.frame_buffer) esp_extractor_release_frame(ext,&frame);
            return hosino_flv_startup_fail(q,ESP_EXTRACTOR_ERR_WRONG_HEADER,"media_packet");
        }
        if (frame.frame_size>HOSINO_FLV_STARTUP_BYTES-q->bytes) {
            esp_extractor_release_frame(ext,&frame);
            return hosino_flv_startup_fail(q,ESP_EXTRACTOR_ERR_NO_MEM,"byte_limit");
        }
        q->frames[q->count++]=frame;
        q->bytes+=frame.frame_size;
    }
    esp_extractor_stream_info_t audio={0},video={0};
    if (esp_extractor_get_stream_info(ext,ESP_EXTRACTOR_STREAM_TYPE_AUDIO,0,&audio)!=ESP_EXTRACTOR_ERR_OK ||
        esp_extractor_get_stream_info(ext,ESP_EXTRACTOR_STREAM_TYPE_VIDEO,0,&video)!=ESP_EXTRACTOR_ERR_OK)
        return hosino_flv_startup_fail(q,ESP_EXTRACTOR_ERR_WRONG_HEADER,"track_info");
    uint16_t w=video.video_info.width,h=video.video_info.height,fps=video.video_info.fps;
    if (audio.audio_info.format!=ESP_EXTRACTOR_AUDIO_FORMAT_PCM || audio.audio_info.sample_rate!=22050 ||
        audio.audio_info.channel!=1 || audio.audio_info.bits_per_sample!=16 ||
        video.video_info.format!=ESP_EXTRACTOR_VIDEO_FORMAT_MJPEG ||
        (fps && fps!=profile_fps) ||
        !hosino_mjpeg_apply_metadata(true,true,profile_w,profile_h,profile_fps,&w,&h,&fps))
        return hosino_flv_startup_fail(q,ESP_EXTRACTOR_ERR_NOT_SUPPORTED,"track_format");
    /* New FLV records inherit cfg.extract_mask in native flv_read_frame. Thus
     * their first packet is retained even when generic parse's enable(audio0)
     * ran before audio was discovered. Explicitly enable both after discovery. */
    if (esp_extractor_enable_stream(ext,ESP_EXTRACTOR_STREAM_TYPE_AUDIO,0,true)!=ESP_EXTRACTOR_ERR_OK ||
        esp_extractor_enable_stream(ext,ESP_EXTRACTOR_STREAM_TYPE_VIDEO,0,true)!=ESP_EXTRACTOR_ERR_OK)
        return hosino_flv_startup_fail(q,ESP_EXTRACTOR_ERR_FAIL,"enable_tracks");
    q->deadline_us=0;
    return ESP_EXTRACTOR_ERR_OK;
}
