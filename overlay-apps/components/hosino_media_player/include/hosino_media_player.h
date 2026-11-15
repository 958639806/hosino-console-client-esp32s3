/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define HOSINO_MEDIA_PATH_MAX 512

typedef struct hosino_media_player *hosino_media_player_handle_t;
typedef enum { HOSINO_MEDIA_OWNER_MUSIC = 1, HOSINO_MEDIA_OWNER_VIDEO = 2 } hosino_media_owner_t;
typedef enum {
    HOSINO_MEDIA_DECODER_ESPRESSIF = 0,
    HOSINO_MEDIA_DECODER_JPEGDEC = 1,
} hosino_media_decoder_t;
typedef enum {
    HOSINO_MEDIA_IDLE = 0, HOSINO_MEDIA_OPENING, HOSINO_MEDIA_PLAYING,
    HOSINO_MEDIA_PAUSED, HOSINO_MEDIA_STOPPING, HOSINO_MEDIA_STOPPED,
    HOSINO_MEDIA_FINISHED, HOSINO_MEDIA_ERROR
} hosino_media_state_t;
/* RGB565 little endian, tightly packed. Borrowed only during callback. Copy it
 * before returning; never retain this pointer or call LVGL from the callback.
 * ESP_OK also permits an intentional dropped presentation frame. */
typedef struct {
    const uint8_t *data;
    size_t bytes;
    uint16_t width, height;
    uint32_t stride_bytes;
} hosino_media_video_frame_t;
typedef esp_err_t (*hosino_media_video_cb_t)(const hosino_media_video_frame_t *, void *);
typedef struct {
    hosino_media_owner_t owner;
    bool enable_video;
    bool allow_video_without_audio;
    uint16_t max_width, max_height;
    uint8_t render_fps; /* Optional compositor cadence, 0 selects 30. Single-stream playback follows source PTS/FPS. */
    hosino_media_decoder_t video_decoder; /* Fixed for this player lifetime; no silent fallback. */
    hosino_media_video_cb_t video_cb;
    void *video_ctx;
    /* Explicit managed bridge profile only; zero for local media. */
    uint16_t network_video_width, network_video_height, network_video_fps;
} hosino_media_player_config_t;
typedef struct {
    hosino_media_state_t state;
    esp_err_t last_error;
    int backend_error;
    uint64_t duration_ms, position_ms;
    uint16_t source_width, source_height, source_fps;
    uint32_t video_frames; /* frames delivered to video_cb, not LCD scan-outs */
    bool audio_enabled;
    bool resources_active; /* leases / callbacks may remain alive */
    bool seeking; /* Accepted seek is still in flight; cleared by SEEK_DONE. */
} hosino_media_player_snapshot_t;

/* The app must serialize all lifecycle / poll calls on its control worker.
 * These may block in the vendor library and must NOT run on the GUI task.
 * This facade creates no additional control task. It owns the playback SD
 * lease and exclusive board audio session; callers must not acquire them too.
 * Directory enumeration uses a separate short-lived application SD lease. */
esp_err_t hosino_media_player_create(const hosino_media_player_config_t *, hosino_media_player_handle_t *);
esp_err_t hosino_media_player_open(hosino_media_player_handle_t, const char *local_path);
/* Managed HTTPS bridge only. Copies strings; Authorization header only.
 * No SD lease, redirects or byte seeking. Online seek starts a new session. */
esp_err_t hosino_media_player_open_network(hosino_media_player_handle_t, const char *stream_url,
                                         const char *allowed_base_url, const char *device_token);
esp_err_t hosino_media_player_start(hosino_media_player_handle_t);
esp_err_t hosino_media_player_pause(hosino_media_player_handle_t);
esp_err_t hosino_media_player_resume(hosino_media_player_handle_t);
/* Serialized control-worker call, never GUI: 0..100 board output volume.
 * Requires this player's active audio ownership. Failure does not stop playback.
 * Uses the board's existing RAM preference; no new NVS write or audio buffer. */
esp_err_t hosino_media_player_set_volume(hosino_media_player_handle_t, int volume);
esp_err_t hosino_media_player_seek(hosino_media_player_handle_t, uint64_t position_ms);
/* Stop closes the vendor player and its file, then renderers, then board audio
 * and lease. A failure retains any uncertain live resources for a retry.
 * join observes completion; its timeout does not bound a concurrent stop call. */
esp_err_t hosino_media_player_stop(hosino_media_player_handle_t);
esp_err_t hosino_media_player_join(hosino_media_player_handle_t, uint32_t timeout_ms);
/* Refresh actual vendor state/time/metadata on the control worker (~100 ms).
 * The GUI-safe get_snapshot below copies only cached fields, never vendor IO. */
esp_err_t hosino_media_player_poll(hosino_media_player_handle_t);
esp_err_t hosino_media_player_get_snapshot(hosino_media_player_handle_t, hosino_media_player_snapshot_t *);
/* Call only after GUI snapshot polling is stopped; success invalidates handle.
 * On failure handle remains valid, and the caller must retain it and retry. */
esp_err_t hosino_media_player_destroy(hosino_media_player_handle_t);
#ifdef __cplusplus
}
#endif

