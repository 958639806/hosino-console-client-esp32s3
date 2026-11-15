/* SPDX-License-Identifier: Apache-2.0
 * Audio resource ownership adapted from Waveshare 1.75 e4344e7, using only
 * the 2.16 board's public pin/I2C definitions and native stereo capture mode.
 * The 2.16 display/touch BSP is unchanged.
 */
#include <stddef.h>
#include <stdbool.h>
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"
#include "bsp/esp-bsp.h"
#include "hosino_audio_board.h"
static const char *TAG = "hosino_audio_2_16";
typedef enum {
    BSP_AUDIO_MODE_NONE,
    BSP_AUDIO_MODE_STD_STD,
} hosino_audio_mode_t;

static hosino_audio_mode_t audio_mode = BSP_AUDIO_MODE_NONE;
static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;
static const audio_codec_data_if_t *i2s_data_if = NULL; /* Codec data interface */

static esp_codec_dev_handle_t speaker_codec_dev = NULL;
static const audio_codec_gpio_if_t *speaker_gpio_if = NULL;
static const audio_codec_ctrl_if_t *speaker_ctrl_if = NULL;
static const audio_codec_if_t *speaker_codec_if = NULL;

static esp_codec_dev_handle_t microphone_codec_dev = NULL;
static const audio_codec_ctrl_if_t *microphone_ctrl_if = NULL;
static const audio_codec_if_t *microphone_codec_if = NULL;

#define BSP_ES7210_CODEC_ADDR ES7210_CODEC_DEFAULT_ADDR
#define BSP_I2S_GPIO_CFG       \
    {                          \
        .mclk = BSP_I2S_MCLK,  \
        .bclk = BSP_I2S_SCLK,  \
        .ws = BSP_I2S_LCLK,    \
        .dout = BSP_I2S_DOUT,  \
        .din = BSP_I2S_DSIN,   \
        .invert_flags = {      \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv = false,   \
        },                     \
    }

static void hosino_audio_update_result(esp_err_t *result, esp_err_t error)
{
    if ((*result == ESP_OK) && (error != ESP_OK)) {
        *result = error;
    }
}

static void hosino_audio_update_codec_result(esp_err_t *result, int codec_error)
{
    if ((*result == ESP_OK) && (codec_error != ESP_CODEC_DEV_OK)) {
        *result = ESP_FAIL;
    }
}

static bool hosino_audio_has_resources(void)
{
    return (audio_mode != BSP_AUDIO_MODE_NONE) ||
           (i2s_tx_chan != NULL) ||
           (i2s_rx_chan != NULL) ||
           (i2s_data_if != NULL) ||
           (speaker_codec_dev != NULL) ||
           (speaker_gpio_if != NULL) ||
           (speaker_ctrl_if != NULL) ||
           (speaker_codec_if != NULL) ||
           (microphone_codec_dev != NULL) ||
           (microphone_ctrl_if != NULL) ||
           (microphone_codec_if != NULL);
}

static esp_err_t hosino_audio_delete_speaker_codec(void)
{
    esp_err_t ret = ESP_OK;

    if (speaker_codec_dev != NULL) {
        hosino_audio_update_codec_result(&ret, esp_codec_dev_close(speaker_codec_dev));
        esp_codec_dev_delete(speaker_codec_dev);
        speaker_codec_dev = NULL;
    }
    if (speaker_codec_if != NULL) {
        hosino_audio_update_codec_result(&ret, audio_codec_delete_codec_if(speaker_codec_if));
        speaker_codec_if = NULL;
    }
    if (speaker_ctrl_if != NULL) {
        hosino_audio_update_codec_result(&ret, audio_codec_delete_ctrl_if(speaker_ctrl_if));
        speaker_ctrl_if = NULL;
    }
    if (speaker_gpio_if != NULL) {
        hosino_audio_update_codec_result(&ret, audio_codec_delete_gpio_if(speaker_gpio_if));
        speaker_gpio_if = NULL;
    }

    return ret;
}

static esp_err_t hosino_audio_delete_microphone_codec(void)
{
    esp_err_t ret = ESP_OK;

    if (microphone_codec_dev != NULL) {
        hosino_audio_update_codec_result(&ret, esp_codec_dev_close(microphone_codec_dev));
        esp_codec_dev_delete(microphone_codec_dev);
        microphone_codec_dev = NULL;
    }
    if (microphone_codec_if != NULL) {
        hosino_audio_update_codec_result(&ret, audio_codec_delete_codec_if(microphone_codec_if));
        microphone_codec_if = NULL;
    }
    if (microphone_ctrl_if != NULL) {
        hosino_audio_update_codec_result(&ret, audio_codec_delete_ctrl_if(microphone_ctrl_if));
        microphone_ctrl_if = NULL;
    }

    return ret;
}

static esp_err_t hosino_audio_delete_i2s_channel(i2s_chan_handle_t *channel)
{
    esp_err_t ret = ESP_OK;
    esp_err_t err;

    if (*channel == NULL) {
        return ESP_OK;
    }

    err = i2s_channel_disable(*channel);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ret = err;
    }
    err = i2s_del_channel(*channel);
    hosino_audio_update_result(&ret, err);
    if (err == ESP_OK) {
        *channel = NULL;
    }

    return ret;
}

