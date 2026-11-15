"""Run real pinned LVGL pointer events with production presentation guards."""
from pathlib import Path
import hashlib,json,subprocess,tempfile

ROOT=Path(__file__).resolve().parents[2]
LVGL=Path('C:/esp/hosino-qspi/project-apps/managed_components/lvgl__lvgl')
TCC=ROOT/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'
SOURCE=ROOT/'overlay-apps/components/BiliBili/BiliBili.cpp'
text=SOURCE.read_text('utf-8')
helpers=text[text.index('// HOST_PRESENTATION_BOUNDARY_BEGIN'):text.index('// HOST_PRESENTATION_BOUNDARY_END')].replace('nullptr','NULL')
# Assert actual call sites preserve the displayed model as well as its objects.
assert 'mayApplySnapshot(pressed,self->_view==View::Player&&self->_presentedView==View::Player)&&self->_worker->copy(*self->_model)' in text
assert 'if(self->_rebuild&&mayRebuild(pressed)) self->render();' in text
assert 'if(!mayRebuild(presentationBusy(_body))) { _rebuild=true; return; }' in text
assert 'if(_presentedView==View::FavoritePick)' in text
fixture=Path(__file__).with_suffix('.c').read_text('utf-8').replace('/* PRODUCTION_PRESENTATION_GUARDS */',helpers)
cache=Path('C:/esp/hosino-qspi/bili-native-ui-test');cache.mkdir(exist_ok=True)
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
out=ROOT/'evidence/bili-ui-gesture-20261115';out.mkdir(exist_ok=True)
report={'source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),'scope':'Real pinned LVGL desktop source, pointer read callbacks and press/move/release event dispatch; production guards inserted verbatim (nullptr converted to C NULL). Presentation/model shell is a test harness, not complete Brookesia/ESP firmware.','compile_exit':build.returncode,'compile_diagnostics':build.stdout+build.stderr}
if build.returncode==0:
 trace=ROOT/'evidence/touch-release-v23-20261115/synthetic-filtered-states.txt'
 run=subprocess.run([str(cache/'test.exe')]+([str(trace)] if trace.exists() else []),capture_output=True,text=True,timeout=15)
 report.update(exit_code=run.returncode,stdout=run.stdout,stderr=run.stderr,passed=run.returncode==0)
else:report['passed']=False
(out/'native-host.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n','utf-8')
print(json.dumps(report,ensure_ascii=False))
raise SystemExit(not report['passed'])
