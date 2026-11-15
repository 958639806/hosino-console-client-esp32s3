"""Compile the project's FatFs against a sparse RAM block device on Windows.

Only this test directory is written. No physical disk/volume APIs are called.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent
TCC_URL = "https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27-win64-bin.zip"
TCC_SHA256 = "34a721949a2583fdff725312da092fa0f5f1f284b702e6f811c6954714faabb2"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def get_tcc(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit).resolve(strict=True)
    installed = shutil.which("tcc")
    if installed:
        return Path(installed)
    archive = ROOT / ".tools" / "tcc-0.9.27-win64-bin.zip"
    target = archive.parent / "tcc-0.9.27"
    archive.parent.mkdir(exist_ok=True)
    if not archive.exists():
        print(f"Downloading portable compiler (478 KiB): {TCC_URL}", flush=True)
        urllib.request.urlretrieve(TCC_URL, archive)
    if sha256(archive) != TCC_SHA256:
        raise RuntimeError("TinyCC archive SHA-256 mismatch")
    with zipfile.ZipFile(archive) as bundle:
        for name in bundle.namelist():
            destination = (target / name).resolve()
            if not destination.is_relative_to(target.resolve()):
                raise RuntimeError("Archive path escapes compiler directory")
        bundle.extractall(target)
    return target / "tcc" / "tcc.exe"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fatfs", type=Path, default=(
        ROOT.parents[1] / "overlay-apps" / "components" / "fatfs"))
    parser.add_argument("--sdkconfig", type=Path, default=Path(
        "C:/esp/hosino-qspi/build-apps-80/config/sdkconfig.h"))
    parser.add_argument("--compiler")
    args = parser.parse_args()
    source = args.fatfs.resolve(strict=True) / "src"
    build = ROOT / ".build"
    build.mkdir(exist_ok=True)
    fingerprints = {}
    for name in ("ff.c", "ff.h", "ffconf.h", "ffunicode.c", "diskio.h"):
        src = source / name
        fingerprints[name] = sha256(src)
        shutil.copyfile(src, build / name)
    config = (build / "ffconf.h").read_text(encoding="utf-8")
    if not re.search(r"#define\s+FF_LBA64\s+0\b", config):
        raise RuntimeError("Test expects the project's 32-bit LBA configuration")
    config, replacements = re.subn(r"(#define\s+FF_FS_EXFAT\s+)[01]\b", r"\g<1>1", config)
    if replacements != 1:
        raise RuntimeError("Unexpected FF_FS_EXFAT definition")
    (build / "ffconf.h").write_text(config, encoding="utf-8")
    shutil.copyfile(args.sdkconfig.resolve(strict=True), build / "sdkconfig.h")
    # Keep the real configuration and API types. Replace only OS includes;
    # mutex hooks below are real Windows critical sections, not FreeRTOS.
    (build / "freertos").mkdir(exist_ok=True)
    (build / "sys").mkdir(exist_ok=True)
    (build / "freertos" / "FreeRTOS.h").write_text(
        "#pragma once\n#define portTICK_PERIOD_MS 1\n", encoding="ascii")
    (build / "freertos" / "semphr.h").write_text("#pragma once\n", encoding="ascii")
    (build / "sys" / "param.h").write_text(
        "#pragma once\n#define MIN(a,b) ((a)<(b)?(a):(b))\n"
        "#define MAX(a,b) ((a)>(b)?(a):(b))\n", encoding="ascii")
    compiler = get_tcc(args.compiler)
    command = [str(compiler), "-Wall", "-I.build", "dual_format_test.c",
               ".build/ff.c", ".build/ffunicode.c", "-o", ".build/dual_format_test.exe"]
    print("Compiling identical FatFs sources with host-only OS/disk hooks...", flush=True)
    subprocess.run(command, cwd=ROOT, check=True)
    result = subprocess.run([str(build / "dual_format_test.exe")], cwd=ROOT,
                            capture_output=True, text=True, check=False)
    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="")
    report = {
        "fatfs_source": str(source), "sdkconfig": str(args.sdkconfig.resolve()),
        "source_sha256": fingerprints, "test_ffconf_sha256": sha256(build / "ffconf.h"),
        "compiler": str(compiler), "compiler_archive_sha256": TCC_SHA256,
        "exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr,
        "scope": "FatFs core, sparse RAM media, Windows mutex hooks; excludes ESP VFS/SDMMC/hardware",
    }
    (build / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    if result.returncode:
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
