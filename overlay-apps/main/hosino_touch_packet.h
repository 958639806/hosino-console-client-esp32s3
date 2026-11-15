/* SPDX-License-Identifier: MIT
 * Protocol reference: Waveshare board example, upstream commit
 * 225a62bff11b5d0a0b607873860d39485a9a9685,
 * examples/arduino/libraries/SensorLib/src/touch/TouchDrvCST92xx.cpp:
 * parseFingerData() and getPoint(). This implementation keeps the report
 * layout/ACK ordering while validating packets and stabilizing one pointer.
 */
/**
 *
 * @license MIT License
 *
 * Copyright (c) 2024 lewis he
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * @file      TouchDrvCST92xx.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @date      2024-07-07
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define HOSINO_TOUCH_PACKET_BYTES 15U
#define HOSINO_TOUCH_CONTACTS 2U

typedef struct {
    uint8_t id;
    uint16_t x, y;
} hosino_touch_contact_t;

typedef struct {
    uint8_t count;
    hosino_touch_contact_t contact[HOSINO_TOUCH_CONTACTS];
} hosino_touch_packet_t;

static inline bool hosino_touch_parse_packet(const uint8_t *data, hosino_touch_packet_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!data || data[6] != 0xAB) return false;
    const uint8_t records = data[5] & 0x7F;
    if (records > HOSINO_TOUCH_CONTACTS) return false;
    for (uint8_t i = 0; i < records; ++i) {
        const uint8_t *p = &data[i == 0 ? 0 : 7];
        if ((p[0] & 0x0F) != 0x06) continue;
        const uint8_t id = p[0] >> 4;
        if (id >= HOSINO_TOUCH_CONTACTS) goto invalid;
        for (uint8_t j = 0; j < out->count; ++j)
            if (out->contact[j].id == id) goto invalid;
        hosino_touch_contact_t *point = &out->contact[out->count++];
        point->id = id;
        point->x = ((uint16_t)p[1] << 4) | (p[3] >> 4);
        point->y = ((uint16_t)p[2] << 4) | (p[3] & 0x0F);
    }
    return true;
invalid:
    memset(out, 0, sizeof(*out));
    return false;
}

typedef struct {
    uint8_t primary_id;
    bool primary_valid;
    bool block_until_all_up;
} hosino_touch_primary_t;

/* There remains one system pointer. When its original finger disappears,
 * release all contacts and require an all-up report before another pointer
 * begins. A remaining pinch finger cannot jump into a navigation gesture.
 */
static inline void hosino_touch_primary_filter(hosino_touch_primary_t *state,
                                               hosino_touch_packet_t *packet)
{
    if (state->block_until_all_up) {
        if (!packet->count) state->block_until_all_up = false;
        state->primary_valid = false;
        packet->count = 0;
        return;
    }
    if (!packet->count) {
        state->primary_valid = false;
        return;
    }
    uint8_t primary = 0;
    if (state->primary_valid) {
        for (; primary < packet->count; ++primary)
            if (packet->contact[primary].id == state->primary_id) break;
        if (primary == packet->count) {
            state->primary_valid = false;
            state->block_until_all_up = true;
            packet->count = 0;
            return;
        }
    } else {
        state->primary_id = packet->contact[0].id;
        state->primary_valid = true;
    }
    if (primary != 0) {
        const hosino_touch_contact_t tmp = packet->contact[0];
        packet->contact[0] = packet->contact[primary];
        packet->contact[primary] = tmp;
    }
}

static inline void hosino_touch_primary_error(hosino_touch_primary_t *state)
{
    /* A failed first report may already contain a held finger. Do not start
     * a pointer halfway through that contact when the next read succeeds. */
    state->block_until_all_up = true;
    state->primary_valid = false;
}
