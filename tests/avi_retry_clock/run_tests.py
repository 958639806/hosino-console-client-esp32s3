"""Run the production bounded AVI header/rational retry helper on a host.

The deterministic fake vendor in extractor_fairness additionally executes the
real process body. Neither test executes the Xtensa-only binary AVI parser.
"""
from pathlib import Path
import hashlib,json,subprocess,tempfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
HEADER=ROOT/'overlay-apps/components/esp_player_compat/esp_player/src/extractor/private_inc/hosino_avi_retry_clock.h'
TCC=ROOT/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'

def main():
    with tempfile.TemporaryDirectory(prefix='hosino_avi_clock_') as work:
        exe=Path(work)/'test.exe'
        built=subprocess.run([str(TCC),'-Wall','-I'+str(HEADER.parent),str(HERE/'clock_test.c'),'-o',str(exe)],capture_output=True,text=True)
        print(built.stdout,built.stderr);assert built.returncode==0
        run=subprocess.run([str(exe)],cwd=work,capture_output=True,text=True)
        print(run.stdout,run.stderr);assert run.returncode==0
    report={'passed':True,'scope':__doc__,'helper_sha256':hashlib.sha256(HEADER.read_bytes()).hexdigest(),
            'compile_exit_code':built.returncode,'test_exit_code':run.returncode,'output':run.stdout,
            'limits':'Pure production helper; host stdio and deterministic metadata; no hardware, real card IO, or binary AVI parser execution.'}
    (ROOT/'evidence/avi-retry-clock-host-20261114.json').write_text(json.dumps(report,indent=2)+'\n','utf-8')

if __name__=='__main__':main()
