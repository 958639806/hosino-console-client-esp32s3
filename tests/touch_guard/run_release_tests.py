"""Execute the complete production touch wrapper against explicit IRQ/IO bounds."""
from pathlib import Path
import hashlib,importlib.util,json,subprocess,tempfile

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('touch_stub',Path(__file__).with_name('run_tests.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
source=ROOT/'overlay-apps/main/hosino_touch_override.c'
with tempfile.TemporaryDirectory(prefix='touch-release-',dir='C:/esp/hosino-qspi') as temp:
 work=Path(temp);(work/'stub.h').write_text(module.STUB,'utf-8')
 for name in ['freertos/FreeRTOS.h','freertos/task.h','esp_attr.h','esp_heap_caps.h','esp_lcd_io_i2c.h','esp_lcd_panel_io.h','esp_lcd_touch.h','esp_lv_adapter.h','esp_lv_adapter_input.h','esp_log.h','esp_timer.h']:
  p=work/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "stub.h"\n','utf-8')
 for name in ['hosino_touch_override.c','hosino_touch_packet.h']:(work/name).write_bytes((ROOT/'overlay-apps/main'/name).read_bytes())
 for name in ['test_touch_v21.c','test_touch_release.c']:(work/name).write_bytes(Path(__file__).with_name(name).read_bytes())
 compiled=subprocess.run([str(module.TCC),'-Wall','-I'+str(work),str(work/'test_touch_release.c'),'-o',str(work/'test.exe')],capture_output=True,text=True)
 assert compiled.returncode==0,compiled.stdout+compiled.stderr
 run=subprocess.run([str(work/'test.exe'),str(work/'states.txt')],capture_output=True,text=True,timeout=10)
 report={'scope':'Complete actual production wrapper/parser, deterministic pinned-API boundary with IRQ semaphore/observer generation and LVGL read-service model; not real I2C, native adapter implementation or board acceptance.','source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'compile_exit':compiled.returncode,'compile_diagnostics':compiled.stdout+compiled.stderr,'exit_code':run.returncode,'stdout':run.stdout,'stderr':run.stderr,'passed':run.returncode==0}
 out=ROOT/'evidence/touch-release-v23-20261115';out.mkdir(exist_ok=True)
 if (work/'states.txt').exists():
  trace=(work/'states.txt').read_bytes();(out/'synthetic-filtered-states.txt').write_bytes(trace)
  report['synthetic_state_trace_sha256']=hashlib.sha256(trace).hexdigest()
 (out/'host.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n','utf-8')
 print(json.dumps(report,ensure_ascii=False));raise SystemExit(run.returncode)
