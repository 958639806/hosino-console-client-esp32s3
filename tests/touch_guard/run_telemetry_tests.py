"""Exercise the complete production touch wrapper without changing hardware.

Uses the established deterministic IO/adapter fixture and adds telemetry checks.
Does not overwrite the historical v21 test report or touch staged build inputs.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import tempfile

from run_tests import STUB

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "overlay-apps/main/hosino_touch_override.c"
FIXTURE = Path(__file__).with_name("test_touch_v21.c")
TCC = ROOT / "tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe"


def main():
    with tempfile.TemporaryDirectory(prefix="touch-telemetry-", dir="C:/esp/hosino-qspi") as folder:
        work = Path(folder)
        (work / "stub.h").write_text(STUB, encoding="utf-8")
        for name in ("freertos/FreeRTOS.h", "freertos/task.h", "esp_attr.h",
                     "esp_heap_caps.h", "esp_lcd_io_i2c.h", "esp_lcd_panel_io.h",
                     "esp_lcd_touch.h", "esp_lv_adapter.h", "esp_lv_adapter_input.h",
                     "esp_log.h", "esp_timer.h"):
            header = work / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "stub.h"\n', encoding="utf-8")
        (work / SOURCE.name).write_bytes(SOURCE.read_bytes())
        packet = SOURCE.with_name("hosino_touch_packet.h")
        (work / packet.name).write_bytes(packet.read_bytes())
        (work / "test_touch.c").write_bytes(FIXTURE.read_bytes())
        build = subprocess.run([str(TCC), "-Wall", "-I" + str(work),
                                str(work / "test_touch.c"), "-o", str(work / "test.exe")],
                               capture_output=True, text=True)
        run = None if build.returncode else subprocess.run(
            [str(work / "test.exe")], capture_output=True, text=True, timeout=10)
    report = {
        "scope": "Complete production wrapper/parser with deterministic IO/adapter/time mocks; telemetry only. Not real I2C/controller/IRQ/native LVGL event or board acceptance.",
        "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        "packet_sha256": hashlib.sha256(packet.read_bytes()).hexdigest(),
        "fixture_sha256": hashlib.sha256(FIXTURE.read_bytes()).hexdigest(),
        "compile": {"exit_code": build.returncode, "stdout": build.stdout, "stderr": build.stderr},
        "run": None if run is None else {"exit_code": run.returncode, "stdout": run.stdout, "stderr": run.stderr},
        "passed": build.returncode == 0 and run is not None and run.returncode == 0,
        "telemetry": {"cadence_us": 5000000, "absolute_coordinates_logged": False,
                      "contact_ids_logged": False, "new_allocations_added": False,
                      "protocol_irq_timer_or_filter_changed": False,
                      "valid_zero_vs_invalid_read_distinguished": True},
    }
    target = ROOT / "evidence/touch-telemetry-v23-20261115"
    target.mkdir(exist_ok=True)
    (target / "host.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
