"""Compile actual parser/store code with RTOS/NVS boundary stubs; no serial ACK is mocked."""
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
COMPILER=ROOT/"tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe"
def main():
    source=HERE.parent
    with tempfile.TemporaryDirectory(prefix="bili_provision_tests_") as folder:
        folder=Path(folder)
        shutil.copytree(HERE/"stubs",folder,dirs_exist_ok=True)
        for name in ("provision_parser.c","provision_parser.h","provision_store.c","config_store.c"):
            shutil.copy2(source/name,folder/name)
        shutil.copy2(source/"include/hosino_bili_config_store.h",folder)
        shutil.copy2(ROOT/"overlay-apps/components/gmf_io_compat/gmf_io/include/hosino_network_policy.h",folder)
        shutil.copy2(HERE/"test_provision.c",folder)
        built=subprocess.run([str(COMPILER),"-Wall","-I.","test_provision.c","provision_parser.c","provision_store.c","config_store.c","-o","test.exe"],cwd=folder,capture_output=True,text=True)
        print(built.stdout+built.stderr,end="")
        if built.returncode:return built.returncode
        test=subprocess.run([str(folder/"test.exe")],cwd=folder,capture_output=True,text=True)
        print(test.stdout+test.stderr,end="")
        if test.returncode:return test.returncode
        idf=Path("C:/Users/95863/.codex/builds/hosino-esp32-qspi/esp-idf-v5.5.5/components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c")
        code=idf.read_text("utf-8")
        start=code.index("static ssize_t usb_serial_jtag_read(int fd, void* data, size_t size)")
        brace=code.index("{",start);depth=1;end=brace+1
        while depth:
            if code[end]=="{":depth+=1
            elif code[end]=="}":depth-=1
            end+=1
        (folder/"idf_usb_read.c").write_text(code[start:end],"utf-8")
        shutil.copy2(HERE/"test_usb_vfs_read.c",folder)
        built=subprocess.run([str(COMPILER),"-Wall","-I.","test_usb_vfs_read.c","-o","usb_test.exe"],cwd=folder,capture_output=True,text=True)
        print(built.stdout+built.stderr,end="")
        if built.returncode:return built.returncode
        usb=subprocess.run([str(folder/"usb_test.exe")],cwd=folder,capture_output=True,text=True)
        print(usb.stdout+usb.stderr,end="")
        return usb.returncode
if __name__=="__main__":raise SystemExit(main())
