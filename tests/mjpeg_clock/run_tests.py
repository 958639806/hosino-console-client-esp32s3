"""Compile the production C clock helper with deterministic timestamps; no device I/O."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
HELPER = ROOT / "overlay-apps/components/esp_player_compat/esp_player/src/sync/private_inc/hosino_mjpeg_clock.h"
COMPILER = ROOT / "tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe"

def main():
    if not COMPILER.is_file():
        raise SystemExit("Existing portable TCC compiler is missing; no automatic install.")
    with tempfile.TemporaryDirectory(prefix="hosino_mjpeg_clock_") as work:
        work = Path(work)
        (work / HELPER.name).write_bytes(HELPER.read_bytes())
        (work / "clock_test.c").write_bytes((HERE / "clock_test.c").read_bytes())
        built = subprocess.run([str(COMPILER), "-Wall", "clock_test.c", "-o", "clock_test.exe"],
                               cwd=work, capture_output=True, text=True)
        print(built.stdout, built.stderr, end="")
        if built.returncode:
            raise SystemExit(built.returncode)
        tested = subprocess.run([str(work / "clock_test.exe")], cwd=work, capture_output=True, text=True)
        print(tested.stdout, tested.stderr, end="")
        if tested.returncode:
            raise SystemExit(tested.returncode)
        vendor = HELPER.parents[3]
        for name, source in {
            "player_sync.c": vendor / "src/sync/player_sync.c",
            "player_sync.h": HELPER.parent / "player_sync.h",
            "hosino_player_diagnostics.h": vendor / "include/hosino_player_diagnostics.h",
            "sync_stubs.h": HERE / "sync_stubs.h",
            "sync_integration_test.c": HERE / "sync_integration_test.c",
        }.items():
            (work / name).write_bytes(source.read_bytes())
        for name in ["freertos/FreeRTOS.h", "freertos/task.h", "esp_log.h", "esp_timer.h",
                     "player_events.h", "player_stream.h", "esp_player_types.h"]:
            target = work / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('#include "sync_stubs.h"\n')
        sync_build = subprocess.run([str(COMPILER), "-Wall", "-I.", "sync_integration_test.c",
                                     "player_sync.c", "-o", "sync_test.exe"],
                                    cwd=work, capture_output=True, text=True)
        print(sync_build.stdout, sync_build.stderr, end="")
        if sync_build.returncode:
            raise SystemExit(sync_build.returncode)
        sync_test = subprocess.run([str(work / "sync_test.exe")], cwd=work, capture_output=True, text=True)
        print(sync_test.stdout, sync_test.stderr, end="")
        record = {"passed": sync_test.returncode == 0, "compiler": str(COMPILER),
                  "production_helper": str(HELPER.relative_to(ROOT)),
                  "production_helper_sha256": hashlib.sha256(HELPER.read_bytes()).hexdigest(),
                  "test_source_sha256": hashlib.sha256((HERE / "clock_test.c").read_bytes()).hexdigest(),
                  "compile_exit_code": built.returncode, "test_exit_code": tested.returncode,
                  "sync_source_sha256": hashlib.sha256((vendor / "src/sync/player_sync.c").read_bytes()).hexdigest(),
                  "sync_test_exit_code": sync_test.returncode,
                  "sync_test_output": sync_test.stdout + sync_test.stderr,
                  "output": tested.stdout + tested.stderr,
                  "scope": "Production C helper/guards and full production player_sync.c using deterministic host clock/RTOS stubs; excludes cross-core scheduling, actual audio hardware and device playback."}
        (HERE / "validation.json").write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        raise SystemExit(sync_test.returncode)

if __name__ == "__main__":
    main()
