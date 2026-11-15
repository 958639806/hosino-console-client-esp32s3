"""Compile the production priming helper against the actual pinned extractor API.

Native implementation is replaced at the read/refcount boundary; no claim that
this runs its proprietary Xtensa machine code or verifies device playback.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]

def main():
    compiler=ROOT/"tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe"
    source=ROOT/"overlay-apps/components/esp_player_compat/esp_player/include"
    native=Path("C:/esp/hosino-qspi/project-apps/managed_components/espressif__esp_extractor/include")
    with tempfile.TemporaryDirectory(prefix="managed_flv_startup_") as folder:
        folder=Path(folder)
        for name in ("hosino_flv_startup.h","hosino_mjpeg_metadata.h"):
            shutil.copy2(source/name,folder)
        for name in ("esp_extractor.h","esp_extractor_types.h"):
            shutil.copy2(native/name,folder)
        shutil.copy2(HERE/"test_flv_startup.c",folder)
        build=subprocess.run([str(compiler),"-Wall","-I.","test_flv_startup.c","-o","startup.exe"],cwd=folder,capture_output=True,text=True)
        print(build.stdout+build.stderr,end="")
        if build.returncode:return build.returncode
        test=subprocess.run([str(folder/"startup.exe")],cwd=folder,capture_output=True,text=True)
        print(test.stdout+test.stderr,end="");return test.returncode

if __name__=="__main__":raise SystemExit(main())
