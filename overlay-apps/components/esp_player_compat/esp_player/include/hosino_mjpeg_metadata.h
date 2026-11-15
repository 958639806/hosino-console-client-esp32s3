/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Explicit trusted bridge profiles only. Never replace nonzero container
 * geometry, infer from URL, or enable this for ordinary files/network media. */
static inline bool hosino_mjpeg_profile_valid(uint16_t w,uint16_t h,uint16_t fps)
{
    return (w==320&&h==180&&fps==20)||(w==480&&h==270&&fps==15);
}
static inline bool hosino_mjpeg_apply_metadata(bool managed,bool mjpeg,
        uint16_t hint_w,uint16_t hint_h,uint16_t hint_fps,
        uint16_t *w,uint16_t *h,uint16_t *fps)
{
    if(!managed||!mjpeg) return true;
    if(!hosino_mjpeg_profile_valid(hint_w,hint_h,hint_fps)||!w||!h||!fps) return false;
    if((*w&&*w!=hint_w)||(*h&&*h!=hint_h)) return false;
    if(!*w) *w=hint_w;
    if(!*h) *h=hint_h;
    if(!*fps) *fps=hint_fps;
    return true;
}
/* Decoder output can include the codec's up-to16-pixel row/block padding.
 * Reject a different picture size before allocating decoder-output blocks. */
static inline bool hosino_mjpeg_decoded_geometry(uint32_t w,uint32_t h,
        uint16_t hint_w,uint16_t hint_h,uint16_t hint_fps)
{
    return hosino_mjpeg_profile_valid(hint_w,hint_h,hint_fps)&&
           (w==hint_w||w==((hint_w+7U)&~7U)||w==((hint_w+15U)&~15U))&&
           (h==hint_h||h==((hint_h+7U)&~7U)||h==((hint_h+15U)&~15U));
}
