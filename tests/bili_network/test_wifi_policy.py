"""Test production PS lease and mechanically translated C++ adapter bodies.

Only C++ references/casts/zero-init spelling are converted for Windows TCC;
actual Xtensa checks compile the unchanged C++ source separately.
"""
from pathlib import Path
import hashlib,json,re,subprocess,tempfile

ROOT=Path(__file__).resolve().parents[2]
source=ROOT/'overlay-apps/main/hosino_bili_media.cpp'
client=ROOT/'overlay-apps/components/BiliBili/BiliClient.cpp'
header=source.with_name('hosino_bili_wifi_policy.h')
cpp=source.read_text('utf-8');policy=header.read_text('utf-8')
policy='\n'.join(x for x in policy.splitlines() if not x.startswith('#include') and not x.startswith('#pragma'))
functions=[]
for name in ('stop','open','pause','volume','snapshot'):
 start=cpp.index('esp_err_t '+name+'(');end=cpp.index('\n}',start)+2
 functions.append(cpp[start:end])
body='\n'.join(functions)
body=body.replace('auto &ctx = *static_cast<MediaContext *>(opaque);','MediaContext *ctx=(MediaContext *)opaque;').replace('ctx.','ctx->')
body=body.replace('const BiliPlaybackRequest &request','const BiliPlaybackRequest *request').replace('request.','request->')
body=body.replace('BiliMediaSnapshot &out','BiliMediaSnapshot *out').replace('out = {};','memset(out,0,sizeof(*out));').replace('out.','out->')
body=body.replace('config{}','config={0}').replace('value{}','value={0}').replace('nullptr','NULL')
assert 'hosino_bili_wifi_enter(&ctx.wifi)' in cpp and '(void)hosino_bili_wifi_leave(&ctx.wifi);' in cpp
assert cpp.index('hosino_media_player_open_network(')<cpp.index('hosino_bili_wifi_enter(&ctx.wifi)')<cpp.index('hosino_media_player_start(')
client_body=client.read_text('utf-8');start=client_body.index('void stopMedia(');end=client_body.index('\n}',start)+2
worker_stop=client_body[start:end].replace('Worker &w','Worker *w').replace('Model &m','Model *m').replace('bool deleteSession=true','bool deleteSession')
worker_stop=worker_stop.replace('w.frames.store(false);','w.frames=false;').replace('w.','w->').replace('m.','m->').replace('nullptr','NULL')
fixture=Path(__file__).with_suffix('.c').read_text('utf-8').replace('/* PRODUCTION_POLICY */',policy).replace('/* PRODUCTION_ADAPTER_BODIES */',body).replace('/* PRODUCTION_WORKER_STOP */',worker_stop)
with tempfile.TemporaryDirectory(prefix='bili_wifi_policy_') as d:
 d=Path(d);(d/'test.c').write_text(fixture,'utf-8')
 tcc=ROOT/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'
 build=subprocess.run([str(tcc),'-Wall',str(d/'test.c'),'-o',str(d/'test.exe')],capture_output=True,text=True)
 assert build.returncode==0,build.stderr
 run=subprocess.run([str(d/'test.exe')],capture_output=True,text=True,timeout=10)
 report={'passed':run.returncode==0,'scope':'Actual policy functions and mechanical C translation of actual adapter and Worker stopMedia C++ bodies against deterministic WiFi/media APIs; not radio/coexistence/device validation.','source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'policy_sha256':hashlib.sha256(header.read_bytes()).hexdigest(),'client_sha256':hashlib.sha256(client.read_bytes()).hexdigest(),'compile_exit':build.returncode,'compile_diagnostics':build.stdout+build.stderr,'exit_code':run.returncode,'stdout':run.stdout,'stderr':run.stderr}
 out=ROOT/'evidence/bili-network-window-20261115';out.mkdir(exist_ok=True);(out/'host.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n','utf-8')
 print(json.dumps(report,ensure_ascii=False));raise SystemExit(run.returncode)
