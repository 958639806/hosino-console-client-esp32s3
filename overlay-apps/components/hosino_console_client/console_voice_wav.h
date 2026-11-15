// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define HS_VOICE_SAMPLE_RATE 16000u
#define HS_VOICE_MAX_SECONDS 15u
#define HS_VOICE_MAX_FRAMES (HS_VOICE_SAMPLE_RATE * HS_VOICE_MAX_SECONDS)
#define HS_VOICE_WAV_HEADER_BYTES 44u
#define HS_VOICE_WAV_CAPACITY (HS_VOICE_WAV_HEADER_BYTES + HS_VOICE_MAX_FRAMES * 2u)
// PCM16 little endian mono RIFF/WAVE. Does not read/write outside capacity.
bool hs_voice_wav_header(uint8_t *out, size_t capacity, uint32_t frames);
// ES7210 STD stereo: MIC1 is serialized index 0, MIC2 index 1.
size_t hs_voice_mic1(uint8_t *out, size_t capacity, const int16_t *stereo, size_t frames);
#ifdef __cplusplus
}
#endif
