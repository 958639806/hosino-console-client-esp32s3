#!/usr/bin/env python3
"""Bounded, synthetic MJPEG/PCM cloud benchmark; Python stdlib only.

This never logs in, downloads videos, opens a listener, installs packages, or
changes services. By default it measures synthetic 720p test-pattern generation,
scaling and encoding. --source-video instead decodes an existing local file with
its audio. Neither mode proves Bilibili access or ESP32 playback. An unpatched
FFmpeg may run the AVI baseline, but is explicitly NOT transport-compatible.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

try:
    import resource
except ImportError:  # Allows the no-process self-check on Windows.
    resource = None


def dump(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def usage():
    if resource is None:
        return None
    value = resource.getrusage(resource.RUSAGE_CHILDREN)
    return value.ru_utime + value.ru_stime, value.ru_maxrss


def lower_priority():
    os.nice(10)


def execute(argv, timeout=60):
    before = usage()
    started = time.monotonic()
    try:
        result = subprocess.run(
            argv, capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=timeout, preexec_fn=lower_priority if os.name == "posix" else None,
        )
        code, stdout, stderr, timed_out = result.returncode, result.stdout, result.stderr, False
    except subprocess.TimeoutExpired as exc:
        def text(value):
            return value.decode("utf-8", "replace") if isinstance(value, bytes) else value or ""
        code, stdout, stderr, timed_out = None, text(exc.stdout), text(exc.stderr), True
    elapsed = time.monotonic() - started
    after = usage()
    return {
        "argv": list(argv), "exit_code": code, "timed_out": timed_out,
        "wall_seconds": elapsed, "stdout": stdout, "stderr": stderr,
        "child_cpu_seconds": after[0] - before[0] if before and after else None,
        "cumulative_child_peak_rss_kib": after[1] if after and sys.platform.startswith("linux") else None,
        "rss_scope": "Linux RUSAGE_CHILDREN cumulative maximum across this script's completed children; not per-run RSS",
    }


def progress_values(text):
    result = {}
    for line in text.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            result[key.strip()] = value.strip()
    return result


def summary(probe, seconds):
    streams = {int(s["index"]): s for s in probe.get("streams", [])}
    result = []
    for index, stream in streams.items():
        packets = [p for p in probe.get("packets", []) if int(p["stream_index"]) == index]
        timestamps = [float(p["pts_time"]) for p in packets if p.get("pts_time") not in (None, "N/A")]
        sizes = [int(p.get("size", 0)) for p in packets]
        ends = [float(p["pts_time"]) + float(p.get("duration_time", 0)) for p in packets
                if p.get("pts_time") not in (None, "N/A") and p.get("duration_time") != "N/A"]
        result.append({
            "index": index, "codec_type": stream.get("codec_type"),
            "codec_name": stream.get("codec_name"), "width": stream.get("width"),
            "height": stream.get("height"), "sample_rate": stream.get("sample_rate"),
            "channels": stream.get("channels"), "packet_count": len(packets),
            "payload_bytes": sum(sizes), "payload_bitrate_kbps": sum(sizes) * 8 / seconds / 1000,
            "first_pts_seconds": timestamps[0] if timestamps else None,
            "last_pts_seconds": timestamps[-1] if timestamps else None,
            "last_packet_end_seconds": max(ends) if ends else None,
            "pts_monotonic": bool(timestamps) and all(a <= b for a, b in zip(timestamps, timestamps[1:])),
        })
    return result


def command(ffmpeg, output, resolution, rate, audio, seconds, container, source_video=None):
    inputs = ["-threads", "2", "-i", str(source_video)] if source_video else [
        "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=24",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=22050",
    ]
    return [
        ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "warning", "-y",
        "-filter_threads", "2", "-filter_complex_threads", "2",
        *inputs,
        "-t", str(seconds), "-map", "0:v:0", "-map", "0:a:0" if source_video else "1:a:0",
        "-vf", f"scale={resolution['width']}:{resolution['height']}:flags=bilinear,fps={resolution['target_fps']}",
        "-c:v", "mjpeg", "-threads:v", "2", "-pix_fmt", "yuvj420p", "-b:v", f"{rate}k",
        "-c:a", "pcm_s16le", "-ar", str(audio["sample_rate_hz"]), "-ac", str(audio["channels"]),
        "-progress", "pipe:1", "-nostats", "-f", container, str(output),
    ]


def probe_file(ffprobe, path, seconds):
    if not ffprobe:
        return {"verified": False, "reason": "ffprobe is unavailable"}
    result = execute([
        ffprobe, "-v", "error", "-show_packets", "-show_streams", "-show_format",
        "-show_entries", "packet=stream_index,size,pts_time,dts_time,duration_time:stream=index,codec_name,codec_type,width,height,sample_rate,channels:format=duration,size,format_name",
        "-of", "json", str(path),
    ])
    if result["exit_code"] != 0:
        return {"verified": False, "exit_code": result["exit_code"], "stderr": result["stderr"][-4000:]}
    try:
        value = json.loads(result["stdout"])
        rows = summary(value, seconds)
    except (ValueError, TypeError, KeyError) as exc:
        return {"verified": False, "reason": str(exc)}
    codecs = {(s["codec_type"], s["codec_name"]) for s in rows}
    return {"verified": codecs == {("video", "mjpeg"), ("audio", "pcm_s16le")} and
                         all(s["pts_monotonic"] for s in rows),
            "format": value.get("format", {}), "streams": rows}


def run_case(args, output, resolution, rate, audio, seconds, container):
    result = execute(command(args.ffmpeg, output, resolution, rate, audio, seconds, container,
                             args.source_video), args.timeout)
    progress = progress_values(result["stdout"])
    size = output.stat().st_size if output.exists() else 0
    metadata = probe_file(args.ffprobe, output, seconds) if result["exit_code"] == 0 and size else {"verified": False}
    video = next((s for s in metadata.get("streams", []) if s["codec_type"] == "video"), {})
    sound = next((s for s in metadata.get("streams", []) if s["codec_type"] == "audio"), {})
    media_checks = {
        "codecs_and_monotonic_pts": metadata["verified"],
        "dimensions_match": (video.get("width"), video.get("height")) == (resolution["width"], resolution["height"]),
        "frame_count_matches": abs(video.get("packet_count", -1000000) - round(seconds * resolution["target_fps"])) <= 1,
        "audio_parameters_match": (sound.get("sample_rate"), sound.get("channels")) == (str(audio["sample_rate_hz"]), audio["channels"]),
    }
    result.update({
        "container": container, "output": output.name, "output_bytes": size,
        "resolution_id": resolution["id"], "width": resolution["width"], "height": resolution["height"],
        "target_fps": resolution["target_fps"], "requested_video_average_kbps": rate,
        "requested_seconds": seconds, "progress": progress, "probe": metadata,
        "total_file_bitrate_kbps": size * 8 / seconds / 1000,
        "actual_video_payload_kbps": video.get("payload_bitrate_kbps"),
        "video_packets": video.get("packet_count"),
        "expected_video_packets": round(seconds * resolution["target_fps"]),
        "encoding_speed_x": seconds / result["wall_seconds"] if result["wall_seconds"] else None,
        "average_cpu_cores_used": result["child_cpu_seconds"] / result["wall_seconds"] if result["child_cpu_seconds"] is not None and result["wall_seconds"] else None,
        "media_checks": media_checks,
        "passed": result["exit_code"] == 0 and size > 0 and all(media_checks.values()),
    })
    return result


def self_check():
    assert progress_values("frame=1\nspeed=2x\nframe=2\n") == {"frame": "2", "speed": "2x"}
    rows = summary({"streams": [{"index": 0, "codec_type": "video", "codec_name": "mjpeg"}],
                    "packets": [{"stream_index": 0, "size": "1000", "pts_time": "0", "duration_time": "0.05"},
                                {"stream_index": 0, "size": "500", "pts_time": "0.05", "duration_time": "0.05"}]}, 0.1)
    assert rows[0]["payload_bitrate_kbps"] == 120 and rows[0]["pts_monotonic"]
    assert rows[0]["last_packet_end_seconds"] == 0.1
    params = ("ffmpeg", "output.flv", {"width": 320, "height": 180, "target_fps": 20},
              1800, {"sample_rate_hz": 22050, "channels": 1}, 20, "flv")
    generated = command(*params)
    source = command(*params, source_video=Path("existing.mp4"))
    assert generated.count("-i") == 2 and "1:a:0" in generated
    assert source.count("-i") == 1 and "0:a:0" in source and "lavfi" not in source
    print("PASS no-process self-check: progress/packet metrics and generated/local source routing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg"))
    parser.add_argument("--ffprobe", default=shutil.which("ffprobe"))
    parser.add_argument("--config", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--source-video", type=Path,
                        help="Existing local audio/video file, starting at its normal beginning; no URL fetching")
    parser.add_argument("--seconds", type=int, default=20)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    if args.self_check:
        self_check()
        return 0
    if not args.ffmpeg or not args.config or not args.output_dir:
        parser.error("--ffmpeg, --config and --output-dir are required for a benchmark")
    if not 1 <= args.seconds <= 30 or not 1 <= args.timeout <= 60:
        parser.error("seconds must be 1..30 and timeout 1..60")
    source_metadata = {"kind": "synthetic_lavfi", "sha256": None}
    if args.source_video:
        if not args.source_video.is_file():
            parser.error("--source-video must be an existing local file")
        args.source_video = args.source_video.resolve()
        digest = hashlib.sha256()
        with args.source_video.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
        source_metadata = {"kind": "local_encoded_audio_video", "file": args.source_video.name,
                           "bytes": args.source_video.stat().st_size, "sha256": digest.hexdigest()}
        if args.ffprobe:
            source_probe = execute([
                args.ffprobe, "-v", "error", "-show_streams", "-show_format",
                "-show_entries", "stream=index,codec_name,codec_type,width,height,r_frame_rate,sample_rate,channels:format=duration,format_name",
                "-of", "json", str(args.source_video),
            ], 30)
            source_metadata["probe_exit_code"] = source_probe["exit_code"]
            try:
                source_metadata["probe"] = json.loads(source_probe["stdout"]) if source_probe["exit_code"] == 0 else None
            except ValueError:
                source_metadata["probe"] = None
    config_bytes = args.config.read_bytes()
    config = json.loads(config_bytes.decode("utf-8"))
    resolutions, rates, audio = config["resolutions"], config["bitrate_selector"]["video_target_kbps"], config["audio_candidate"]
    if len(resolutions) != 2 or len(rates) != 5 or audio["codec"] != "pcm_s16le":
        parser.error("expected the two-resolution/five-rate PCM plan")
    if args.output_dir.exists():
        parser.error("output directory must be fresh; existing results are never overwritten")
    args.output_dir.mkdir(parents=True)
    version = execute([args.ffmpeg, "-version"], 15)
    (args.output_dir / "ffmpeg-version.txt").write_text(version["stdout"] + version["stderr"], "utf-8")
    compatibility = run_case(args, args.output_dir / "compatibility.flv", resolutions[0], rates[0], audio, 1, "flv")
    dump(args.output_dir / "flv-compatibility.json", compatibility)
    container = "flv" if compatibility["passed"] else "avi"
    rows = []
    for resolution in resolutions:
        for rate in rates:
            name = f"{resolution['id']}-{rate}k.{container}"
            row = run_case(args, args.output_dir / name, resolution, rate, audio, args.seconds, container)
            rows.append(row)
            dump(args.output_dir / (name + ".json"), row)
            print(json.dumps({"case": name, "passed": row["passed"], "speed_x": round(row["encoding_speed_x"], 2),
                              "actual_video_kbps": round(row["actual_video_payload_kbps"], 1) if row["actual_video_payload_kbps"] is not None else None,
                              "total_file_kbps": round(row["total_file_bitrate_kbps"], 1)}, ensure_ascii=False), flush=True)
    report = {
        "created_at_utc": datetime.now(timezone.utc).isoformat(),
        "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "config_sha256": hashlib.sha256(config_bytes).hexdigest(),
        "scope": ("Local source decode/scale/MJPEG+PCM encode, container codecs and packet PTS. "
                  if args.source_video else "Synthetic server generation/scale/MJPEG+PCM encode, container codecs and packet PTS; no encoded-source decode. ")
                 + "No Bilibili source fetch, HTTP/WiFi/TLS transfer, ESP32 playback, live rate adaptation, or application UI validation.",
        "input": "Existing local encoded video with audio" if args.source_video else
                 "testsrc2 1280x720 at24fps plus440Hz sine; generated inline, not an encoded Bilibili source",
        "source": source_metadata,
        "transport_compatible": compatibility["passed"], "container_benchmarked": container,
        "avi_fallback_is_baseline_only": container == "avi", "runs": rows,
        "benchmark_completed": len(rows) == 10,
        "passed": compatibility["passed"] and len(rows) == 10 and all(r["passed"] for r in rows),
        "rate_control_note": "-b:v requests an MJPEG average target; measured output may differ substantially. No strict CBR guarantee. File bitrate excludes TCP/TLS overhead.",
        "hardware_validated": False, "server_service_deployed": False,
    }
    dump(args.output_dir / "results.json", report)
    print(json.dumps({"report": str(args.output_dir / "results.json"), "transport_compatible": report["transport_compatible"], "all_encodes_verified": all(r["passed"] for r in rows), "passed": report["passed"]}), flush=True)
    return 0 if report["passed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
