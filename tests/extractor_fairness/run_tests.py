"""Exercise production demux process/ownership against deterministic host ports.

No device/network access. The old v20 pre-read hold must reproduce no progress;
the candidate must continue through discarded independent frames to audio.
"""
from pathlib import Path
import hashlib,json,subprocess,tempfile,zipfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
SOURCE=ROOT/'overlay-apps/components/esp_player_compat/esp_player/src/extractor/player_extractor.c'
BASELINE=ROOT/'evidence/apps-v20-observed/staged-source-inputs.zip'
BASELINE_ENTRY='project-apps/components/esp_player_compat/esp_player/src/extractor/player_extractor.c'
BASELINE_SOURCE_SHA256='085aa97bd4eb4cb98c521b654ad002d851db3e1b60edf83bb94efcf38a294140'
TCC=ROOT/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'

def function(source,name):
    begin=source.index('{',source.index(name+'('))
    depth=0
    for end in range(begin,len(source)):
        if source[end]=='{':depth+=1
        elif source[end]=='}':
            depth-=1
            if depth==0:return source[begin:end+1]
    raise AssertionError(name)

def build_source(source,candidate):
    typedef_start=source.index('typedef struct {')
    typedef_end=source.index('} esp_player_extractor_t;',typedef_start)+len('} esp_player_extractor_t;')
    out='#define CANDIDATE '+str(int(candidate))+'\n'+(HERE/'host_stubs.h').read_text('utf-8')+'\n'
    if candidate:
        out+=(SOURCE.parent/'private_inc/hosino_avi_retry_clock.h').read_text('utf-8')+'\n'
    out+=source[typedef_start:typedef_end]+'\n'
    if candidate:
        for name,signature in [
            ('player_extractor_avi_retry_invalidate','static void player_extractor_avi_retry_invalidate(esp_player_extractor_t *extractor)'),
            ('player_extractor_avi_retry_prepare','static void player_extractor_avi_retry_prepare(esp_player_extractor_t *extractor,esp_gmf_element_handle_t self)'),
            ('player_extractor_avi_retry_note','static void player_extractor_avi_retry_note(esp_player_extractor_t *extractor,const esp_extractor_frame_info_t *frame,esp_extractor_err_t result)'),
            ('player_extractor_avi_retry_correct','static void player_extractor_avi_retry_correct(esp_player_extractor_t *extractor,esp_extractor_frame_info_t *frame)'),
        ]:out+=signature+function(source,name)+'\n'
        seek_source=source[source.index('\nesp_gmf_err_t player_extractor_seek('):]
        out+='static int player_extractor_seek(esp_gmf_element_handle_t handle,uint64_t time_pos)'+function(seek_source,'player_extractor_seek')+'\n'
        out+='static esp_gmf_job_err_t player_extractor_ops_close(esp_gmf_element_handle_t self,void *para)'+function(source,'player_extractor_ops_close')+'\n'
    out+='static bool player_extractor_has_output_stream(const esp_player_extractor_t *extractor)'+function(source,'player_extractor_has_output_stream')+'\n'
    out+='static bool player_extractor_should_hold_for_audio(esp_player_extractor_t *extractor,esp_gmf_element_handle_t self)'+function(source,'player_extractor_should_hold_for_audio')+'\n'
    out+='static esp_gmf_job_err_t extractor_send_eos(esp_gmf_port_handle_t out_port)'+function(source,'extractor_send_eos')+'\n'
    if candidate:
        out+='static bool player_extractor_local_mjpeg_av(esp_player_extractor_t *extractor,esp_gmf_element_handle_t self)'+function(source,'player_extractor_local_mjpeg_av')+'\n'
        out+='static bool player_extractor_should_discard_video(esp_player_extractor_t *extractor,esp_gmf_element_handle_t self,const esp_extractor_frame_info_t *frame,esp_extractor_err_t result)'+function(source,'player_extractor_should_discard_video')+'\n'
        out+='static esp_gmf_job_err_t player_extractor_discard_video(esp_player_extractor_t *extractor,esp_extractor_frame_info_t *frame)'+function(source,'player_extractor_discard_video')+'\n'
    out+='static esp_gmf_job_err_t player_extractor_process(esp_gmf_element_handle_t self,void *para)'+function(source,'player_extractor_process')+'\n'
    out+='#define CANDIDATE '+str(int(candidate))+'\n'+(HERE/'process_test.c').read_text('utf-8')
    return out

def main():
    assert BASELINE.is_file() and TCC.is_file()
    source=SOURCE.read_text('utf-8')
    with zipfile.ZipFile(BASELINE) as archive:baseline_bytes=archive.read(BASELINE_ENTRY)
    assert hashlib.sha256(baseline_bytes).hexdigest()==BASELINE_SOURCE_SHA256
    baseline=baseline_bytes.decode('utf-8');runs=[]
    with tempfile.TemporaryDirectory(prefix='hosino_extractor_fairness_') as td:
        work=Path(td)
        for label,data,candidate in [('v20_baseline',baseline,False),('candidate',source,True)]:
            (work/'test.c').write_text(build_source(data,candidate),'utf-8')
            c=subprocess.run([str(TCC),'-Wall','test.c','-o','test.exe'],cwd=work,capture_output=True,text=True)
            print(label,'compile',c.returncode,c.stdout,c.stderr)
            assert c.returncode==0
            result=subprocess.run([str(work/'test.exe')],cwd=work,capture_output=True,text=True)
            print(label,result.returncode,result.stdout,result.stderr)
            runs.append({'label':label,'compile_exit_code':c.returncode,'test_exit_code':result.returncode,'output':result.stdout,'stderr':result.stderr,'production_source_sha256':hashlib.sha256(data.encode()).hexdigest()})
            assert result.returncode==0
    report={'passed':True,'scope':__doc__,'production_source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),'baseline_archive':str(BASELINE.relative_to(ROOT)),'baseline_archive_sha256':hashlib.sha256(BASELINE.read_bytes()).hexdigest(),'runs':runs,'limits':'Actual production C functions and process body; deterministic fake extractor, queues, RTOS and URI. Does not execute binary AVI parser, PSRAM allocator, audio hardware, cross-core scheduling or prove real sound continuity.'}
    (ROOT/'evidence/extractor-fairness-host-v23-20261114.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n','utf-8')

if __name__=='__main__':main()
