// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lvgl.h"
#include "file_iterator.h"
#ifdef __cplusplus
extern "C" {
#endif
void lv_demo_music(lv_obj_t *parent, file_iterator_instance_t *iterator);
void lv_demo_music_close(void);
void lv_demo_music_pause(void);
void lv_demo_music_exit_pause(void);
void lv_demo_music_activate(void);
void lv_demo_music_set_message(const char *message);
#ifdef __cplusplus
}
#endif
