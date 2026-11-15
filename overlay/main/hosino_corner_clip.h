#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lv_adapter_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Single 480x480 RGB565 display, PARTIAL/NONE only. Every stateful API must
 * run before the LVGL worker starts, or on the GUI task/under its mutex.
 * This module owns the adapter's custom_draw_bitmap slot; do not replace it
 * independently. It never changes display resolution or input coordinates. */
typedef struct {
    uint64_t requested_flushes;
    uint64_t requested_bytes;
    uint64_t sent_bytes; /* Bytes successfully submitted, NOT optical delivery. */
    uint64_t clipped_flushes;
    uint64_t skipped_flushes;
    uint64_t failures;
    uint64_t fallback_flushes; /* Unsupported geometry/capacity: original sent. */
} hosino_corner_clip_stats_t;

typedef struct {
    int x1;
    int y1;
    int x2; /* Exclusive. */
    int y2; /* Exclusive. */
    bool visible;
} hosino_corner_clip_box_t;

/* Called immediately after adapter display registration, with its final cfg.
 * Allocates one persistent PSRAM scratch buffer and installs the callback.
 * An allocation/installation failure leaves the original display path active. */
esp_err_t hosino_corner_clip_attach(const esp_lv_adapter_display_config_t *cfg,
                                  lv_display_t *disp);
esp_err_t hosino_corner_clip_init(lv_display_t *disp); /* Idempotent after attach. */
void hosino_corner_clip_set_radius(uint16_t radius); /* Clamped to 0..240. */
uint16_t hosino_corner_clip_get_radius(void);
/* Enabling without a successful attach or with radius 0 leaves it disabled. */
void hosino_corner_clip_set_enabled(bool enabled);
bool hosino_corner_clip_is_enabled(void);
void hosino_corner_clip_get_stats(hosino_corner_clip_stats_t *stats);
void hosino_corner_clip_reset_stats(void);

/* Pure geometry shared with calibration. Each row retains [inset,480-inset).
 * A conservative circle/pixel-box intersection is rounded OUT to even x.
 * It is a user-calibrated mask, not a claim about this panel's actual radius. */
int hosino_corner_clip_row_inset(int y, int radius);
/* Input must be an in-bounds nonempty even rectangle. Returns a conservative
 * even bounding box containing every retained pixel, at most one rectangle. */
hosino_corner_clip_box_t hosino_corner_clip_bounds(int x1, int y1, int x2, int y2,
                                                uint16_t radius);

#ifdef __cplusplus
}
#endif
