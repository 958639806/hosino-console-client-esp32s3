"""Execute LVGL's actual two-point center/scale functions with deterministic contacts.

This verifies direction and reset-relative distance math only. It does not
validate native state dispatch, the hardware controller, latency or the UI.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source_path = Path('C:/esp/hosino-qspi/project-apps/managed_components/lvgl__lvgl/src/indev/lv_indev_gesture.c')
source = source_path.read_text('utf-8')
compiler = root / 'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'


def body(marker):
    # Use the definition, after the forward declarations.
    marker_pos = source.rindex(marker)
    start = source.index('{', marker_pos)
    depth = 0
    for end in range(start, len(source)):
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
            if not depth:
                return source[start:end + 1]
    raise AssertionError(marker)


code = r'''
#include <math.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* This TCC runtime exports only double libm; retain float results in the
 * untouched production bodies with explicit float casts around host libm. */
#define sqrtf(x) ((float)sqrt((double)(x)))
#define atan2f(y,x) ((float)atan2((double)(y),(double)(x)))
#define LV_GESTURE_MAX_POINTS 2
typedef struct {int32_t x,y;} lv_point_t;
typedef struct {int finger;lv_point_t point;} lv_indev_gesture_motion_t;
typedef struct {
 lv_indev_gesture_motion_t motions[2];lv_point_t center;
 float scale,p_scale,delta_x,p_delta_x,delta_y,p_delta_y,rotation,p_rotation;
 float scale_factors_x[2],scale_factors_y[2];
} lv_indev_gesture_t;
'''
code += '\nstatic void gesture_update_center_point(lv_indev_gesture_t *gesture,int touch_points_nb)' + body('static void gesture_update_center_point(')
code += '\nstatic void gesture_calculate_factors(lv_indev_gesture_t *gesture,int touch_points_nb)' + body('static void gesture_calculate_factors(')
code += r'''
static int checks;
#define CHECK(x) do {++checks;assert(x);} while(0)
static void start(lv_indev_gesture_t *g,int span,int vertical){
 memset(g,0,sizeof(*g));g->scale=1;g->motions[0].finger=0;g->motions[1].finger=1;
 g->motions[0].point=(lv_point_t){240-(vertical?0:span/2),180-(vertical?span/2:0)};
 g->motions[1].point=(lv_point_t){240+(vertical?0:span/2),180+(vertical?span/2:0)};
 gesture_update_center_point(g,2);
}
static void move(lv_indev_gesture_t *g,int span,int vertical){
 g->motions[0].point=(lv_point_t){240-(vertical?0:span/2),180-(vertical?span/2:0)};
 g->motions[1].point=(lv_point_t){240+(vertical?0:span/2),180+(vertical?span/2:0)};
 gesture_calculate_factors(g,2);
}
int main(void){
 lv_indev_gesture_t g;
 for(int axis=0;axis<2;axis++){
  start(&g,100,axis);move(&g,100,axis);CHECK(fabs(g.scale-1)<.0001f);
  move(&g,130,axis);CHECK(fabs(g.scale-1.3f)<.0001f);CHECK(g.scale>1.15f);
  // A fresh gesture must reset to one, then inward motion yields below one.
  start(&g,200,axis);move(&g,200,axis);CHECK(fabs(g.scale-1)<.0001f);
  move(&g,140,axis);CHECK(fabs(g.scale-.7f)<.0001f);CHECK(g.scale<.85f);
  // Translation/rotation do not turn an inward pinch into an outward scale.
  g.motions[0].point=(lv_point_t){200,130};g.motions[1].point=(lv_point_t){200,270};
  gesture_calculate_factors(&g,2);CHECK(fabs(g.scale-.7f)<.0001f);
 }
 printf("PASS actual LVGL native pinch scale: %d assertions; horizontal/vertical inward/outward and independent gesture reset\n",checks);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='hs_native_scale_') as work:
    work = Path(work)
    (work / 'test.c').write_text(code, 'utf-8')
    built = subprocess.run([str(compiler), '-Wall', 'test.c', '-o', 'test.exe'], cwd=work, capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    ran = subprocess.run([str(work / 'test.exe')], cwd=work, capture_output=True, text=True)
    print(ran.stdout + ran.stderr, end='')
    assert ran.returncode == 0
report = {'scope': __doc__, 'passed': True, 'source_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(),
          'compile_exit_code': built.returncode, 'test_exit_code': ran.returncode, 'output': ran.stdout.strip()}
(root / 'evidence/video-pinch-v22-native-scale-20261114.json').write_text(json.dumps(report, indent=2) + '\n', 'utf-8')

