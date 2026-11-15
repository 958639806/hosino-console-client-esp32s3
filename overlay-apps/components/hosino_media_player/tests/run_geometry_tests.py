"""Compile production metadata guards; excludes native extractor/decoder execution."""
from pathlib import Path
import shutil
import subprocess
import tempfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
def main():
    compiler=ROOT/"tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe"
    header=ROOT/"overlay-apps/components/esp_player_compat/esp_player/include/hosino_mjpeg_metadata.h"
    with tempfile.TemporaryDirectory(prefix="managed_mjpeg_geometry_") as folder:
        folder=Path(folder);shutil.copy2(header,folder);shutil.copy2(HERE/"test_managed_geometry.c",folder)
        build=subprocess.run([str(compiler),"-Wall","-I.","test_managed_geometry.c","-o","geometry.exe"],cwd=folder,capture_output=True,text=True)
        print(build.stdout+build.stderr,end="")
        if build.returncode:return build.returncode
        test=subprocess.run([str(folder/"geometry.exe")],cwd=folder,capture_output=True,text=True)
        print(test.stdout+test.stderr,end="");return test.returncode
if __name__=="__main__":raise SystemExit(main())
