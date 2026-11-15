"""Exercise actual bounded numeric diagnostics and playback identity guards."""
from pathlib import Path
import hashlib,json,shutil,subprocess,tempfile

ROOT=Path(__file__).resolve().parents[2]
path=ROOT/'overlay-apps/components/BiliBili/BiliClient.cpp'
source=path.read_text('utf-8')
helper=source.split('// HOST_PREPARE_DIAGNOSTICS_BEGIN',1)[1].split('// HOST_PREPARE_DIAGNOSTICS_END',1)[0].replace('nullptr','NULL')
fixture=r'''
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include "cJSON.h"
static char logline[600];static const char *RequestTag="test";
static void capture(const char *tag,const char *format,...) {va_list v;va_start(v,format);vsnprintf(logline,sizeof(logline),format,v);va_end(v);}
#define ESP_LOGI capture
/* HELPERS */
int main(void) {
 cJSON *d=cJSON_Parse("{\"target_video_kbps\":800,\"sample_video_kbps\":920.4,\"selected_quality\":31,\"diagnostics\":{\"sample_attempts\":[{\"quality\":4},{\"quality\":31}]},\"session_id\":\"PRIVATE_SECRET\",\"url\":\"https://PRIVATE_SECRET\"}");
 assert(d);logPreparation(d,"error",0,800);
 assert(strstr(logline,"profile=standard requested_kbps=800 target_kbps=800 sample_kbps=920 quality=31 attempts=2 last_quality=31"));
 assert(!strstr(logline,"PRIVATE_SECRET")&&!strstr(logline,"https://"));
 assert(prepareNumber(d,"missing")==-1&&prepareNumber(NULL,"quality")==-1);
 cJSON_Delete(d);
 d=cJSON_Parse("{\"target_video_kbps\":-1,\"sample_video_kbps\":1000001,\"selected_quality\":\"PRIVATE_SECRET\",\"diagnostics\":{\"sample_attempts\":[1,2,3,4,5,6,7,8,9]}}");
 logPreparation(d,"ready",1,0);
 assert(strstr(logline,"profile=clear requested_kbps=0 target_kbps=-1 sample_kbps=-1 quality=-1 attempts=-1 last_quality=-1"));
 assert(!strstr(logline,"PRIVATE_SECRET"));cJSON_Delete(d);
 assert(playbackIdentityMatches("BV_OLD","BV_OLD"));
 assert(!playbackIdentityMatches("BV_OLD","BV_NEW"));
 assert(!playbackIdentityMatches("","BV_NEW")&&!playbackIdentityMatches(NULL,"BV_NEW"));
 puts("PRODUCTION_NUMERIC_PREPARE_DIAGNOSTIC_REDACTION_AND_PLAY_IDENTITY_PASS");return 0;
}
'''.replace('/* HELPERS */',helper)
with tempfile.TemporaryDirectory(prefix='bili_prepare_diag_') as d:
 d=Path(d);(d/'test.c').write_text(fixture,'utf-8')
 for name in ('cJSON.c','cJSON.h'):
  shutil.copy2(Path('C:/Users/95863/.codex/builds/hosino-esp32-qspi/esp-idf-v5.5.5/components/json/cJSON')/name,d)
 tcc=ROOT/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'
 compiled=subprocess.run([str(tcc),'-Wall','test.c','cJSON.c','-o','test.exe'],cwd=d,capture_output=True,text=True)
 assert compiled.returncode==0,compiled.stderr
 run=subprocess.run([str(d/'test.exe')],capture_output=True,text=True,timeout=5)
 report={'passed':run.returncode==0,'scope':'Actual production cJSON numeric diagnostics and identity guard; logging boundary and stale-video rejection, not server/device playback acceptance.','source_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'compile_exit':compiled.returncode,'compile_diagnostics':compiled.stdout+compiled.stderr,'exit_code':run.returncode,'stdout':run.stdout,'stderr':run.stderr}
 out=ROOT/'evidence/bili-ui-gesture-20261115';out.mkdir(exist_ok=True);(out/'diagnostics-host.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n','utf-8')
 print(json.dumps(report,ensure_ascii=False));raise SystemExit(run.returncode)
