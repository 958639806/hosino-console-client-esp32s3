"""Run actual VideoPlayer gesture-policy bodies with deterministic LVGL stubs.

This checks actual LVGL hit routing plus production event admission, one mode
change per gesture and click suppression. It does not emulate physical touch,
native gesture recognition or rendering.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=ROOT / 'overlay-apps/components/VideoPlayer/VideoPlayer.cpp')
parser.add_argument('--expect-regression-failure', action='store_true')
args = parser.parse_args()
SOURCE = args.source.resolve()
source = SOURCE.read_text('utf-8')
compiler = ROOT / 'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'
lvgl = Path('C:/esp/hosino-qspi/project-apps/managed_components/lvgl__lvgl/src')


def body(marker, text=None):
    text = source if text is None else text
    start = text.index('{', text.index(marker))
    depth = 0
    for end in range(start, len(text)):
        if text[end] == '{': depth += 1
        elif text[end] == '}':
            depth -= 1
            if not depth: return text[start:end + 1]
    raise AssertionError(marker)


def convert(text):
    text = text.replace('constexpr ', 'const ').replace('nullptr', 'NULL')
    text = text.replace('auto *self = static_cast<VideoPlayer *>(lv_event_get_user_data(event))',
                        'Self *self = (Self *)lv_event_get_user_data(event)')
    text = text.replace('auto *target = static_cast<lv_obj_t *>(lv_event_get_target(event))',
                        'lv_obj_t *target = (lv_obj_t *)lv_event_get_target(event)')
    text = text.replace('const State state', 'const int state').replace('const auto gesture_state', 'const int gesture_state')
    text = text.replace('State::', 'STATE_').replace('self->_state.load()', 'self->_state')
    text = text.replace('self->_browser_visible.load()', 'self->_browser_visible')
    text = text.replace('self->resetPinch()', 'resetPinch(self)').replace('self->showChrome()', 'showChrome(self)')
    text = text.replace('self->setChromeVisible(false)', 'setChromeVisible(self, false)')
    text = text.replace('self->_scale_failed.store(false)', 'self->_scale_failed = false')
    text = text.replace('self->_scale_mode.store(static_cast<hosino_video_scale_mode_t>(mode))', 'self->_scale_mode = mode')
    return text


code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define LV_USE_GESTURE_RECOGNITION 1
#define ESP_UTILS_LOGI(...) (++log_calls)
enum { HOSINO_VIDEO_ORIGINAL, HOSINO_VIDEO_FIT, HOSINO_VIDEO_FILL };
enum { LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST, LV_EVENT_GESTURE, LV_EVENT_CLICKED, LV_EVENT_SIZE_CHANGED, LV_EVENT_HIT_TEST };
enum { LV_INDEV_GESTURE_PINCH, LV_INDEV_GESTURE_ROTATE };
enum { LV_INDEV_GESTURE_STATE_NONE, LV_INDEV_GESTURE_STATE_ONGOING, LV_INDEV_GESTURE_STATE_RECOGNIZED, LV_INDEV_GESTURE_STATE_ENDED, LV_INDEV_GESTURE_STATE_CANCELED };
enum { STATE_Playing, STATE_Paused, STATE_Preparing };
enum { LV_INDEV_TYPE_POINTER, LV_INDEV_TYPE_KEYPAD };
enum { LV_OBJ_FLAG_CLICKABLE=1, LV_OBJ_FLAG_SCROLLABLE=2, LV_OBJ_FLAG_HIDDEN=4, LV_OBJ_FLAG_OVERFLOW_VISIBLE=8, LV_OBJ_FLAG_ADV_HITTEST=16, LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE=1 };
typedef int lv_event_code_t;
typedef struct {int32_t x,y;} lv_point_t;
typedef struct {int32_t x1,y1,x2,y2;} lv_area_t;
typedef struct lv_obj_t {lv_area_t area;bool hidden;int flags;struct {struct lv_obj_t **children;} *spec_attr;uint32_t child_cnt;} lv_obj_t;
typedef struct {const lv_point_t *point;bool res;} lv_hit_test_info_t;
typedef struct {int32_t width,height;} lv_display_t;
typedef struct {int type;lv_point_t point;} lv_indev_t;
typedef struct {
 bool _pinch_armed,_pinch_applied,_pinch_seen,_foreground,_browser_visible,_frame_presented;
 bool _seek_dragging,_volume_dragging,_scale_failed,_chrome_visible;
 int _state,_scale_mode;uint32_t _chrome_hide_at_ms;
 lv_obj_t *_canvas,*_viewport,*_screen,*_header,*_controls;
} Self;
typedef struct {Self *self;int code;lv_obj_t *target,*current;lv_indev_t *indev;int gesture,state;float scale;} lv_event_t;
static lv_obj_t screen,viewport,canvas,header,controls,button,other_screen;
static lv_obj_t *viewport_children[]={&canvas,&header,&controls},*controls_children[]={&button};
static lv_obj_t *active_screen=&screen;
static lv_display_t display={480,480};
static lv_indev_t indev={LV_INDEV_TYPE_POINTER,{240,180}};
static Self player;
static int show_calls,visible_calls,stop_calls,log_calls,checks;
#define CHECK(x) do {++checks;assert(x);} while(0)
void *lv_event_get_user_data(lv_event_t *e){return e->self;}
int lv_event_get_code(lv_event_t *e){return e->code;}
void *lv_event_get_target(lv_event_t *e){return e->target;}
void *lv_event_get_current_target(lv_event_t *e){return e->current;}
lv_obj_t *lv_screen_active(void){return active_screen;}
lv_display_t *lv_obj_get_display(lv_obj_t *o){return &display;}
int32_t lv_display_get_horizontal_resolution(lv_display_t *d){return d->width;}
int32_t lv_display_get_vertical_resolution(lv_display_t *d){return d->height;}
lv_indev_t *lv_event_get_indev(lv_event_t *e){return e->indev;}
int lv_indev_get_type(lv_indev_t *i){return i->type;}
void lv_indev_get_point(lv_indev_t *i,lv_point_t *p){*p=i->point;}
void lv_obj_get_coords(lv_obj_t *o,lv_area_t *a){*a=o->area;}
bool lv_obj_has_flag(lv_obj_t *o,int flag){return flag==LV_OBJ_FLAG_HIDDEN?o->hidden:(o->flags&flag)!=0;}
void lv_obj_remove_flag(lv_obj_t *o,int flag){o->flags&=~flag;}
void lv_obj_get_click_area(lv_obj_t *o,lv_area_t *area){*area=o->area;}
void lv_obj_transform_point(lv_obj_t *o,lv_point_t *p,int flag){}
int32_t lv_obj_get_ext_draw_size(lv_obj_t *o){return 0;}
void lv_area_increase(lv_area_t *a,int32_t x,int32_t y){a->x1-=x;a->x2+=x;a->y1-=y;a->y2+=y;}
bool lv_area_is_point_on(const lv_area_t *a,const lv_point_t *p,int radius){return p->x>=a->x1&&p->x<=a->x2&&p->y>=a->y1&&p->y<=a->y2;}
uint32_t lv_obj_get_child_count(lv_obj_t *o){return o->child_cnt;}
void lv_obj_send_event(lv_obj_t *o,int code,void *data){}
int lv_event_get_gesture_type(lv_event_t *e){return e->gesture;}
int lv_event_get_gesture_state(lv_event_t *e,int type){return e->state;}
float lv_event_get_pinch_scale(lv_event_t *e){return e->scale;}
void lv_event_stop_bubbling(lv_event_t *e){++stop_calls;}
void showChrome(Self *s){++show_calls;s->_chrome_visible=true;header.hidden=controls.hidden=false;}
void setChromeVisible(Self *s,bool v){++visible_calls;s->_chrome_visible=v;header.hidden=controls.hidden=!v;}
'''
code += '\nstatic bool lv_obj_hit_test(lv_obj_t *obj,const lv_point_t *point)' + body('bool lv_obj_hit_test(', (lvgl / 'core/lv_obj_pos.c').read_text('utf-8'))
code += '\nlv_obj_t *lv_indev_search_obj(lv_obj_t *obj,lv_point_t *point);'
search_body = body('lv_obj_t * lv_indev_search_obj(', (lvgl / 'indev/lv_indev.c').read_text('utf-8')).replace('obj->coords', 'obj->area')
code += '\nlv_obj_t *lv_indev_search_obj(lv_obj_t *obj,lv_point_t *point)' + search_body
# Run precisely the production container flag calls, not a hand-written intended
# configuration. LVGL's real hit search must still return an interactive child.
container_flags = re.findall(r'lv_obj_remove_flag\(_(?:header|controls), LV_OBJ_FLAG_(?:CLICKABLE|SCROLLABLE)\);', body('bool VideoPlayer::run()'))
code += '\nstatic void configure_chrome_input(void){' + ''.join(container_flags).replace('_header', '&header').replace('_controls', '&controls') + '}\n'
code += '\nstatic bool video_pinch_inside(int32_t x,int32_t y,int32_t left,int32_t top,int32_t right,int32_t bottom)' + convert(body('bool video_pinch_inside('))
code += '\nstatic int video_pinch_mode(float scale)' + convert(body('int video_pinch_mode('))
reset = body('void VideoPlayer::resetPinch()')
reset = re.sub(r'\b(_pinch_armed|_pinch_applied|_pinch_seen)\b', r'self->\1', reset)
code += '\nstatic void resetPinch(Self *self)' + reset
code += '\nstatic void pinchEvent(lv_event_t *event)' + convert(body('void VideoPlayer::pinchEvent('))
code += '\nstatic void chromeEvent(lv_event_t *event)' + convert(body('void VideoPlayer::chromeEvent('))
code += r'''
static void fresh(void){
 memset(&player,0,sizeof(player));player._foreground=true;player._frame_presented=true;player._state=STATE_Playing;
 player._canvas=&canvas;player._viewport=&viewport;player._screen=&screen;player._header=&header;player._controls=&controls;
 viewport.area=(lv_area_t){0,0,479,371};header.area=(lv_area_t){20,22,459,63};controls.area=(lv_area_t){65,210,414,363};
 canvas.area=viewport.area;button.area=(lv_area_t){220,270,260,310};
 viewport.flags=canvas.flags=header.flags=controls.flags=button.flags=LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE;
 static struct {lv_obj_t **children;} vp_spec,ctrl_spec;vp_spec.children=viewport_children;ctrl_spec.children=controls_children;
 viewport.spec_attr=(void *)&vp_spec;viewport.child_cnt=3;controls.spec_attr=(void *)&ctrl_spec;controls.child_cnt=1;
 configure_chrome_input();
 header.hidden=controls.hidden=true;active_screen=&screen;indev.point=(lv_point_t){240,180};indev.type=LV_INDEV_TYPE_POINTER;
 display=(lv_display_t){480,480};
 show_calls=visible_calls=stop_calls=log_calls=0;
}
static void emit(int code,int state,float scale){lv_event_t e={&player,code,&canvas,&canvas,&indev,LV_INDEV_GESTURE_PINCH,state,scale};pinchEvent(&e);}
static void click(void){lv_event_t e={&player,LV_EVENT_CLICKED,&canvas,&canvas,&indev,0,0,1};chromeEvent(&e);}
static lv_obj_t *route(int code,int state,float scale){
 lv_obj_t *target=lv_indev_search_obj(&viewport,&indev.point);
 if(target==&canvas||target==&viewport){lv_event_t e={&player,code,target,target,&indev,LV_INDEV_GESTURE_PINCH,state,scale};pinchEvent(&e);}
 return target;
}
int main(void){
 // Regression: expand, lift both fingers, then start the inward pinch in blank
 // controls. Fill makes chrome visible; this must not steal the next gesture.
 fresh();route(LV_EVENT_PRESSED,0,1);route(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,1.3f);
 CHECK(player._scale_mode==HOSINO_VIDEO_FILL&&!controls.hidden);
 route(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_ENDED,1.3f);route(LV_EVENT_RELEASED,0,1);click();
 indev.point=(lv_point_t){80,300};CHECK(route(LV_EVENT_PRESSED,0,1)==&canvas);CHECK(player._pinch_armed);
 route(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,.7f);CHECK(player._scale_mode==HOSINO_VIDEO_FIT&&log_calls==2);
 route(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_ENDED,.7f);route(LV_EVENT_RELEASED,0,1);click();CHECK(visible_calls==0);
 indev.point=(lv_point_t){300,50};CHECK(route(LV_EVENT_PRESSED,0,1)==&canvas&&player._pinch_armed);
 route(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,1.3f);CHECK(player._scale_mode==HOSINO_VIDEO_FILL&&log_calls==3);
 route(LV_EVENT_RELEASED,0,1);CHECK(!player._pinch_armed);
 indev.point=(lv_point_t){240,290};CHECK(route(LV_EVENT_PRESSED,0,1)==&button);CHECK(!player._pinch_armed);
 CHECK(lv_obj_has_flag(&button,LV_OBJ_FLAG_CLICKABLE)); // children keep their own hit targets
 route(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,.7f);CHECK(player._scale_mode==HOSINO_VIDEO_FILL&&log_calls==3);
 lv_event_t control_bubble={&player,LV_EVENT_GESTURE,&button,&viewport,&indev,LV_INDEV_GESTURE_PINCH,LV_INDEV_GESTURE_STATE_RECOGNIZED,.7f};pinchEvent(&control_bubble);
 CHECK(player._scale_mode==HOSINO_VIDEO_FILL&&log_calls==3&&!player._pinch_armed);
 CHECK(video_pinch_inside(24,24,0,0,479,371));CHECK(video_pinch_inside(455,347,0,0,479,371));
 CHECK(!video_pinch_inside(23,200,0,0,479,371));CHECK(!video_pinch_inside(456,200,0,0,479,371));
 CHECK(!video_pinch_inside(240,23,0,0,479,371));CHECK(!video_pinch_inside(240,348,0,0,479,371));
 CHECK(!video_pinch_inside(24,24,0,0,47,371));CHECK(!video_pinch_inside(24,24,0,0,479,47));
 CHECK(video_pinch_mode(1.15f)==-1);CHECK(video_pinch_mode(.85f)==-1);CHECK(video_pinch_mode(1)==-1);
 CHECK(video_pinch_mode(1.151f)==HOSINO_VIDEO_FILL);CHECK(video_pinch_mode(.849f)==HOSINO_VIDEO_FIT);
 CHECK(video_pinch_mode(NAN)==-1);CHECK(video_pinch_mode(INFINITY)==-1);CHECK(video_pinch_mode(-INFINITY)==-1);CHECK(video_pinch_mode(0)==-1);
 fresh();emit(LV_EVENT_PRESSED,0,1);CHECK(player._pinch_armed&&!player._pinch_seen);
 emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,1.2f);CHECK(player._scale_mode==HOSINO_VIDEO_FILL);CHECK(player._pinch_seen&&player._pinch_applied);CHECK(show_calls==1&&stop_calls==1&&log_calls==1);
 emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,.7f);CHECK(player._scale_mode==HOSINO_VIDEO_FILL&&show_calls==1&&log_calls==1);
 emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_ENDED,1.2f);CHECK(!player._pinch_armed&&!player._pinch_applied&&player._pinch_seen);
 emit(LV_EVENT_RELEASED,0,1);click();CHECK(show_calls==1&&visible_calls==0); // no post-pinch chrome toggle
 lv_event_t bubbled={&player,LV_EVENT_CLICKED,&canvas,&screen,&indev,0,0,1};chromeEvent(&bubbled);CHECK(visible_calls==0); // original target survives bubble to screen
 bubbled.target=&screen;chromeEvent(&bubbled);CHECK(visible_calls==1); // distinct screen-area tap stays usable
 player._chrome_visible=true;visible_calls=0;
 emit(LV_EVENT_PRESSED,0,1);CHECK(!player._pinch_seen&&player._pinch_armed);click();CHECK(visible_calls==1&&!player._chrome_visible);
 emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,.8f);CHECK(player._scale_mode==HOSINO_VIDEO_FIT&&show_calls==2);
 emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_CANCELED,.8f);CHECK(!player._pinch_armed&&!player._pinch_applied);click();CHECK(visible_calls==1);
 fresh();emit(LV_EVENT_PRESSED,0,1);emit(LV_EVENT_PRESS_LOST,0,1);emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,2);CHECK(player._scale_mode==HOSINO_VIDEO_ORIGINAL&&show_calls==0);
 fresh();emit(LV_EVENT_PRESSED,0,1);emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_ONGOING,2);CHECK(!player._pinch_seen&&!player._pinch_applied);
 emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,1.1f);CHECK(player._pinch_seen&&!player._pinch_applied&&show_calls==0);
 emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,NAN);CHECK(player._scale_mode==0&&show_calls==0);
 emit(LV_EVENT_RELEASED,0,1);CHECK(!player._pinch_armed);emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,2);CHECK(player._scale_mode==0);
 for(int edge=0;edge<4;edge++){fresh();indev.point=edge==0?(lv_point_t){23,180}:edge==1?(lv_point_t){456,180}:edge==2?(lv_point_t){240,23}:(lv_point_t){240,348};emit(LV_EVENT_PRESSED,0,1);CHECK(!player._pinch_armed);emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,2);CHECK(!player._pinch_applied&&show_calls==0);}
 for(int bad=0;bad<8;bad++){fresh();if(bad==0)player._foreground=false;if(bad==1)player._browser_visible=true;if(bad==2)player._frame_presented=false;if(bad==3)player._seek_dragging=true;if(bad==4)player._volume_dragging=true;if(bad==5)player._state=STATE_Preparing;if(bad==6)active_screen=&other_screen;if(bad==7)indev.type=LV_INDEV_TYPE_KEYPAD;emit(LV_EVENT_PRESSED,0,1);CHECK(!player._pinch_armed);emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,2);CHECK(player._scale_mode==0&&show_calls==0);}
 fresh();header.hidden=false;indev.point=(lv_point_t){240,40};route(LV_EVENT_PRESSED,0,1);CHECK(player._pinch_armed);
 fresh();controls.hidden=false;indev.point=(lv_point_t){240,300};CHECK(route(LV_EVENT_PRESSED,0,1)==&button);CHECK(!player._pinch_armed);
 for(int edge=0;edge<4;edge++){fresh();viewport.area=(lv_area_t){0,0,479,479};indev.point=edge==0?(lv_point_t){39,180}:edge==1?(lv_point_t){440,180}:edge==2?(lv_point_t){240,39}:(lv_point_t){240,440};emit(LV_EVENT_PRESSED,0,1);CHECK(!player._pinch_armed);}
 fresh();indev.point=(lv_point_t){40,180};emit(LV_EVENT_PRESSED,0,1);CHECK(player._pinch_armed);
 fresh();indev.point=(lv_point_t){439,180};emit(LV_EVENT_PRESSED,0,1);CHECK(player._pinch_armed);
 fresh();display.width=80;emit(LV_EVENT_PRESSED,0,1);CHECK(!player._pinch_armed);
 fresh();lv_event_t e={&player,LV_EVENT_PRESSED,&button,&viewport,&indev,0,0,1};pinchEvent(&e);CHECK(!player._pinch_armed); // controls cannot arm via bubbling
 e.target=&canvas;pinchEvent(&e);CHECK(!player._pinch_armed); // parent duplicate cannot arm
 e.current=&canvas;e.indev=NULL;pinchEvent(&e);CHECK(!player._pinch_armed);
 fresh();emit(LV_EVENT_PRESSED,0,1);e=(lv_event_t){&player,LV_EVENT_GESTURE,&canvas,&canvas,&indev,LV_INDEV_GESTURE_ROTATE,LV_INDEV_GESTURE_STATE_RECOGNIZED,2};pinchEvent(&e);CHECK(!player._pinch_seen&&player._scale_mode==0);
 player._foreground=false;emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,2);CHECK(player._scale_mode==0&&show_calls==0);
 fresh();player._state=STATE_Paused;emit(LV_EVENT_PRESSED,0,1);emit(LV_EVENT_GESTURE,LV_INDEV_GESTURE_STATE_RECOGNIZED,2);CHECK(player._scale_mode==HOSINO_VIDEO_FILL&&show_calls==1);
 resetPinch(&player);CHECK(!player._pinch_armed&&!player._pinch_applied&&!player._pinch_seen);
 printf("PASS actual video pinch policies: %d assertions; content-only admission, one change per gesture, lifecycle and no synthesized click toggle\n",checks);
 return 0;
}
'''
assert 'lv_obj_add_event_cb(_screen, pinchEvent' not in source
assert 'resetPinch();' in body('void VideoPlayer::requestStop()')
assert 'resetPinch();' in body('void VideoPlayer::destroyUi()')
assert 'resetPinch();' in body('void VideoPlayer::showBrowser()')
with tempfile.TemporaryDirectory(prefix='hs_video_pinch_') as work:
    work = Path(work)
    (work / 'test.c').write_text(code, 'utf-8')
    built = subprocess.run([str(compiler), '-Wall', 'test.c', '-o', 'test.exe'], cwd=work, capture_output=True, text=True)
    if built.returncode:
        print(built.stdout + built.stderr)
        raise SystemExit(built.returncode)
    ran = subprocess.run([str(work / 'test.exe')], cwd=work, capture_output=True, text=True)
    print(ran.stdout + ran.stderr, end='')
    if args.expect_regression_failure:
        assert ran.returncode != 0, 'Frozen v21 unexpectedly passed the v22 hit-routing regression'
    else:
        assert ran.returncode == 0
result = {'passed': ran.returncode == 0, 'expected_regression_failure': args.expect_regression_failure,
          'scope': __doc__, 'source_sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
          'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          'compile_exit_code': built.returncode, 'test_exit_code': ran.returncode, 'output': ran.stdout.strip(),
          'source_checks': ['No screen-wide pinch handler', 'Stop, browser and UI destruction reset pinch state'],
          'production_container_flag_calls': container_flags,
          'lvgl_hit_search_sha256': hashlib.sha256((lvgl / 'indev/lv_indev.c').read_bytes()).hexdigest(),
          'lvgl_hit_test_sha256': hashlib.sha256((lvgl / 'core/lv_obj_pos.c').read_bytes()).hexdigest()}
suffix = 'baseline' if args.expect_regression_failure else 'host'
(ROOT / f'evidence/video-pinch-v22-{suffix}-20261114.json').write_text(json.dumps(result, indent=2) + '\n', 'utf-8')
