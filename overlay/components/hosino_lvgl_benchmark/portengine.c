/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025 LVGL Kft
 * Derived from LVGL 9.4.0 demos/benchmark/lv_demo_benchmark.c.
 * See LICENSE.txt, SOURCE.md, source-manifest.json and portengine.patch.
 */
#include "sdkconfig.h"
#include "portengine.h"
#if defined(CONFIG_HOSINO_DISPLAY_BUFFERED) && CONFIG_HOSINO_DISPLAY_BUFFERED

#define HEADER_HEIGHT 0 /* Host application owns its header outside the scene. */
#define FALL_HEIGHT 80
#define PAD_BASIC 8
static lv_obj_t *scene_root;
static lv_obj_t *scene_overlay;
static uint32_t rnd_act;

static void rnd_reset(void);
static int32_t rnd_next(int32_t min, int32_t max);
static lv_color_t rnd_color(void);
static void shake_anim_y_cb(void * var, int32_t v);
static void fall_anim(lv_obj_t * obj, int32_t y_max);
static void scroll_anim(lv_obj_t * obj, int32_t y_max);
static void scroll_anim_y_cb(void * var, int32_t v);
static void color_anim_cb(void * var, int32_t v);
static void color_anim(lv_obj_t * obj);
static void arc_anim(lv_obj_t * obj);

static lv_obj_t * card_create(void);

static void empty_screen_cb(void)
{
    color_anim(scene_root);
}

static void moving_wallpaper_cb(void)
{
    lv_obj_set_style_pad_all(scene_root, 0, 0);
    LV_IMAGE_DECLARE(img_benchmark_lvgl_logo_rgb);

    lv_obj_t * img = lv_image_create(scene_root);
    lv_obj_set_size(img, lv_pct(150), lv_pct(150));
    lv_image_set_src(img, &img_benchmark_lvgl_logo_rgb);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_TILE);
    fall_anim(img, - lv_obj_get_height(scene_root) / 3);
}

static void single_rectangle_cb(void)
{
    lv_obj_t * obj = lv_obj_create(scene_root);
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_center(obj);
    lv_obj_set_size(obj, lv_pct(30), lv_pct(30));

    color_anim(obj);

}