esp_err_t hosino_audio_deinit(void)
{
    esp_err_t ret = ESP_OK;

    hosino_audio_update_result(&ret, hosino_audio_delete_microphone_codec());
    hosino_audio_update_result(&ret, hosino_audio_delete_speaker_codec());

    if (i2s_data_if != NULL) {
        hosino_audio_update_codec_result(&ret, audio_codec_delete_data_if(i2s_data_if));
        i2s_data_if = NULL;
    }

    hosino_audio_update_result(&ret, hosino_audio_delete_i2s_channel(&i2s_rx_chan));
    hosino_audio_update_result(&ret, hosino_audio_delete_i2s_channel(&i2s_tx_chan));
    audio_mode = BSP_AUDIO_MODE_NONE;

    return ret;
}

static esp_err_t hosino_audio_init_channels(const i2s_std_config_t *tx_config,
                                         const i2s_std_config_t *rx_std_config,
                                         hosino_audio_mode_t mode)
{
    esp_err_t ret = ESP_OK;

    if (hosino_audio_has_resources()) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    ESP_GOTO_ON_ERROR(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan),
                      err, TAG, "I2S channel creation failed");
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_tx_chan, tx_config),
                      err, TAG, "I2S TX STD initialization failed");

    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_rx_chan, rx_std_config),
                      err, TAG, "I2S RX STD initialization failed");

    ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_tx_chan), err, TAG, "I2S TX enabling failed");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_rx_chan), err, TAG, "I2S RX enabling failed");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = i2s_rx_chan,
        .tx_handle = i2s_tx_chan,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (i2s_data_if == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    audio_mode = mode;
    return ESP_OK;

err:
    {
        esp_err_t cleanup_ret = hosino_audio_deinit();
        if (cleanup_ret != ESP_OK) {
            ESP_LOGE(TAG, "Audio cleanup failed: %s", esp_err_to_name(cleanup_ret));
        }
    }
    return ret;
}

esp_err_t hosino_audio_init(const i2s_std_config_t *i2s_config)
{
    if ((audio_mode == BSP_AUDIO_MODE_STD_STD) &&
            i2s_tx_chan != NULL && i2s_rx_chan != NULL && i2s_data_if != NULL) {
        return ESP_OK;
    }
    if (hosino_audio_has_resources()) {
        // Retry cleanup after a previous incomplete channel deletion. The
        // singleton codec owner must not leak a handle on an init failure.
        esp_err_t cleanup = hosino_audio_deinit();
        if (cleanup != ESP_OK) return cleanup;
    }

    const i2s_std_config_t default_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = BSP_I2S_GPIO_CFG,
    };
    const i2s_std_config_t *config = (i2s_config != NULL) ? i2s_config : &default_config;
    return hosino_audio_init_channels(config, config, BSP_AUDIO_MODE_STD_STD);
}

esp_err_t hosino_audio_init_voice_24k(void)
{
    return hosino_audio_init(NULL);
}

esp_codec_dev_handle_t hosino_audio_codec_speaker_init(void)
{
    if (speaker_codec_dev != NULL) {
        return speaker_codec_dev;
    }

    if (bsp_i2c_init() != ESP_OK) return NULL;
    if (i2s_data_if == NULL) {
        if (hosino_audio_init(NULL) != ESP_OK) return NULL;
    }

    speaker_gpio_if = audio_codec_new_gpio();
    if (speaker_gpio_if == NULL) {
        goto err;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    speaker_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (speaker_ctrl_if == NULL) {
        goto err;
    }

    /* Match the 2.16 BSP gain and PA pin configuration. */
    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = speaker_ctrl_if,
        .gpio_if = speaker_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    speaker_codec_if = es8311_codec_new(&es8311_cfg);
    if (speaker_codec_if == NULL) {
        goto err;
    }

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = speaker_codec_if,
        .data_if = i2s_data_if,
    };
    speaker_codec_dev = esp_codec_dev_new(&codec_dev_cfg);
    if (speaker_codec_dev == NULL) {
        goto err;
    }
    return speaker_codec_dev;

err:
    (void)hosino_audio_delete_speaker_codec();
    return NULL;
}

esp_codec_dev_handle_t hosino_audio_codec_microphone_init(void)
{
    if (microphone_codec_dev != NULL) {
        return microphone_codec_dev;
    }

    if (bsp_i2c_init() != ESP_OK) return NULL;
    if (i2s_data_if == NULL) {
        if (hosino_audio_init(NULL) != ESP_OK) return NULL;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = BSP_ES7210_CODEC_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    microphone_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (microphone_ctrl_if == NULL) {
        goto err;
    }

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = microphone_ctrl_if,
        /* 2.16 official example uses ES7210 in two-channel STD mode. */
        .mic_selected = 0,
    };
    microphone_codec_if = es7210_codec_new(&es7210_cfg);
    if (microphone_codec_if == NULL) {
        goto err;
    }

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = microphone_codec_if,
        .data_if = i2s_data_if,
    };
    microphone_codec_dev = esp_codec_dev_new(&codec_dev_cfg);
    if (microphone_codec_dev == NULL) {
        goto err;
    }
    return microphone_codec_dev;

err:
    (void)hosino_audio_delete_microphone_codec();
    return NULL;
}

