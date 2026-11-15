// SPDX-License-Identifier: Apache-2.0
#include "BiliBili.hpp"
#include "hosino_bili_wifi_policy.h"

namespace {
using namespace esp_brookesia::apps;
struct MediaContext {
    hosino_media_player_handle_t handle = nullptr;
    hosino_bili_wifi_policy_t wifi{};
} context;

esp_err_t stop(void *opaque)
{
    auto &ctx = *static_cast<MediaContext *>(opaque);
    esp_err_t result=ESP_OK;
    if (ctx.handle) {
        result=hosino_media_player_destroy(ctx.handle);
        if (result==ESP_OK) ctx.handle=nullptr;
    }
    /* Even failed teardown must not silently leave global modem sleep disabled.
     * Retain the media handle on failure and retain a failed restore for retry. */
    (void)hosino_bili_wifi_leave(&ctx.wifi);
    /* A restore-only failure must not keep Worker::media_active true after
     * successful destroy. The static policy retains that retry separately. */
    return result;
}
esp_err_t open(void *opaque, const BiliPlaybackRequest &request,
               hosino_media_video_cb_t callback, void *callback_context)
{
    auto &ctx = *static_cast<MediaContext *>(opaque);
    esp_err_t result = stop(opaque);
    if (result != ESP_OK) return result;
    /* Refuse a new acquisition while an old restore remains unresolved, but
     * never report a freed player as active merely for that policy failure. */
    if (ctx.wifi.owned) {
        result=hosino_bili_wifi_leave(&ctx.wifi);
        if (result!=ESP_OK) return result;
    }
    hosino_media_player_config_t config{};
    config.owner = HOSINO_MEDIA_OWNER_VIDEO;
    config.enable_video = true;
    config.allow_video_without_audio = false;
    config.max_width = 480;
    config.max_height = 480;
    config.render_fps = 30;
    config.video_decoder = HOSINO_MEDIA_DECODER_ESPRESSIF;
    config.video_cb = callback;
    config.video_ctx = callback_context;
    config.network_video_width=request.video_width;
    config.network_video_height=request.video_height;
    config.network_video_fps=request.video_fps;
    result = hosino_media_player_create(&config, &ctx.handle);
    if (result == ESP_OK)
        result = hosino_media_player_open_network(ctx.handle, request.stream_url,
                                                  request.allowed_base_url, request.device_token);
    if (result==ESP_OK) {
        ESP_LOGI("bili_wifi","STREAM_RX window=%d mailbox=%d",CONFIG_LWIP_TCP_WND_DEFAULT,CONFIG_LWIP_TCP_RECVMBOX_SIZE);
        result=hosino_bili_wifi_enter(&ctx.wifi);
    }
    if (result == ESP_OK) result = hosino_media_player_start(ctx.handle);
    if (result != ESP_OK) {
        const esp_err_t cleanup = stop(opaque);
        if (cleanup != ESP_OK) return cleanup;
    }
    return result;
}
esp_err_t pause(void *opaque, bool paused)
{
    auto &ctx = *static_cast<MediaContext *>(opaque);
    if (!ctx.handle) return ESP_ERR_INVALID_STATE;
    if (paused) {
        const esp_err_t result=hosino_media_player_pause(ctx.handle);
        if (result==ESP_OK) (void)hosino_bili_wifi_leave(&ctx.wifi);
        return result;
    }
    esp_err_t result=hosino_bili_wifi_enter(&ctx.wifi);
    if (result==ESP_OK) result=hosino_media_player_resume(ctx.handle);
    if (result!=ESP_OK) (void)hosino_bili_wifi_leave(&ctx.wifi);
    return result;
}
esp_err_t volume(void *opaque, int value)
{
    auto &ctx = *static_cast<MediaContext *>(opaque);
    return ctx.handle ? hosino_media_player_set_volume(ctx.handle, value) : ESP_ERR_INVALID_STATE;
}
esp_err_t snapshot(void *opaque, BiliMediaSnapshot &out)
{
    auto &ctx = *static_cast<MediaContext *>(opaque);
    if (!ctx.handle) { out = {}; (void)hosino_bili_wifi_leave(&ctx.wifi); return ESP_OK; }
    const esp_err_t result = hosino_media_player_poll(ctx.handle);
    hosino_media_player_snapshot_t value{};
    const esp_err_t copied = hosino_media_player_get_snapshot(ctx.handle, &value);
    if (copied != ESP_OK) { (void)hosino_bili_wifi_leave(&ctx.wifi);return copied; }
    out.state = value.state;
    out.error = value.last_error;
    out.position_ms = value.position_ms;
    out.duration_ms = value.duration_ms;
    if (result!=ESP_OK||value.state==HOSINO_MEDIA_PAUSED||value.state==HOSINO_MEDIA_FINISHED||value.state==HOSINO_MEDIA_ERROR)
        (void)hosino_bili_wifi_leave(&ctx.wifi);
    return result;
}
} // namespace

void hosino_bili_install_media_hooks(esp_brookesia::apps::BiliBili *app)
{
    if (!app) return;
    BiliMediaHooks hooks{};
    hooks.context = &context;
    hooks.open = open;
    hooks.stop = stop;
    hooks.pause = pause;
    hooks.volume = volume;
    hooks.snapshot = snapshot;
    app->setMediaHooks(hooks);
}
