#pragma once

#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once with the GUI lock held. The monitored display must outlive the HUD. */
void hosino_perf_overlay_init(lv_display_t *disp);

/* GUI-task snapshot for scene benchmarks; counts completed logical refreshes.
 * Call with non-NULL outputs. It does not change the global HUD's sampling. */
void hosino_perf_overlay_get_snapshot(uint32_t *completed_frames, int64_t *time_us);

#ifdef __cplusplus
}
#endif
