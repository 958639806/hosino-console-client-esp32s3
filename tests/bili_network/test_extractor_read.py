"""Exercise the production extractor read function with deterministic IO errors."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
REL = 'components/esp_player_compat/esp_player/src/extractor/player_extractor_io.c'
SOURCE = ROOT / 'overlay-apps' / REL
TCC = ROOT / 'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'

def read_function(text):
    start = text.index('int _extractor_read(')
    end = text.index('\nint _extractor_seek(', start)
    return text[start:end]

STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_gmf_err_io_t;
typedef void *esp_gmf_element_handle_t;
typedef struct { bool _is_stop; void *input_handle,*sync_handle; bool hosino_managed_mjpeg_http; } esp_player_stream_t;
typedef struct {uint8_t *buf;uint32_t buf_length,valid_size;bool is_done;} esp_gmf_payload_t;
enum {ESP_PLAYER_ERR_OK=0,ESP_GMF_IO_OK=0,ESP_GMF_IO_FAIL=-1,ESP_GMF_IO_ABORT=-2,ESP_GMF_IO_TIMEOUT=-3};
#define ESP_PLAYER_READ_WAIT_TIME_MS 20
#define pdMS_TO_TICKS(x) (x)
#define PLAYER_MANAGED_READ_US (8000000LL)
static int64_t esp_timer_get_time(void){return 0;}
static int player_managed_read(void *b,uint32_t n,esp_player_stream_t *s,int64_t d){assert(0);return -1;}
static uint32_t player_extractor_startup_read_limit(void *e,uint32_t n,uint32_t *w){return n;}
static void player_extractor_startup_note_read(void *e,uint32_t n){}
static unsigned calls, mode;
static bool open_failed;
static int player_ensure_input_open(esp_player_stream_t *s){return open_failed?-1:0;}
static void *player_extractor_el(esp_player_stream_t *s){return NULL;}
static uint64_t player_sync_get_audio_decode_pts_ms(void *s){return 0;}
static uint64_t player_sync_get_audio_render_pts_ms(void *s){return 0;}
static void player_extractor_raw_read_pace(esp_player_stream_t *s,void *e,uint64_t n){}
static void player_extractor_get_last_pts(void *s,uint64_t *p){*p=0;}
static int esp_gmf_io_acquire_read(void *h,esp_gmf_payload_t *p,unsigned n,unsigned ticks){
    ++calls;assert(calls<=8);p->valid_size=0;p->is_done=false;
    if(mode==0)return ESP_GMF_IO_FAIL;
    if(mode==1)return ESP_GMF_IO_ABORT;
    if(mode==2)return ESP_GMF_IO_TIMEOUT;
    if(mode==3 && calls==2)return ESP_GMF_IO_FAIL;
    if(mode==5){p->is_done=true;return 0;}
    assert(n>=2);memcpy(p->buf,"ok",2);p->valid_size=2;return 0;
}
static void esp_gmf_io_release_read(void *h,void *p,unsigned ticks){}
'''
TEST = r'''
int main(void){
    esp_player_stream_t stream={0};char buffer[8]={0};
    mode=0;calls=0;assert(_extractor_read(buffer,4,&stream)==-1);assert(calls==1);
    mode=1;calls=0;assert(_extractor_read(buffer,4,&stream)==-1);assert(calls==1);
    mode=0;calls=0;stream._is_stop=true;assert(_extractor_read(buffer,4,&stream)==0);assert(calls==1);
    mode=2;calls=0;assert(_extractor_read(buffer,4,&stream)==0);assert(calls==1);
    stream._is_stop=false;mode=3;calls=0;assert(_extractor_read(buffer,4,&stream)==2);assert(calls==2);
    mode=4;calls=0;assert(_extractor_read(buffer,4,&stream)==4);assert(calls==2);assert(!memcmp(buffer,"okok",4));
    mode=5;calls=0;assert(_extractor_read(buffer,4,&stream)==0);assert(calls==1);
    mode=4;calls=0;open_failed=true;assert(_extractor_read(buffer,4,&stream)==-1);assert(calls==0);
    puts("extractor read: 8 failure/abort/stop/partial-data/EOF cases passed");return 0;
}
'''

with zipfile.ZipFile(ROOT / 'evidence/apps-v22-observed/staged-source-inputs.zip') as archive:
    baseline = archive.read('project-apps/' + REL).decode('utf-8')
current = SOURCE.read_text('utf-8')
runs = []
with tempfile.TemporaryDirectory(prefix='bili_extractor_read_') as folder:
    work = Path(folder)
    for label, text, expected_success in [('v22_zero_byte_failure_baseline', baseline, False), ('v23_candidate', current, True)]:
        code = work / 'test.c'
        code.write_text(STUBS + read_function(text) + TEST, 'utf-8')
        compiled = subprocess.run([str(TCC), '-Wall', str(code), '-o', str(work / 'test.exe')], capture_output=True, text=True)
        assert compiled.returncode == 0, compiled.stderr
        run = subprocess.run([str(work / 'test.exe')], capture_output=True, text=True, timeout=5)
        assert (run.returncode == 0) == expected_success, (label, run.stdout, run.stderr)
        runs.append({'label': label, 'expected_success': expected_success, 'exit_code': run.returncode,
                     'stdout': run.stdout.strip(), 'stderr': run.stderr.strip(),
                     'source_sha256': hashlib.sha256(text.encode()).hexdigest()})
report = {'passed': True, 'scope': 'Actual production read function with deterministic IO; not hardware/network acceptance', 'runs': runs}
(ROOT / 'evidence/bili-extractor-read-host-v23-20261114.json').write_text(json.dumps(report, indent=2) + '\n', 'utf-8')
print(json.dumps({'passed': True, 'baseline_failure_reproduced': runs[0]['exit_code'] != 0, 'candidate': runs[1]['stdout']}))
