/* SPDX-License-Identifier: MIT
 * Private-container port of the first 15 LVGL 9.4 benchmark workloads.
 */
#pragma once
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif

#define HOSINO_LVGL_BENCHMARK_SCENE_COUNT 15
const char *hosino_lvgl_benchmark_scene_name(unsigned index);
uint32_t hosino_lvgl_benchmark_scene_duration_ms(unsigned index);
/* GUI lock required. One application instance; all objects remain below parent. */
bool hosino_lvgl_benchmark_scene_start(lv_obj_t *parent, unsigned index);
/* Deletes only private scene objects and their animations; never global timers. */
void hosino_lvgl_benchmark_scene_stop(void);

#ifdef __cplusplus
}
#endif
