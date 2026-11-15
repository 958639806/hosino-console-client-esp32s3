"""Run unmodified media_profile.c on the host with deterministic platform shims.

Uses an already installed compiler; never downloads tools or accesses hardware.
Only .build and validation.json in this test directory are written.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path)
    args = parser.parse_args()
    fallback = ROOT / "tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe"
    compiler = args.compiler or Path(shutil.which("tcc") or fallback)
    if not compiler.is_file():
        raise SystemExit("No installed host compiler; pass --compiler. No download attempted.")
    build = HERE / ".build"
    build.mkdir(exist_ok=True)
    files = {
        "media_profile.c": "overlay-apps/components/hosino_media_player/media_profile.c",
        "media_profile.h": "overlay-apps/components/hosino_media_player/media_profile.h",
        "hosino_media_player.h": "overlay-apps/components/hosino_media_player/include/hosino_media_player.h",
        "hosino_player_diagnostics.h": "overlay-apps/components/esp_player_compat/esp_player/include/hosino_player_diagnostics.h",
        "hosino_file_diagnostics.h": "overlay-apps/components/gmf_io_compat/gmf_io/include/hosino_file_diagnostics.h",
    }
    hashes = {}
    for name, rel in files.items():
        shutil.copyfile(ROOT / rel, build / name)
        hashes[rel] = digest(ROOT / rel)
        assert digest(build / name) == hashes[rel]
    shims = {
        "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n",
        "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
        "esp_video_dec.h": "#pragma once\ntypedef int esp_vc_err_t;\n#define ESP_VC_ERR_OK 0\ntypedef void *esp_video_dec_handle_t;\ntypedef struct { int unused; } esp_video_dec_in_frame_t;\ntypedef struct { int unused; } esp_video_dec_out_frame_t;\n",
        "freertos/FreeRTOS.h": "#pragma once\ntypedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\nvoid host_enter(void);\nvoid host_exit(void);\n#define portENTER_CRITICAL(p) ((void)(p), host_enter())\n#define portEXIT_CRITICAL(p) ((void)(p), host_exit())\n",
        "host_port.h": "#include <stddef.h>\nsize_t host_strlcpy(char *, const char *, size_t);\n#define strlcpy host_strlcpy\n",
    }
    for name, contents in shims.items():
        p = build / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(contents, encoding="ascii")
    command = [str(compiler.resolve()), "-Wall", "-I.build", "-include", ".build/host_port.h",
               "host_profile_test.c", ".build/media_profile.c", "-o", ".build/host_profile_test.exe"]
    compiled = subprocess.run(command, cwd=HERE, capture_output=True, text=True)
    print(compiled.stdout, end="")
    print(compiled.stderr, end="")
    if compiled.returncode:
        raise SystemExit(compiled.returncode)
    result = subprocess.run([str(build / "host_profile_test.exe")], cwd=HERE, capture_output=True, text=True)
    for rel, expected in hashes.items():
        assert digest(ROOT / rel) == expected, f"Source changed during test: {rel}"
    report = {"source_sha256": hashes, "unmodified_source_copies": True,
              "compiler": str(compiler), "compiler_sha256": digest(compiler),
              "compile_exit_code": compiled.returncode, "run_exit_code": result.returncode,
              "stdout": result.stdout, "stderr": result.stderr,
              "scope": "Actual media_profile.c and public headers; deterministic clock, decoder and critical-section boundary shims. No FreeRTOS scheduling, I/O, device or full player validation."}
    (HERE / "validation.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(result.stdout, end="")
    print(result.stderr, end="")
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
