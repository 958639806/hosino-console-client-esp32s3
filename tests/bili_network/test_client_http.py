"""Run verbatim production transfer/error/notice helpers with pinned real cJSON.

HTTP calls model the documented API boundary. This is not TLS/network/device
acceptance; the open/write/read counters verify no POST replay on -EAGAIN.
"""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
TCC=ROOT/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'
IDF=Path('C:/Users/95863/.codex/builds/hosino-esp32-qspi/esp-idf-v5.5.5')

def section(source, name):
    return source.split('// HOST_'+name+'_BEGIN',1)[1].split('// HOST_'+name+'_END',1)[0].split('\n',1)[1]

def main():
    client=(ROOT/'overlay-apps/components/BiliBili/BiliClient.cpp').read_text('utf-8')
    ui=(ROOT/'overlay-apps/components/BiliBili/BiliBili.cpp').read_text('utf-8')
    helpers=section(client,'HTTP_BOUNDARY')+section(client,'ERROR_BOUNDARY')+section(ui,'NOTICE_BOUNDARY')
    fixture=(ROOT/'tests/bili_network/test_client_http.c').read_text('utf-8').replace('/* PRODUCTION_HELPERS */',helpers)
    assert 'transferResponse(http,body,length,buffer,ResponseLimit,deadline,requestActive,&activity,&trace)' in client
    assert 'shouldClearOldNotice(self->_noticeText,previousModelError)' in ui
    with tempfile.TemporaryDirectory(prefix='bili_http_client_') as folder:
        folder=Path(folder);(folder/'test.c').write_text(fixture,'utf-8')
        for name in ('cJSON.c','cJSON.h'):
            shutil.copy2(IDF/'components/json/cJSON'/name,folder)
        compiled=subprocess.run([str(TCC),'-Wall','-I.','test.c','cJSON.c','-o','test.exe'],cwd=folder,capture_output=True,text=True)
        run=None
        if compiled.returncode==0:run=subprocess.run([str(folder/'test.exe')],cwd=folder,capture_output=True,text=True,timeout=10)
    report={'scope':'Actual production helpers and pinned real cJSON; deterministic HTTP API boundary; not device or real TLS validation',
            'production_client_sha256':hashlib.sha256(client.encode()).hexdigest(),
            'production_ui_sha256':hashlib.sha256(ui.encode()).hexdigest(),
            'compile':{'exit_code':compiled.returncode,'stdout':compiled.stdout,'stderr':compiled.stderr},
            'run':None if run is None else {'exit_code':run.returncode,'stdout':run.stdout,'stderr':run.stderr},
            'passed':compiled.returncode==0 and run is not None and run.returncode==0}
    target=ROOT/'evidence/bili-dns-error-candidate-20261115/http-host.json';target.parent.mkdir(exist_ok=True);target.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n','utf-8')
    print(json.dumps(report,ensure_ascii=False,indent=2));return 0 if report['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
