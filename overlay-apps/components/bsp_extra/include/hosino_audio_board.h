/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"
#include "esp_codec_dev.h"
#include "driver/i2s_std.h"
esp_err_t hosino_audio_init(const i2s_std_config_t *config);
esp_err_t hosino_audio_init_voice_24k(void);
esp_err_t hosino_audio_deinit(void);
esp_codec_dev_handle_t hosino_audio_codec_speaker_init(void);
esp_codec_dev_handle_t hosino_audio_codec_microphone_init(void);
