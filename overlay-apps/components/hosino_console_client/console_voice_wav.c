// SPDX-License-Identifier: Apache-2.0
#include "console_voice_wav.h"
#include <string.h>

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
bool hs_voice_wav_header(uint8_t *out, size_t capacity, uint32_t frames) {
    if (!out || capacity < HS_VOICE_WAV_HEADER_BYTES || frames > HS_VOICE_MAX_FRAMES) return false;
    const uint32_t bytes = frames * 2u;
    memcpy(out, "RIFF", 4); put32(out + 4, 36u + bytes);
    memcpy(out + 8, "WAVEfmt ", 8); put32(out + 16, 16);
    put16(out + 20, 1); put16(out + 22, 1);
    put32(out + 24, HS_VOICE_SAMPLE_RATE); put32(out + 28, HS_VOICE_SAMPLE_RATE * 2u);
    put16(out + 32, 2); put16(out + 34, 16);
    memcpy(out + 36, "data", 4); put32(out + 40, bytes);
    return true;
}
size_t hs_voice_mic1(uint8_t *out, size_t capacity, const int16_t *stereo, size_t frames) {
    if (!out || !stereo || frames > HS_VOICE_MAX_FRAMES || frames > capacity / 2u) return 0;
    for (size_t i = 0; i < frames; ++i) put16(out + i * 2u, (uint16_t)stereo[i * 2u]);
    return frames * 2u;
}
