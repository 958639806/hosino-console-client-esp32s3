"""Reproduce this local 15-scene derivative from the pinned LVGL 9.4 sources."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import shutil


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lvgl-root", required=True)
    args = parser.parse_args()
    lvgl = Path(args.lvgl_root)
    out = Path(__file__).resolve().parent
    src_path = lvgl / "demos/benchmark/lv_demo_benchmark.c"
    src = src_path.read_text(encoding="utf-8")
    prototypes = src[src.index("static void rnd_reset(void);"):src.index("static void empty_screen_cb(void)")]
    workloads = src[src.index("static void empty_screen_cb(void)"):src.index("static void widgets_demo_cb(void)")]
    helpers = src[src.index("static void color_anim_cb(void * var, int32_t v)\n{"):src.rindex("#endif")]
    adapted = prototypes + workloads + helpers
    replacements = {
        "lv_screen_active()": "scene_root",
        "lv_layer_top()": "scene_overlay",
        "lv_display_get_horizontal_resolution(NULL)": "lv_obj_get_width(scene_root)",
        "lv_display_get_vertical_resolution(NULL)": "lv_obj_get_height(scene_root)",
    }
    for old, new in replacements.items():
        adapted = adapted.replace(old, new)
    prefix = '''/* SPDX-License-Identifier: MIT
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

'''
    api = '''
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
'''
    output = prefix + adapted + api
    (out / "portengine.c").write_text(output, encoding="utf-8", newline="\n")
    patch = "".join(difflib.unified_diff(src.splitlines(True), output.splitlines(True),
                                      fromfile="LVGL-9.4.0/demos/benchmark/lv_demo_benchmark.c",
                                      tofile="portengine.c"))
    (out / "portengine.patch").write_text(patch, encoding="utf-8", newline="\n")
    (out / "assets").mkdir(exist_ok=True)
    sources = [src_path]
    for name in ("img_benchmark_avatar.c", "img_benchmark_lvgl_logo_argb.c", "img_benchmark_lvgl_logo_rgb.c"):
        asset = lvgl / "demos/benchmark/assets" / name
        shutil.copyfile(asset, out / "assets" / name)
        sources.append(asset)
    license_path = lvgl / "LICENCE.txt"
    shutil.copyfile(license_path, out / "LICENSE.txt")
    sources.append(license_path)
    manifest = {
        "upstream": "https://github.com/lvgl/lvgl/tree/v9.4.0/demos/benchmark",
        "version": "9.4.0",
        "license": "MIT",
        "scenes": 15,
        "omitted_scene": "Widgets demo (original scene 16)",
        "upstream_files": [{"file": p.relative_to(lvgl).as_posix(), "sha256": digest(p)} for p in sources],
        "derived_portengine_sha256": digest(out / "portengine.c"),
        "patch_sha256": digest(out / "portengine.patch"),
        "source_kind": "Pinned local lvgl/lvgl 9.4.0 component already used by the firmware",
        "measurement": "Completed display-update count / actual elapsed time; not original LVGL sysmon score",
    }
    (out / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Imported {manifest['scenes']} scenes; portengine SHA256 {manifest['derived_portengine_sha256']}")


if __name__ == "__main__":
    main()
