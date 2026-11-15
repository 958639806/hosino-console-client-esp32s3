"""Compile/run production C protocol helpers using the existing TCC tool.
No device, network, account, credential or firmware changes occur.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser()
p.add_argument("--tcc", required=True)
p.add_argument("--work", required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
source = root / "overlay-apps/components/hosino_console_client"
work = Path(a.work).resolve()
work.mkdir(parents=True, exist_ok=True)
for name in ("console_protocol.c", "console_protocol.h"):
    shutil.copy2(source / name, work / name)
shutil.copy2(Path(__file__).with_name("test_protocol.c"), work / "test_protocol.c")
subprocess.run([a.tcc, "-I" + str(work), str(work / "console_protocol.c"),
                str(work / "test_protocol.c"), "-o", str(work / "protocol_tests.exe")], check=True)
fixture = Path(__file__).with_name("fixtures") / "sessions-200-fictional.json"
shutil.copy2(fixture, work / fixture.name)
subprocess.run([str(work / "protocol_tests.exe"), str(work / fixture.name)], check=True)