static void multiple_rectangles_cb(void)
{
    lv_obj_set_flex_flow(scene_root, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(scene_root, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);

    uint32_t i;
    for(i = 0; i < 9; i++) {
        lv_obj_t * obj = lv_obj_create(scene_root);
        lv_obj_remove_style_all(obj);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_size(obj, lv_pct(25), lv_pct(25));

        color_anim(obj);
    }
}

static void multiple_rgb_images_cb(void)
{
    lv_obj_t * scr = scene_root;
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_set_style_pad_bottom(scr, FALL_HEIGHT + PAD_BASIC, 0);

    LV_IMAGE_DECLARE(img_benchmark_lvgl_logo_rgb);
    int32_t hor_cnt = ((int32_t)lv_obj_get_content_width(scr)) / 160;
    int32_t ver_cnt = ((int32_t)lv_obj_get_content_height(scr)) / 160;

    if(hor_cnt < 1) hor_cnt = 1;
    if(ver_cnt < 1) ver_cnt = 1;

    int32_t y;
    for(y = 0; y < ver_cnt; y++) {
        int32_t x;
        for(x = 0; x < hor_cnt; x++) {
            lv_obj_t * obj = lv_image_create(scene_root);
            lv_image_set_src(obj, &img_benchmark_lvgl_logo_rgb);
            if(x == 0) lv_obj_add_flag(obj, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);

            fall_anim(obj, 80);
        }
    }
}

static void multiple_argb_images_cb(void)
{
    lv_obj_t * scr = scene_root;
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_set_style_pad_bottom(scr, FALL_HEIGHT + PAD_BASIC, 0);

    LV_IMAGE_DECLARE(img_benchmark_lvgl_logo_argb);
    int32_t hor_cnt = ((int32_t)lv_obj_get_content_width(scr)) / 160;
    int32_t ver_cnt = ((int32_t)lv_obj_get_content_height(scr)) / 160;

    if(hor_cnt < 1) hor_cnt = 1;
    if(ver_cnt < 1) ver_cnt = 1;

    int32_t y;
    for(y = 0; y < ver_cnt; y++) {
        int32_t x;
        for(x = 0; x < hor_cnt; x++) {
            lv_obj_t * obj = lv_image_create(scene_root);
            lv_image_set_src(obj, &img_benchmark_lvgl_logo_argb);
            if(x == 0) lv_obj_add_flag(obj, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);

            fall_anim(obj, 80);
        }
    }
}

static void rotated_argb_image_cb(void)
{
    lv_obj_t * scr = scene_root;
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_set_style_pad_bottom(scr, FALL_HEIGHT + PAD_BASIC, 0);

    LV_IMAGE_DECLARE(img_benchmark_lvgl_logo_argb);
    int32_t hor_cnt = ((int32_t)lv_obj_get_content_width(scr)) / 240;   /*240 instead of 160 to have less rotated images*/
    int32_t ver_cnt = ((int32_t)lv_obj_get_content_height(scr)) / 240;

    if(hor_cnt < 1) hor_cnt = 1;
    if(ver_cnt < 1) ver_cnt = 1;

    int32_t y;
    for(y = 0; y < ver_cnt; y++) {
        int32_t x;
        for(x = 0; x < hor_cnt; x++) {
            lv_obj_t * obj = lv_image_create(scene_root);
            lv_image_set_src(obj, &img_benchmark_lvgl_logo_argb);
            if(x == 0) lv_obj_add_flag(obj, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);

            lv_image_set_rotation(obj, lv_rand(100, 3500));
            fall_anim(obj, 80);
        }
    }
}

static void multiple_labels_cb(void)
{
    lv_obj_t * scr = scene_root;
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_SPACE_EVENLY);

    int32_t hor_res = lv_obj_get_width(scene_root);
    int32_t ver_res = lv_obj_get_height(scene_root);

#if LV_DEMO_BENCHMARK_ALIGNED_FONTS
    if(hor_res * ver_res > 800 * 480) lv_obj_set_style_text_font(scr, &lv_font_benchmark_montserrat_26_aligned, 0);
    else if(hor_res * ver_res > 320 * 240) lv_obj_set_style_text_font(scr, &lv_font_benchmark_montserrat_20_aligned, 0);
    else lv_obj_set_style_text_font(scr, &lv_font_benchmark_montserrat_14_aligned, 0);
#else
    if(hor_res * ver_res > 800 * 480) lv_obj_set_style_text_font(scr, &lv_font_montserrat_26, 0);
    else if(hor_res * ver_res > 320 * 240) lv_obj_set_style_text_font(scr, &lv_font_montserrat_20, 0);
    else lv_obj_set_style_text_font(scr, &lv_font_montserrat_14, 0);
#endif

    lv_point_t s;
    lv_text_get_size(&s, "Hello LVGL!", lv_obj_get_style_text_font(scr, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

    int32_t hor_cnt = ((int32_t)lv_obj_get_content_width(scr)) / (s.x * 3 / 2);
    int32_t ver_cnt = ((int32_t)lv_obj_get_content_height(scr)) / (s.y  * 3);

    if(hor_cnt < 1) hor_cnt = 1;
    if(ver_cnt < 1) ver_cnt = 1;

    int32_t y;
    for(y = 0; y < ver_cnt; y++) {
        int32_t x;
        for(x = 0; x < hor_cnt; x++) {
            lv_obj_t * obj = lv_label_create(scene_root);
            if(x == 0) lv_obj_add_flag(obj, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            lv_label_set_text_static(obj, "Hello LVGL!");
            color_anim(obj);
        }
    }
}

static void screen_sized_text_cb(void)
{
    const char * txt =
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Pellentesque fringilla, lorem dapibus fringilla feugiat, justo arcu volutpat magna, vitae ultricies metus tortor nec est. Fusce ut tellus arcu. Fusce eu rutrum metus, nec porta felis. Sed sed ligula laoreet, sodales lacus blandit, elementum justo. Sed posuere quam ut pellentesque ullamcorper. In quis consequat magna. Etiam quis turpis nec lorem dictum finibus. Donec mattis enim dolor, consequat lacinia nisi scelerisque id. Nulla euismod, purus sit amet accumsan tempus, lorem lectus euismod dolor, sit amet facilisis nisl quam elementum nisi. Curabitur et massa eget lorem lacinia scelerisque eget vitae felis. Nulla facilisi.\n\n"
        "Vivamus auctor sit amet ante id rhoncus. Duis a dolor neque. Mauris eu ornare tortor. Vivamus consequat, ipsum a volutpat congue, sem libero laoreet nulla, malesuada efficitur leo orci a est. Donec tincidunt nulla nibh, quis pretium mi fermentum quis. Fusce a mattis libero. Curabitur in felis suscipit, ultrices diam imperdiet, vestibulum arcu. Praesent id faucibus turpis. Pellentesque sed massa tincidunt, interdum purus tempus, pellentesque risus. Fusce feugiat magna eget nisl eleifend efficitur. Mauris ut convallis justo. Integer malesuada rutrum orci non tincidunt.\n\n"
        "Nullam aliquet leo sit amet volutpat tincidunt. Mauris ac accumsan nibh. Morbi accumsan commodo leo, at hendrerit massa hendrerit et. Aliquam nec sodales ex. Morbi at aliquet sem. Sed at magna ut felis mollis dictum ut ac orci. Nunc id lorem lacus. Vivamus id accumsan dolor, sed suscipit nulla. Pellentesque dictum erat non bibendum tempor. Fusce arcu risus, eleifend in lacus a, iaculis fermentum sapien. Praesent sodales libero vitae massa suscipit tincidunt. Aliquam quis arcu urna. Nunc sit amet mi leo.\n\n"
        "Aliquam erat volutpat. Sed viverra pharetra ipsum, sed various arcu various nec. Curabitur rutrum odio et pretium fermentum. Maecenas vitae ligula nisi. Maecenas nec dapibus erat. Suspendisse vel accumsan erat. Proin congue diam at turpis accumsan eleifend.\n\n"
        "Etiam suscipit metus magna, in vulputate magna cursus eget. Donec vel rhoncus turpis. Phasellus vitae enim quis sapien pharetra aliquam quis a quam. In mauris nulla, euismod quis orci et, interdum finibus lorem. Aenean quis dolor eget est ultricies consectetur eu nec metus. Nullam at pulvinar elit. Aenean blandit faucibus sodales. Vivamus vel porta enim, et pharetra libero. Donec aliquet pretium erat viverra fermentum. Fusce sit amet porta mi. Nullam non elit ex. In luctus, nunc id iaculis ullamcorper, eros quam eleifend elit, quis dictum sem justo eu eros. Nulla vitae faucibus lectus. Nunc blandit, mi eget suscipit scelerisque, lorem nunc tincidunt tellus, eget gravida libero metus sed nunc.\n\n"
        "Morbi erat libero, commodo sit amet turpis eget, efficitur pulvinar dolor. Pellentesque vehicula, velit eget auctor scelerisque, sem risus aliquam lectus, sit amet dapibus massa ex non magna. Donec magna leo, laoreet quis erat vitae, consequat aliquet tellus. Etiam vitae lectus erat. Mauris interdum feugiat aliquet. Nunc justo augue, mattis id finibus eu, sagittis id enim. Vivamus malesuada mauris sed nibh luctus, porta bibendum quam ornare. Vestibulum ante ipsum primis in faucibus orci luctus et ultrices posuere cubilia curae; Vivamus malesuada magna nec diam tempus, laoreet imperdiet magna faucibus. Aliquam erat volutpat.\n\n"
        "Aenean mattis lobortis quam in venenatis. Sed euismod convallis lectus vel euismod. Vestibulum consequat luctus neque. Quisque consequat bibendum neque eget mollis. Vivamus viverra vehicula eros vel dapibus. Nullam id lectus aliquam, sagittis mi efficitur, interdum mauris. Nunc at felis lobortis, lobortis erat a, euismod augue. In id purus malesuada, tempus magna at, porta mi. Sed tristique nunc eget placerat luctus. Pellentesque posuere non purus vitae malesuada. Curabitur hendrerit dolor metus, nec posuere orci placerat ac.\n\n";

    lv_obj_t * scr = scene_root;
    int32_t hor_res = lv_obj_get_width(scene_root);
    int32_t ver_res = lv_obj_get_height(scene_root);

#if LV_DEMO_BENCHMARK_ALIGNED_FONTS
    if(hor_res * ver_res > 800 * 480) lv_obj_set_style_text_font(scr, &lv_font_benchmark_montserrat_26_aligned, 0);
    else if(hor_res * ver_res > 320 * 240) lv_obj_set_style_text_font(scr, &lv_font_benchmark_montserrat_20_aligned, 0);
    else lv_obj_set_style_text_font(scr, &lv_font_benchmark_montserrat_14_aligned, 0);
#else
    if(hor_res * ver_res > 800 * 480) lv_obj_set_style_text_font(scr, &lv_font_montserrat_26, 0);
    else if(hor_res * ver_res > 320 * 240) lv_obj_set_style_text_font(scr, &lv_font_montserrat_20, 0);
    else lv_obj_set_style_text_font(scr, &lv_font_montserrat_14, 0);
#endif

    lv_obj_t * obj = lv_label_create(scr);
    lv_obj_set_width(obj, lv_pct(100));
    lv_label_set_text_static(obj, txt);

    lv_obj_update_layout(obj);

    scroll_anim(scr, lv_obj_get_scroll_bottom(scr));
}

static void multiple_arcs_cb(void)
{
    lv_obj_t * scr = scene_root;
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_SPACE_EVENLY);

    LV_IMAGE_DECLARE(img_benchmark_lvgl_logo_argb);

    int32_t hor_cnt = ((int32_t)lv_obj_get_content_width(scr)) / 160;
    int32_t ver_cnt = ((int32_t)lv_obj_get_content_height(scr)) / 160;

    if(hor_cnt < 1) hor_cnt = 1;
    if(ver_cnt < 1) ver_cnt = 1;

    int32_t y;
    for(y = 0; y < ver_cnt; y++) {
        int32_t x;
        for(x = 0; x < hor_cnt; x++) {

            lv_obj_t * obj = lv_arc_create(scene_root);
            if(x == 0) lv_obj_add_flag(obj, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            lv_obj_set_size(obj, 100, 100);
            lv_obj_center(obj);

            lv_arc_set_bg_angles(obj, 0, 360);

            lv_obj_set_style_arc_opa(obj, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(obj, 0, LV_PART_KNOB);
            lv_obj_set_style_arc_width(obj, 10, LV_PART_INDICATOR);
            lv_obj_set_style_arc_rounded(obj, false, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(obj, rnd_color(), LV_PART_INDICATOR);
            arc_anim(obj);
        }
    }
}

static void containers_cb(void)
{
    lv_obj_t * scr = scene_root;
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_set_style_pad_bottom(scr, FALL_HEIGHT + PAD_BASIC, 0);

    int32_t hor_cnt = ((int32_t)lv_obj_get_content_width(scr)) / 350;
    int32_t ver_cnt = ((int32_t)lv_obj_get_content_height(scr)) / 170;

    if(hor_cnt < 1) hor_cnt = 1;
    if(ver_cnt < 1) ver_cnt = 1;

    int32_t y;
    for(y = 0; y < ver_cnt; y++) {
        int32_t x;
        for(x = 0; x < hor_cnt; x++) {
            lv_obj_t * card = card_create();
            if(x == 0) lv_obj_add_flag(card, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            fall_anim(card, 30);
        }
    }
}

static void containers_with_overlay_cb(void)
{
    lv_obj_t * scr = scene_root;
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_set_style_pad_bottom(scr, FALL_HEIGHT + PAD_BASIC, 0);

    int32_t hor_cnt = ((int32_t)lv_obj_get_content_width(scr)) / 350;
    int32_t ver_cnt = ((int32_t)lv_obj_get_content_height(scr)) / 170;

    if(hor_cnt < 1) hor_cnt = 1;
    if(ver_cnt < 1) ver_cnt = 1;

    int32_t y;
    for(y = 0; y < ver_cnt; y++) {
        int32_t x;
        for(x = 0; x < hor_cnt; x++) {
            lv_obj_t * card = card_create();
            if(x == 0) lv_obj_add_flag(card, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            fall_anim(card, 30);
        }
    }

    lv_obj_set_style_bg_opa(scene_overlay, LV_OPA_50, 0);
    color_anim(scene_overlay);
}

static void containers_with_opa_cb(void)
{
    lv_obj_t * scr = scene_root;
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_set_style_pad_bottom(scr, FALL_HEIGHT + PAD_BASIC, 0);

    int32_t hor_cnt = ((int32_t)lv_obj_get_content_width(scr)) / 350;
    int32_t ver_cnt = ((int32_t)lv_obj_get_content_height(scr)) / 170;

    if(hor_cnt < 1) hor_cnt = 1;
    if(ver_cnt < 1) ver_cnt = 1;

    int32_t y;
    for(y = 0; y < ver_cnt; y++) {
        int32_t x;
        for(x = 0; x < hor_cnt; x++) {
            lv_obj_t * card = card_create();
            if(x == 0) lv_obj_add_flag(card, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            lv_obj_set_style_opa(card, LV_OPA_50, 0);
            fall_anim(card, 30);
        }
    }
}

static void containers_with_opa_layer_cb(void)
{
    lv_obj_t * scr = scene_root;
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_set_style_pad_bottom(scr, FALL_HEIGHT + PAD_BASIC, 0);

    int32_t hor_cnt = ((int32_t)lv_obj_get_content_width(scr)) / 350;
    int32_t ver_cnt = ((int32_t)lv_obj_get_content_height(scr)) / 170;

    if(hor_cnt < 1) hor_cnt = 1;
    if(ver_cnt < 1) ver_cnt = 1;

    int32_t y;
    for(y = 0; y < ver_cnt; y++) {
        int32_t x;
        for(x = 0; x < hor_cnt; x++) {
            lv_obj_t * card = card_create();
            lv_obj_set_style_opa_layered(card, LV_OPA_50, 0);
            if(x == 0) lv_obj_add_flag(card, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            fall_anim(card, 30);
        }
    }
}

static void containers_with_scrolling_cb(void)
{
    lv_obj_t * scr = scene_root;
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(scr, 32, 0);

    int32_t hor_cnt = ((int32_t)lv_obj_get_content_width(scr)) / 400;
    int32_t ver_cnt = ((int32_t)lv_obj_get_content_height(scr)) / (120 + 32);

    if(hor_cnt < 1) hor_cnt = 1;
    if(ver_cnt < 1) ver_cnt = 1;

    ver_cnt *= 2; /*To make it scroll*/
    if(ver_cnt < 20) ver_cnt = 20; /*The test with many widgets*/

    int32_t y;
    for(y = 0; y < ver_cnt; y++) {
        int32_t x;
        for(x = 0; x < hor_cnt; x++) {
            lv_obj_t * card = card_create();
            if(x == 0) lv_obj_add_flag(card, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
        }
    }

    lv_obj_update_layout(scr);
    scroll_anim(scr, lv_obj_get_scroll_bottom(scr));
}

static void color_anim_cb(void * var, int32_t v)
{
    LV_UNUSED(v);
    lv_color_t c = rnd_color();
    lv_obj_set_style_bg_color(var, c, 0);
    lv_obj_set_style_text_color(var, rnd_color(), 0);
}

static void color_anim(lv_obj_t * obj)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, color_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 100);      /*New value in each ms*/
    lv_anim_set_var(&a, obj);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void arc_anim_cb(void * var, int32_t v)
{
    lv_arc_set_value(var, v);
}

static void arc_anim(lv_obj_t * obj)
{
    uint32_t t1 = rnd_next(1000, 3000);
    uint32_t t2 = rnd_next(1000, 3000);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, arc_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, t1);
    lv_anim_set_reverse_duration(&a, t2);
    lv_anim_set_var(&a, obj);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void scroll_anim_y_cb(void * var, int32_t v)
{
    lv_obj_scroll_to_y(var, v, LV_ANIM_OFF);
}

static void scroll_anim(lv_obj_t * obj, int32_t y_max)
{
    uint32_t t = lv_anim_speed(lv_display_get_dpi(NULL));

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, scroll_anim_y_cb);
    lv_anim_set_values(&a, 0, y_max);
    lv_anim_set_duration(&a, t);
    lv_anim_set_reverse_duration(&a, t);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

}
static void shake_anim_y_cb(void * var, int32_t v)
{
    lv_obj_set_style_translate_y(var, v, 0);
}

static void fall_anim(lv_obj_t * obj, int32_t y_max)
{
    uint32_t t1 = rnd_next(300, 3000);
    uint32_t t2 = rnd_next(300, 3000);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, shake_anim_y_cb);
    lv_anim_set_values(&a, 0, y_max);
    lv_anim_set_duration(&a, t1);
    lv_anim_set_reverse_duration(&a, t2);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static lv_obj_t * card_create(void)
{
    lv_obj_t * panel = lv_obj_create(scene_root);
    lv_obj_set_size(panel, 270, 120);
    lv_obj_set_style_pad_all(panel, 8, 0);

    LV_IMAGE_DECLARE(img_benchmark_avatar);
    lv_obj_t * child = lv_image_create(panel);
    lv_obj_align(child, LV_ALIGN_LEFT_MID, 0, 0);
    lv_image_set_src(child, &img_benchmark_avatar);

    child = lv_label_create(panel);
    lv_label_set_text_static(child, "John Smith");

#if LV_DEMO_BENCHMARK_ALIGNED_FONTS
    lv_obj_set_style_text_font(child, &lv_font_benchmark_montserrat_24_aligned, 0);
#else
    lv_obj_set_style_text_font(child, &lv_font_montserrat_24, 0);
#endif

    lv_obj_set_pos(child, 100, 0);

    child = lv_label_create(panel);
    lv_label_set_text_static(child, "A DIY enthusiast");

#if LV_DEMO_BENCHMARK_ALIGNED_FONTS
    lv_obj_set_style_text_font(child, &lv_font_benchmark_montserrat_14_aligned, 0);
#else
    lv_obj_set_style_text_font(child, &lv_font_montserrat_14, 0);
#endif

    lv_obj_set_pos(child, 100, 30);

    child = lv_button_create(panel);
    lv_obj_set_pos(child, 100, 50);

    child = lv_label_create(child);
    lv_label_set_text_static(child, "Connect");

    return panel;
}

static void rnd_reset(void)
{
    rnd_act = 0;
}

static int32_t rnd_next(int32_t min, int32_t max)
{
    static const uint32_t rnd_map[] = {
        0xbd13204f, 0x67d8167f, 0x20211c99, 0xb0a7cc05,
        0x06d5c703, 0xeafb01a7, 0xd0473b5c, 0xc999aaa2,
        0x86f9d5d9, 0x294bdb29, 0x12a3c207, 0x78914d14,
        0x10a30006, 0x6134c7db, 0x194443af, 0x142d1099,
        0x376292d5, 0x20f433c5, 0x074d2a59, 0x4e74c293,
        0x072a0810, 0xdd0f136d, 0x5cca6dbc, 0x623bfdd8,
        0xb645eb2f, 0xbe50894a, 0xc9b56717, 0xe0f912c8,
        0x4f6b5e24, 0xfe44b128, 0xe12d57a8, 0x9b15c9cc,
        0xab2ae1d3, 0xb4dc5074, 0x67d457c8, 0x8e46b00c,
        0xa29a1871, 0xcee40332, 0x80f93aa1, 0x85286096,
        0x09bd6b49, 0x95072088, 0x2093924b, 0x6a27328f,
        0xa796079b, 0xc3b488bc, 0xe29bcce0, 0x07048a4c,
        0x7d81bd99, 0x27aacb30, 0x44fc7a0e, 0xa2382241,
        0x8357a17d, 0x97e9c9cc, 0xad10ff52, 0x9923fc5c,
        0x8f2c840a, 0x20356ba2, 0x7997a677, 0x9a7f1800,
        0x35c7562b, 0xd901fe51, 0x8f4e053d, 0xa5b94923,
        0xd2c5eedd, 0x24f0cc9b, 0x3aa7b571, 0xd289a1c9,
        0x79c7dc3,  0x5bf68c86, 0xc9f55239, 0x42052cfb,
        0x63dae9df, 0x75c9e11f, 0x407f9151, 0x104ebc63,
        0xb4b52591, 0x53a46b7a, 0x9398d144, 0x9a7c6c3d,
        0x76b35b78, 0xa028e33e, 0xbfe586e4, 0xf3f79731,
        0x99591738, 0xd7b0a847, 0x1ffb1936, 0xfeeea2e4,
        0xbc896279, 0xa8241a72, 0x871124fa, 0x27bb9866,
        0x41794272, 0x92f5dc59, 0x98c9d185, 0x6fc5905b,
        0xf0ba9f1a, 0x771cad1b, 0xf6285752, 0xb5ffcbc5,
        0x6fd2b63c, 0x2c404190, 0x209469e6, 0x628531b1,
        0x98a726bc, 0xcc6c0d97, 0x86c2e7b9, 0x7bc12e1f,
        0xf9a67e10, 0xd5bf101f, 0xa1aaaf35, 0x69b078fc,
        0x71d698b2, 0x9a954baa, 0xe7423a82, 0xdd9898e1,
        0xf4980e5c, 0x4f3607b9, 0x9ce35d27, 0xb4b764e0,
        0xa1fa3ad3, 0x220ad165, 0x282216b4, 0x7e583888,
        0xf8315b2b, 0x81c27062, 0x8eb89a85,     /*Intentionally incomplete line to make the length of array more arbitrary*/

    };

    if(min == max) return min;

    if(min > max) {
        int32_t t = min;
        min = max;
        max = t;
    }

    int32_t d = max - min;
    int32_t r = (rnd_map[rnd_act] % d) + min;

    rnd_act++;
    if(rnd_act >= sizeof(rnd_map) / sizeof(rnd_map[0])) rnd_act = 0;

    return r;
}

static lv_color_t rnd_color(void)
{
    return lv_palette_main(rnd_next(0, LV_PALETTE_LAST - 1));
}


typedef struct {
    const char *name;
    uint32_t duration_ms;
    void (*create_cb)(void);
} hosino_scene_t;

static const hosino_scene_t scenes[HOSINO_LVGL_BENCHMARK_SCENE_COUNT] = {
    {"Empty screen", 3000, empty_screen_cb},
    {"Moving wallpaper", 3000, moving_wallpaper_cb},
    {"Single rectangle", 3000, single_rectangle_cb},
    {"Multiple rectangles", 3000, multiple_rectangles_cb},
    {"Multiple RGB images", 3000, multiple_rgb_images_cb},
    {"Multiple ARGB images", 3000, multiple_argb_images_cb},
    {"Rotated ARGB images", 3000, rotated_argb_image_cb},
    {"Multiple labels", 3000, multiple_labels_cb},
    {"Screen sized text", 5000, screen_sized_text_cb},
    {"Multiple arcs", 3000, multiple_arcs_cb},
    {"Containers", 3000, containers_cb},
    {"Containers with overlay", 3000, containers_with_overlay_cb},
    {"Containers with opa", 3000, containers_with_opa_cb},
    {"Containers with opa_layer", 3000, containers_with_opa_layer_cb},
    {"Containers with scrolling", 5000, containers_with_scrolling_cb},
};

const char *hosino_lvgl_benchmark_scene_name(unsigned index)
{
    return index < HOSINO_LVGL_BENCHMARK_SCENE_COUNT ? scenes[index].name : "";
}

uint32_t hosino_lvgl_benchmark_scene_duration_ms(unsigned index)
{
    return index < HOSINO_LVGL_BENCHMARK_SCENE_COUNT ? scenes[index].duration_ms : 0;
}

void hosino_lvgl_benchmark_scene_stop(void)
{
    /* Deleting an object also deletes animations whose var is that object.
     * All imported animations target these private objects or their children. */
    if(scene_overlay) {
        lv_obj_delete(scene_overlay);
        scene_overlay = NULL;
    }
    if(scene_root) {
        lv_obj_delete(scene_root);
        scene_root = NULL;
    }
}

bool hosino_lvgl_benchmark_scene_start(lv_obj_t *parent, unsigned index)
{
    if(!parent || index >= HOSINO_LVGL_BENCHMARK_SCENE_COUNT) return false;
    hosino_lvgl_benchmark_scene_stop();
    scene_root = lv_obj_create(parent);
    if(!scene_root) return false;
    lv_obj_remove_style_all(scene_root);
    lv_obj_set_size(scene_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(scene_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scene_root, lv_palette_lighten(LV_PALETTE_GREY, 4), 0);
    lv_obj_set_style_text_color(scene_root, lv_color_black(), 0);
    lv_obj_set_style_text_font(scene_root, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_pad_all(scene_root, PAD_BASIC, 0);
    lv_obj_set_style_pad_gap(scene_root, PAD_BASIC, 0);
    lv_obj_set_style_pad_top(scene_root, HEADER_HEIGHT, 0);
    lv_obj_set_layout(scene_root, LV_LAYOUT_NONE);
    lv_obj_set_flex_align(scene_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(scene_root, LV_SCROLLBAR_MODE_OFF);

    scene_overlay = lv_obj_create(parent);
    if(!scene_overlay) {
        hosino_lvgl_benchmark_scene_stop();
        return false;
    }
    lv_obj_remove_style_all(scene_overlay);
    lv_obj_set_size(scene_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(scene_overlay, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(scene_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(parent);
    rnd_reset();
    scenes[index].create_cb();
    return true;
}
#endif
