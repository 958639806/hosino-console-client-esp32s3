"""Package already-built images; this script never opens a serial port."""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

APPS_PROJECT_VERSION = "qspi-apps-23"


def validate_apps_version(build):
    description = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
    if description.get("project_version") != APPS_PROJECT_VERSION:
        raise SystemExit(f"Apps packaging requires a {APPS_PROJECT_VERSION} build; refusing to relabel an older image")
    # A successful configure can update project_description before compilation
    # finishes. Check the actual IDF image's fixed-offset esp_app_desc as well,
    # before creating or overwriting any packaged firmware files.
    with (build / "esp-brookesia.bin").open("rb") as image:
        header = image.read(80)
    if (len(header) != 80 or header[0] != 0xE9
            or struct.unpack_from("<I", header, 32)[0] != 0xABCD5432):
        raise SystemExit("Apps image is missing the expected ESP-IDF app descriptor")
    try:
        embedded_version = header[48:80].split(b"\0", 1)[0].decode("ascii")
    except UnicodeDecodeError:
        raise SystemExit("Apps image has an invalid embedded version") from None
    if embedded_version != APPS_PROJECT_VERSION:
        raise SystemExit(f"Apps image contains {embedded_version!r}, expected {APPS_PROJECT_VERSION}; build may be incomplete")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_tool(args, log):
    result = subprocess.run([sys.executable, "-m", "esptool", *args],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            encoding="utf-8", errors="replace")
    log.write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        raise SystemExit(result.stdout)
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-root", default=r"C:\esp\hosino-qspi")
    parser.add_argument("--performance", action="store_true")
    parser.add_argument("--buffered", action="store_true")
    parser.add_argument("--corners", action="store_true")
    parser.add_argument("--localized", action="store_true")
    parser.add_argument("--mhz", nargs="+", type=int, choices=(40, 80), default=[40, 80])
    parser.add_argument("--apps", action="store_true")
    args = parser.parse_args()
    args.localized = args.localized or args.apps
    if args.localized and args.corners:
        parser.error("--localized and --corners are mutually exclusive")
    buffered = args.buffered or args.corners or args.localized
    performance = args.performance or buffered
    root = Path(args.build_root).resolve()
    workspace = Path(__file__).resolve().parents[1]
    source_lock = json.loads((workspace / "source-lock.json").read_text(encoding="utf-8"))
    family = "apps" if args.apps else "localized" if args.localized else "corners" if args.corners else "buffered" if buffered else "performance" if performance else "comparison"
    def config_path(mhz):
        return root / (f"sdkconfig.apps.{mhz}" if args.apps
                       else f"sdkconfig.localized.{mhz}" if args.localized
                       else f"sdkconfig.corners.{mhz}" if args.corners
                       else f"sdkconfig.buffered.{mhz}" if buffered
                       else f"sdkconfig.perf.{mhz}" if performance else f"sdkconfig.{mhz}")
    manifests = []
    for mhz in sorted(set(args.mhz)):
        build = root / (f"build-apps-{mhz}" if args.apps
                        else f"build-localized-{mhz}" if args.localized
                        else f"build-corners-{mhz}" if args.corners
                        else f"build-buffered-{mhz}" if buffered
                        else f"build-perf-{mhz}" if performance else f"build-{mhz}")
        config = config_path(mhz).read_text(encoding="utf-8")
        other = 80 if mhz == 40 else 40
        if (f"CONFIG_APP_LCD_QSPI_{mhz}MHZ=y" not in config.splitlines()
                or f"CONFIG_APP_LCD_QSPI_{other}MHZ=y" in config.splitlines()):
            raise SystemExit(f"Wrong clock selection in sdkconfig.{mhz}")
        if performance and ("CONFIG_HOSINO_PERF_DEMO=y" not in config.splitlines()
                                 or "CONFIG_LV_USE_PERF_MONITOR=y" in config.splitlines()):
            raise SystemExit("Performance build must enable the custom HUD and disable LVGL sysmon FPS")
        if buffered != ("CONFIG_HOSINO_DISPLAY_BUFFERED=y" in config.splitlines()):
            raise SystemExit("Buffered configuration does not match the requested firmware family")
        if args.corners != ("CONFIG_HOSINO_DISPLAY_CORNERS=y" in config.splitlines()):
            raise SystemExit("Corners configuration does not match the requested firmware family")
        if args.localized != ("CONFIG_HOSINO_UI_ZH=y" in config.splitlines()):
            raise SystemExit("Localized configuration does not match the requested firmware family")
        if args.apps:
            validate_apps_version(build)
            console_options = (
                "CONFIG_LV_USE_IME_PINYIN=y", "CONFIG_LV_IME_PINYIN_USE_DEFAULT_DICT=y",
                "CONFIG_LV_IME_PINYIN_USE_K9_MODE=y", "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y",
                "CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y", "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y",
                "CONFIG_MBEDTLS_SSL_PROTO_TLS1_2=y",
            )
            if any(option not in config.splitlines() for option in console_options):
                raise SystemExit("Apps console requires Pinyin/K9, verified HTTPS and external TLS allocation")
            if "CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y" in config.splitlines():
                raise SystemExit("Apps console must not force TLS allocations into scarce internal SRAM")
            description = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
            if not {"HosinoConsole", "hosino_console_client", "hosino_chat_fonts", "BiliBili", "hosino_bili_provision"}.issubset(description["build_component_info"]):
                raise SystemExit("Apps build is missing a required console component")
            if "CONFIG_ESP_PLAYER_ENABLE_HTTP_IO=y" not in config.splitlines():
                raise SystemExit("Apps v23 requires the managed network-player HTTP path")
            chat_font = json.loads((root / "project-apps/components/hosino_chat_fonts/font-manifest.json").read_text(encoding="utf-8"))
            cache_lines = (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines()
            sample_options = [line.split("=", 1)[1] for line in cache_lines
                              if line.startswith("HOSINO_INSTALL_MEDIA_SAMPLES:BOOL=")]
            if len(sample_options) != 1 or sample_options[0] not in ("ON", "OFF"):
                raise SystemExit("Apps build must explicitly select HOSINO_INSTALL_MEDIA_SAMPLES ON/OFF")
            samples_embedded = sample_options[0] == "ON"
        flash = json.loads((build / "flasher_args.json").read_text(encoding="utf-8"))
        out = workspace / f"firmware/{family}/{mhz}mhz"
        (out / "split").mkdir(parents=True, exist_ok=True)
        ordered = sorted(flash["flash_files"].items(), key=lambda pair: int(pair[0], 0))
        files = []
        name = f"brookesia-qspi-{mhz}mhz" + ("-apps" if args.apps else "-localized" if args.localized else "-corners" if args.corners
                                               else "-buffered" if buffered else "-fps" if performance else "") + ".bin"
        merge = ["--chip", "esp32s3", "merge_bin", "-o", str(out / name)]
        for key, value in flash["flash_settings"].items():
            merge.extend(["--" + key, str(value)])
        for offset, relative in ordered:
            source = (build / relative).resolve()
            if not source.is_relative_to(build.resolve()):
                raise SystemExit(f"Unexpected flash input outside build: {source}")
            destination = out / "split" / source.name
            shutil.copy2(source, destination)
            merge.extend([offset, str(destination)])
            files.append({"offset": offset, "file": destination.relative_to(out).as_posix(),
                          "size": destination.stat().st_size, "sha256": sha256(destination)})
        run_tool(merge, out / "merge.log")
        combined = out / name
        # Independently verify that merging preserves every source binary at its offset.
        merged_data = combined.read_bytes()
        for item in files:
            offset = int(item["offset"], 0)
            data = (out / item["file"]).read_bytes()
            if merged_data[offset:offset + len(data)] != data:
                raise SystemExit(f"Merged image differs from split input: {item['file']}")
        app = out / "split/esp-brookesia.bin"
        run_tool(["--chip", "esp32s3", "image_info", str(app)], out / "image-info.log")
        shutil.copy2(config_path(mhz), out / "sdkconfig")
        shutil.copy2(build / "dependencies.lock", out / "dependencies.lock")
        shutil.copy2(build / "flasher_args.json", out / "flasher_args.json")
        manifest = {
            "board": source_lock["board"],
            "kind": "brookesia_localized_apps" if args.apps else "brookesia_localized_ui" if args.localized
                    else "brookesia_corner_calibration_experiment" if args.corners
                    else "brookesia_buffered_experiment" if buffered
                    else "brookesia_fps_benchmark" if performance else "brookesia_example_qspi_experiment",
            "display_qspi_requested_hz": mhz * 1000000,
            "not_panel_refresh_hz": True, "not_flash_clock": True,
            "upstream_commit": source_lock["upstream_commit"],
            "idf_version": source_lock["idf_version"], "idf_commit": source_lock["idf_commit"],
            "combined": {"file": combined.name, "flash_offset": "0x0",
                         "size": combined.stat().st_size, "sha256": sha256(combined)},
            "split_images": files, "flash_settings": flash["flash_settings"],
            "dependency_lock_sha256": sha256(out / "dependencies.lock"),
            "sdkconfig_sha256": sha256(out / "sdkconfig"),
            "app_elf_sha256": sha256(build / "esp-brookesia.elf"),
            "app_map_sha256": sha256(build / "esp-brookesia.map"),
            "validation": {"compiled": True, "merge_offsets_verified": True,
                           "hardware_flashed": False, "display_stability_tested": False,
                           "fps_measured": False},
        }
        if performance:
            manifest["features"] = {
                "hud": "UI FPS: last flush-ready notifications per elapsed second; not optical scan Hz",
                "apps": ["Render Test", "Scroll Test"],
                "project_version": APPS_PROJECT_VERSION if args.apps else "qspi-ui-2" if args.localized else "qspi-corner-1" if args.corners else "qspi-buf-4" if buffered else "qspi-perf-1",
            }
        if buffered:
            height = int(next(line.split("=", 1)[1] for line in config.splitlines()
                              if line.startswith("CONFIG_HOSINO_DISPLAY_BUFFER_HEIGHT=")))
            manifest["features"]["apps"].append("LVGL Bench")
            manifest["features"].update({
                "draw_buffer_height": height,
                "draw_buffer_count": 2,
                "draw_buffer_memory": "PSRAM",
                "psram_direct_dma": False,
                "dma_staging": "internal SRAM; LCD IO chunks at most 32768 bytes on ESP32-S3 SPI2",
                "hardware_wiring_changed": False,
                "te_sync_enabled": False,
                "render_mode": "PARTIAL",
                "draw_buffer_total_bytes": 480 * height * 2 * 2,
                "lvgl_task_stack_bytes_minimum": 16384,
                "startup_brightness_serialized_with_lvgl": True,
                "benchmark_source": "LVGL v9.4.0 benchmark; 15 basic scenes adapted to an app-owned root",
                "benchmark_metrics": "completed UI FPS; no synthetic overall score or whole-chip CPU percentage",
            })
        if args.corners:
            manifest["features"]["apps"].append("Corner Cal")
            manifest["features"].update({
                "canvas_resolution": [480, 480],
                "corner_clip_default": "OFF on every boot",
                "corner_clip_persisted_to_nvs": False,
                "physical_corner_radius": "unknown; requires on-device CornerCal calibration",
                "corner_clip_mask": "conservative 2-pixel alignment",
                "max_lcd_submissions_per_flush": 1,
                "ui_inset_applied": False,
                "corner_calibration_required": True,
            })
        if args.localized:
            manifest["features"].update({
                "apps": ["娓叉煋娴嬭瘯", "婊氬姩娴嬭瘯", "鐣岄潰绀轰緥", "鎬ц兘娴嬭瘯"],
                "ui_language": "zh-CN",
                "status_bar_horizontal_inset_px": 40,
                "canvas_resolution": [480, 480],
                "display_crop_enabled": False,
                "corner_clip_enabled": False,
                "recents_memory": {
                    "safe_width_px": 400, "top_padding_px": 40,
                    "alignment": "center",
                    "shows": "free internal and external 8-bit data memory",
                    "refresh_interval_ms": 1000,
                    "refresh_when": "recents visible; initial sample after app install",
                },
            })
        if args.apps:
            if "CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL=y" in config.splitlines():
                raise SystemExit("Refusing an app build that autoformats SD cards")
            manifest["features"].update({
                "apps": ["渲染测试", "滚动测试", "界面示例", "性能测试", "计算器", "2048", "时钟",
                         "画板", "相册", "设置", "录音", "重力小球", "音乐", "视频播放", "声音频谱", "星野控制台", "哔哩哔哩"],
                "expected_app_count": 17,
                "bilibili": {
                    "status": "candidate_not_device_validated",
                    "separate_app": True,
                    "bridge_origin": "user-configured HTTPS; device Bearer header; no redirects",
                    "account": "Normal phone-confirmed QR login; account cookies stay on private server storage",
                    "resolutions": [[320, 180, 20], [480, 270, 15]],
                    "video_target_kbps": [0, 800, 1200, 1800, 2500, 3500],
                    "audio": "PCM16 mono 22050Hz; AUDIO master",
                    "network_ring_bytes": 262144,
                    "preserves": ["desktop", "status bar", "full-screen edge gestures", "local media player"],
                    "hardware_playback_verified": False,
                    "configuration": "Physical USB JSON provisioning; device token stored in separate NVS namespace, never embedded in firmware; NVS executes on internal stack",
                },
                "console": {
                    "views": ["Chat", "Management"],
                    "endpoint": "https://hosino.ume.wiki",
                    "transport": "TLS 1.2 HTTPS with certificate bundle and hostname validation; no redirects",
                    "tls_dynamic_memory": "PSRAM",
                    "authentication": "Device pairing via owner-approved short code and separately persisted Bearer token; ACK only after durable NVS save; explicit password fallback can remember verified hs_sid; passwords never persisted",
                    "chat": "Explicit existing-session AI send/abort, default deliver:false; confirmed QQ delivery/direct text; request-ID acceptance; no automatic POST retry",
                    "management": ["overview", "models", "usage", "memory search"],
                    "input": "Full-key K26 Pinyin with LVGL single-character dictionary; English, numeric and symbol modes",
                    "password_input": "Dedicated six-column paged large keyboard; ASCII letters, numbers, punctuation and space; explicit show/hide, masked by default",
                    "response_limit_bytes": 262144,
                    "json_token_limit": 16384,
                    "chat_font_glyph_count": chat_font["glyph_count"],
                    "chat_font_size_px": 16,
                    "chat_font_bpp": 2,
                    "chat_font_sha256": chat_font["font_c_sha256"],
                    "chat_font_storage": "Static const flash-mapped; RODATA copying into PSRAM disabled; code still fetched from PSRAM",
                    "background_network_buffers": "Released by worker after HTTP cleanup; recreated on next foreground request",
                    "voice_conversation_included": False,
                    "voice_input": "Explicit ES7210 MIC1 capture,16kHz mono PCM16 WAV,max15s; HTTPS /api/asr/transcribe; result appends editable draft,never auto-sends",
                    "voice_server_requirement": "MiMo pay-as-you-go key remains server-side; deployed ASR endpoint required",
                    "hardware_shortcuts": "POWER GPIO16 active-high, BOOT GPIO0 and USER GPIO18 active-low; short release only; user mapping in NVS; app foreground only",
                    "supplemental_font": "7749 monochrome emoji/kaomoji/symbol glyphs; no full combined-emoji shaping",
                    "chat_message_area_px": [400, 300],
                    "session_selection": "Background reads do not lock controls; keyed selection barrier; explicit command ACK; content-based redraw",
                    "device_acceptance": "pending; build candidate is not a flashed or accepted console",
                },
                "media_storage": "FAT32 and exFAT microSD, MBR; no autoformat",
                "video_volume": "0-100, codec update on existing video worker",
                "console_send_recovery": "Explicit local target-confirmed acknowledgement; never resends or clears draft; preserve last send HTTP result",
                "device_pair_server_requirement": "Reviewed device pairing server patch must be deployed; firmware alone cannot enable pairing",
                "video_audio_sync": "AUDIO-master PCM timestamp gaps remain renderable; only successful sink output advances audio reference; seek/pause generation gates retained",
                "media_task_priorities": {"extractor_with_audio": 7, "audio_decode": 9, "audio_render": 10, "video_decode": 5, "video_render": 5, "mixer": 20},
                "touch_guard": "15-byte dual-touch reports, hardware IDs and ACK; 8ms LVGL input period; stable primary pointer with all-release handoff barrier; app-local pinch; actual report rate and gestures require device validation",
                "mjpeg_audio_admission": "Local MJPEG A/V can release excess independent compressed video frames before decoding to keep demux audio progressing; bounded 256KiB compressed A/V pool; no automatic audio discard",
                "avi_mjpeg_retry_timestamps": "Pinned AVI extractor retry-counter compensation using the selected local MJPEG stream's exact dwScale/dwRate; no integer-FPS reconstruction; device A/V synchronization requires validation",
                "video_pinch_routing": "Blank chrome backgrounds pass input to video content; buttons and sliders keep exclusive hit targets; physical 40px edge start exclusion retained",
                "video_presentation": "Original memcpy, aspect-fit/fill opaque RGB565 resampling into two PSRAM buffers; 2880-byte internal row scratch, canvas 1:1; no source-resolution improvement",
                "sdmmc_requested_hz": 40000000,
                "sdmmc_bus_width": 1,
                "file_stdio_cache_bytes": 16384,
                "file_stdio_cache_memory": "PSRAM; unchanged fixed internal SD DMA bounce buffer",
                "sdmmc_fixed_dma_bytes": 4096,
                "lvgl_allocator": "CLIB core allocation wrappers: PSRAM first, internal fallback; global allocator unchanged",
                "fatfs_exfat": True,
                "fatfs_lba64": False,
                "settings": "Wi-Fi disconnect/forget; BLE control and scan; PMU telemetry; internal and card storage; device details",
                "audio": "2.16 pins, ES8311 output + ES7210 stereo STD input; exclusive app sessions",
                "media_framework": "Espressif ESP Player 1.1.1; audio/video render 1.1.0; Brookesia app UI retained",
                "media_acceptance": "FLAC audio and video performance require independent device validation",
                "media_samples_embedded": samples_embedded,
                "media_samples_install_at_boot": samples_embedded,
                "status": "native Wi-Fi, read-only PMU battery telemetry, SNTP system clock",
                "original_xiaozhi_ai_included": False,
                "app_partition_bytes": 8388608,
                "fixed_lcd_dma_staging_bytes": 48000,
                "fps_hud": "Single line at status bar center, x=190..289 y=14, noninteractive; detailed memory metrics on serial",
                "fixed_staging_reuse": "drain old DMA, copy, submit once through original IO; original completion callback retained",
                "internal_dma_reserve_bytes": int(next(line.split("=", 1)[1] for line in config.splitlines()
                    if line.startswith("CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL="))),
                "malloc_internal_preference_threshold_bytes": int(next(line.split("=", 1)[1] for line in config.splitlines()
                    if line.startswith("CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL="))),
                "recommended_flashing": "validated split images via Flash-Firmware.ps1 -Apps; preserves NVS/PHY gaps",
            })
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        manifests.append(manifest)
        print(f"{mhz} MHz: {combined} ({manifest['combined']['sha256']})")
    if len(manifests) == 2:
        if manifests[0]["dependency_lock_sha256"] != manifests[1]["dependency_lock_sha256"]:
            raise SystemExit("A/B dependency locks differ")
        configurations = []
        for mhz in (40, 80):
            configurations.append([line for line in config_path(mhz).read_text(
                encoding="utf-8").splitlines() if "CONFIG_APP_LCD_QSPI_" not in line])
        if configurations[0] != configurations[1]:
            raise SystemExit("A/B configurations differ beyond the display clock choice")
        if manifests[0]["flash_settings"] != manifests[1]["flash_settings"]:
            raise SystemExit("A/B Flash settings differ")
        matching = [[item["sha256"] for item in manifest["split_images"]
                     if item["file"] == "split/partition-table.bin"] for manifest in manifests]
        if len(matching[0]) != 1 or matching[0] != matching[1]:
            raise SystemExit("A/B partition table differs")
    available = [json.loads(p.read_text(encoding="utf-8")) for mhz in (40, 80)
                 if (p := workspace / f"firmware/{family}/{mhz}mhz/manifest.json").exists()]
    (workspace / f"firmware/{family}/manifest.json").write_text(
        json.dumps(available, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
