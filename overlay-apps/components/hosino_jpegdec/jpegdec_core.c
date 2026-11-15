/* SPDX-License-Identifier: Apache-2.0
 * Build the vendored portable C API, with explicit ESP-IDF S3 SIMD selection. */
#include "vendor/JPEGDEC/src/jpeg.inl"

#if defined(ESP_PLATFORM) && !defined(HOSINO_JPEGDEC_HOST_TEST)
#ifndef ESP32S3_SIMD
#error "JPEGDEC must compile its ESP32-S3 SIMD implementation on this board"
#endif
#endif
