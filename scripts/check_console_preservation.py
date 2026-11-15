"""Compare existing firmware sources against the frozen 11a build, read-only.

This checks source preservation, not device behavior. New console/font sources
and generated menu fonts are intentional additions; hardware settings are also
checked separately by the firmware packager.
"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
STAGE = Path("C:/esp/hosino-qspi/project-apps")
ARCHIVE = ROOT / "evidence/apps-v11a-observed/staged-project-and-build-tools.zip"
BASELINE = ROOT / "evidence/apps-v11a-observed/apps-v11a-source-check.json"
OUTPUT = ROOT / "evidence/console-preserved-sources-v22.json"
EXPECTED_ARCHIVE = "73c6e4518a31131fc3672ce8a2cf514bda6d39301bb08b04559e777bed6a0289"
INTENTIONAL = {
    "components/VideoPlayer/VideoPlayer.cpp": "v22 content-only pinch works through blank chrome background; interactive controls and edge gestures retained",
    "components/VideoPlayer/VideoPlayer.hpp": "v21 content-area pinch lifecycle retained with v22 routing correction",
    "components/hosino_media_player/hosino_media_player.c": "v21 bounded 256KiB local A/V compressed packet pool; retains v20 audio priorities, clock fixes and volume",
    "components/esp_player_compat/esp_player/src/extractor/player_extractor.c": "v22 AVI MJPEG retry timestamp correction; retains v21 local per-packet admission for audio progress",
    "components/esp_player_compat/esp_player/src/sync/player_sync.c": "v19 AUDIO-master accepts timestamp gaps before sink, successful output remains the clock anchor",
    "components/esp_player_compat/esp_player/include/hosino_player_diagnostics.h": "v21 bounded compressed-packet admission/drop and size diagnostics; retains PCM counters",
    "components/hosino_media_player/media_profile.c": "v21 aggregate compressed-packet size and release/drop outcomes without per-frame logging",
    "components/hosino_media_player/media_profile.h": "v21 bounded aggregate admission diagnostics",
    "components/hosino_media_player/include/hosino_media_player.h": "v16 exposes player volume setter",
    "main/hosino_perf_overlay.c": "User requested top-center FPS and PSRAM diagnostics (v13)",
    "components/storage_service/storage_service.c": "v15 benchmark uses explicit 16KiB PSRAM stdio buffering and separate CRC timing",
    "components/esp_player_compat/esp_player/src/utils/player_url.c": "v15 file input uses 16KiB PSRAM stdio cache through existing fixed SD DMA bounce buffer",
    "components/gmf_io_compat/gmf_io/esp_gmf_io_file.c": "v15 configure file cache before seek; close/free safely on partial-open failure",
    "components/bsp_extra/include/bsp_board_extra.h": "v15 add exclusive console microphone audio owner without changing codec/pins",
    "components/bsp_extra/src/bsp_board_extra.c": "v15 name exclusive console microphone audio owner without changing codec/pins",
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    archive_sha = sha(ARCHIVE.read_bytes())
    if archive_sha != EXPECTED_ARCHIVE:
        raise SystemExit("Frozen 11a archive does not match recorded hash")
    baseline = json.loads(BASELINE.read_text(encoding="utf-8"))
    critical = []
    for row in baseline["preserved_v6_board_paths"]:
        actual = sha((ROOT / row["path"]).read_bytes())
        critical.append({**row, "current_sha256": actual, "match": actual == row["sha256"]})
    code_count = 0
    unexpected = []
    allowed_font_changes = []
    intentional_changes = []
    with zipfile.ZipFile(ARCHIVE) as archive:
        for name in archive.namelist():
            if not name.startswith("project-apps/") or name.endswith("/"):
                continue
            relative = name.removeprefix("project-apps/")
            if Path(relative).suffix.lower() not in {".c", ".cpp", ".h", ".hpp", ".s"}:
                continue
            target = STAGE / relative
            changed = not target.is_file() or sha(archive.read(name)) != sha(target.read_bytes())
            if relative.startswith("components/hosino_ui_fonts/"):
                if changed:
                    allowed_font_changes.append(relative)
                continue
            if relative in INTENTIONAL:
                if changed:
                    intentional_changes.append({"path": relative,
                        "reason": INTENTIONAL[relative],
                        "sha256": sha(target.read_bytes())})
                continue
            code_count += 1
            if changed:
                unexpected.append(relative)
    result = {
        "checked_at_utc": datetime.now(timezone.utc).isoformat(),
        "scope": "Existing source byte comparison only; not a hardware or runtime regression test",
        "baseline_project_version": "qspi-apps-11a",
        "baseline_archive_sha256": archive_sha,
        "candidate_stage": str(STAGE),
        "existing_code_files_compared_excluding_menu_font": code_count,
        "unexpected_existing_code_changes": unexpected,
        "intentional_regenerated_menu_fonts": allowed_font_changes,
        "intentional_existing_code_changes": intentional_changes,
        "critical_board_sources": critical,
        "passed": not unexpected and all(row["match"] for row in critical),
    }
    OUTPUT.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": result["passed"], "existing_code_files": code_count,
                      "critical_board_files": len(critical), "unexpected": unexpected}))
    if not result["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
