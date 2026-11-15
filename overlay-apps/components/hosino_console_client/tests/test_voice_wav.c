// SPDX-License-Identifier: Apache-2.0
#include "console_voice_wav.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned le16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | ((uint32_t)le16(p + 2) << 16); }

int main(int argc, char **argv) {
    uint8_t guarded[48]; memset(guarded, 0xa5, sizeof(guarded));
    assert(!hs_voice_wav_header(NULL, 44, 1));
    assert(!hs_voice_wav_header(guarded + 2, 43, 1));
    assert(!hs_voice_wav_header(guarded + 2, 44, HS_VOICE_MAX_FRAMES + 1));
    for (size_t i = 0; i < sizeof(guarded); ++i) assert(guarded[i] == 0xa5);
    const uint32_t lengths[] = {0, 3200, 16000, HS_VOICE_MAX_FRAMES};
    for (size_t n = 0; n < sizeof(lengths) / sizeof(lengths[0]); ++n) {
        uint8_t *h = guarded + 2;
        assert(hs_voice_wav_header(h, 44, lengths[n]));
        assert(guarded[0] == 0xa5 && guarded[1] == 0xa5 && guarded[46] == 0xa5 && guarded[47] == 0xa5);
        assert(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4));
        assert(le32(h + 4) == 36 + lengths[n] * 2 && le32(h + 16) == 16);
        assert(le16(h + 20) == 1 && le16(h + 22) == 1);
        assert(le32(h + 24) == 16000 && le32(h + 28) == 32000);
        assert(le16(h + 32) == 2 && le16(h + 34) == 16 && le32(h + 40) == lengths[n] * 2);
    }
    const int16_t stereo[] = {-32768, 111, -1, 222, 0, 333, 1, 444, 32767, 555};
    const uint8_t expected[] = {0x00,0x80,0xff,0xff,0x00,0x00,0x01,0x00,0xff,0x7f};
    uint8_t out[14]; memset(out, 0xa5, sizeof(out));
    assert(hs_voice_mic1(out + 2, 10, stereo, 5) == 10);
    assert(!memcmp(out + 2, expected, 10) && out[0] == 0xa5 && out[13] == 0xa5);
    memset(out, 0xa5, sizeof(out));
    assert(hs_voice_mic1(out + 2, 9, stereo, 5) == 0);
    assert(hs_voice_mic1(out + 2, 10, stereo, (size_t)-1) == 0);
    for (size_t i = 0; i < sizeof(out); ++i) assert(out[i] == 0xa5);

    assert(argc == 2);
    uint8_t *wav = (uint8_t *)malloc(HS_VOICE_WAV_CAPACITY);
    assert(wav && hs_voice_wav_header(wav, HS_VOICE_WAV_CAPACITY, HS_VOICE_MAX_FRAMES));
    int16_t block[512];
    for (uint32_t n = 0; n < HS_VOICE_MAX_FRAMES; n += 256) {
        size_t count = HS_VOICE_MAX_FRAMES - n; if (count > 256) count = 256;
        for (size_t i = 0; i < count; ++i) { block[i * 2] = (int16_t)((n + i) % 30000); block[i * 2 + 1] = -12345; }
        size_t offset = 44 + n * 2;
        assert(hs_voice_mic1(wav + offset, HS_VOICE_WAV_CAPACITY - offset, block, count) == count * 2);
    }
    FILE *f = fopen(argv[1], "wb"); assert(f);
    assert(fwrite(wav, 1, HS_VOICE_WAV_CAPACITY, f) == HS_VOICE_WAV_CAPACITY);
    assert(fclose(f) == 0); free(wav);
    puts("Production WAV/MIC1 helper boundaries and canaries PASS");
    return 0;
}
