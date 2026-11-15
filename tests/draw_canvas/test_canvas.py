"""Real pinned LVGL raster + pointer-event test of extracted production helpers.

This is not the whole Brookesia application or physical touch acceptance.
"""
from pathlib import Path
import hashlib,json,re,subprocess

ROOT=Path(__file__).resolve().parents[2]
LVGL=Path('C:/esp/hosino-qspi/project-apps/managed_components/lvgl__lvgl')
TCC=ROOT/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'
SOURCE=ROOT/'overlay-apps/components/draw/Drawpanel.cpp'
HEADER=SOURCE.with_suffix('.hpp')
text=SOURCE.read_text('utf-8');header=HEADER.read_text('utf-8')
helpers=text.split('// HOST_DRAW_CANVAS_BEGIN',1)[1].split('// HOST_DRAW_CANVAS_END',1)[0]
state=header.split('// HOST_DRAW_STATE_BEGIN',1)[1].split('// HOST_DRAW_STATE_END',1)[0]
flags=text.split('static void configureFixedDrawObject',1)[1].split('\nDrawpanel *Drawpanel::_instance',1)[0]
flags='static void configureFixedDrawObject'+flags
# Actual application call-site/lifetime checks supplement the native event shell.
assert 'lv_obj_create(' not in helpers and 'lv_obj_delete(' not in helpers
assert 'heap_caps_aligned_alloc(alignment, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)' in text
assert 'width > 480' in text and 'height > 480' in text
assert 'lv_obj_get_child_cnt' not in text and '_dots' not in text
for method in ['pause','resume']:
    body=text.split('bool Drawpanel::'+method+'()',1)[1].split('\n}',1)[0]
    assert 'breakDrawStroke(&_stroke)' in body
close=text.split('bool Drawpanel::close(void)',1)[1].split('\nbool Drawpanel::init',1)[0]
assert close.index('breakDrawStroke')<close.index('lv_obj_delete(_stroke.canvas)')<close.index('heap_caps_free(_pixels)')
assert 'lv_obj_remove_event_cb_with_user_data(_panel_obj, touch_event_cb, this)' in close
assert 'lv_event_get_target(e) != app->_panel_obj' in text
fixture=Path(__file__).with_suffix('.c').read_text('utf-8')
fixture=fixture.replace('/* PRODUCTION_STATE */',state).replace('/* PRODUCTION_HELPERS */',helpers).replace('/* PRODUCTION_FLAGS */',flags).replace('nullptr','NULL')
cache=Path('C:/esp/hosino-qspi/draw-native-canvas-test');cache.mkdir(exist_ok=True)
(cache/'lv_conf.h').write_text('''#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#define LV_USE_LOG 0
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_DRAW_SW 1
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#define LV_USE_GESTURE_RECOGNITION 1
#define LV_USE_FLOAT 1
#endif
''','utf-8')
(cache/'test.c').write_text(fixture,'utf-8')
sources=sorted((LVGL/'src').rglob('*.c'))
args=['-w','-DLV_CONF_INCLUDE_SIMPLE','-I'+str(cache),'-I'+str(LVGL),str(cache/'test.c')]+[str(x) for x in sources]+['-o',str(cache/'test.exe')]
(cache/'args.txt').write_text('\n'.join('"'+x.replace('\\','/')+'"' for x in args),'utf-8')
build=subprocess.run([str(TCC),'@'+str(cache/'args.txt')],capture_output=True,text=True,timeout=60)
out=ROOT/'evidence/paint-canvas-20261115';out.mkdir(exist_ok=True)
report={'source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),'header_sha256':hashlib.sha256(HEADER.read_bytes()).hexdigest(),
 'fixture_sha256':hashlib.sha256(Path(__file__).with_suffix('.c').read_bytes()).hexdigest(),'lvgl_c_sources':len(sources),
 'scope':'Actual production stroke helpers/flags/state inserted verbatim in C shell; native pinned LVGL pointer events, raster pixels, clipping canaries, constant widget count. Brookesia lifecycle call sites checked statically, not a complete app/ESP or physical touch test.',
 'compile_exit':build.returncode,'compile_diagnostics':build.stdout+build.stderr}
if build.returncode==0:
 run=subprocess.run([str(cache/'test.exe')],capture_output=True,text=True,timeout=25)
 report.update(exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr,passed=run.returncode==0)
else: report['passed']=False
(out/'native-host.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n','utf-8')
print(json.dumps(report,ensure_ascii=False));raise SystemExit(not report['passed'])
