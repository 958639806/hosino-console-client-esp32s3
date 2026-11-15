#!/usr/bin/env python3
"""Decode a growing real-source FLV over loopback HTTP during transcoding.

Extends the finite upstream relay probe with a separate live HTTP decoder. Only
ephemeral loopback listeners are opened. This proves host HTTP delivery/decoding,
not ESP32 Wi-Fi delivery, display performance, human listening or account access.
"""
from __future__ import annotations

import argparse
import array
from datetime import datetime, timezone
import hashlib
from http.server import BaseHTTPRequestHandler
import math
import os
from pathlib import Path
import re
import secrets
import struct
import subprocess
import sys
import threading
import time
import wave

from live_relay_probe import RelayHandler, RelayServer, RelayState
from real_stream_probe import dump, probe_json, resolve_sources, verify_output


MEDIA_ERROR = re.compile(r"\b(?:error|invalid|corrupt(?:ed)?|prematurely|failed)\b", re.I)


def audio_rms(path):
    """Read a bounded decoded-audio prefix, even while its WAV is growing."""
    try:
        with path.open("rb") as source:
            prefix = source.read(3 * 22050 * 2 + 65536)
        if prefix[:4] != b"RIFF" or prefix[8:12] != b"WAVE":
            return None
        # FFmpeg's seekable WAV writes zero placeholder RIFF/data sizes until
        # close; parse bounded chunks without trusting the unfinished RIFF size.
        position, pcm_format, raw = 12, False, b""
        while position + 8 <= len(prefix):
            tag, size = struct.unpack_from("<4sI", prefix, position)
            position += 8
            if tag == b"fmt " and size >= 16 and position + 16 <= len(prefix):
                code, channels, rate, _, alignment, bits = struct.unpack_from("<HHIIHH", prefix, position)
                pcm_format = (code, channels, rate, alignment, bits) == (1, 1, 22050, 2, 16)
            if tag == b"data":
                raw = prefix[position:position + 3 * 22050 * 2] if pcm_format else b""
                break
            if size > len(prefix) - position:
                return None
            position += size + size % 2
        samples = array.array("h")
        samples.frombytes(raw[:len(raw) // 2 * 2])
        if sys.byteorder != "little":
            samples.byteswap()
        if len(samples) < 1024:
            return None
        return math.sqrt(sum(float(sample) * sample for sample in samples) / len(samples))
    except (OSError, EOFError, struct.error):
        return None


class TailHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_GET(self):
        server = self.server
        if self.path != "/" + server.token + "/live.flv":
            self.send_error(404)
            return
        if self.headers.get("Range", "bytes=0-") not in ("bytes=0-", "bytes=0-0"):
            self.send_error(416)
            return
        with server.stats_lock:
            server.stats["requests"] += 1
            accepted = server.stats["requests"] == 1
        if not accepted:
            self.send_error(429)
            return
        self.connection.settimeout(12)
        self.send_response(200)
        self.send_header("Content-Type", "video/x-flv")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        source = None
        clean_eof = False
        try:
            while not server.stop.is_set() and time.monotonic() < server.deadline:
                if source is None:
                    try:
                        source = server.output.open("rb")
                    except FileNotFoundError:
                        if server.finished.is_set():
                            clean_eof = True
                            break
                        time.sleep(0.025)
                        continue
                block = source.read(64 * 1024)
                if block:
                    if source.tell() > 64 * 1024 * 1024:
                        raise ValueError("output_transfer_limit")
                    self.wfile.write(("%x\r\n" % len(block)).encode("ascii") + block + b"\r\n")
                    self.wfile.flush()
                    with server.stats_lock:
                        server.stats["bytes"] += len(block)
                        if server.stats["first_body_seconds"] is None:
                            server.stats["first_body_seconds"] = time.monotonic() - server.started
                        server.stats["last_body_seconds"] = time.monotonic() - server.started
                elif server.finished.is_set():
                    clean_eof = True
                    break
                else:
                    time.sleep(0.025)
            if clean_eof:
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
                with server.stats_lock:
                    server.stats["terminal_chunk_sent"] = True
        except (BrokenPipeError, ConnectionResetError, TimeoutError, OSError):
            with server.stats_lock:
                server.stats["downstream_closed_early"] = not server.finished.is_set()
        except Exception as exc:
            with server.stats_lock:
                server.stats["error_class"] = type(exc).__name__
        finally:
            if source:
                source.close()
            self.close_connection = True


class Capture:
    """Bounded FFmpeg diagnostics and progress with real-time observations."""
    def __init__(self, process, started, callback=None):
        self.process, self.started, self.callback = process, started, callback
        self.stderr = bytearray()
        self.stderr_bytes_seen = 0
        self.media_error_count = 0
        self.media_error_lines = []
        self.progress = []
        self.threads = [threading.Thread(target=self.read_progress, daemon=True),
                        threading.Thread(target=self.read_errors, daemon=True)]
        for thread in self.threads:
            thread.start()

    def read_errors(self):
        for line in self.process.stderr:
            self.stderr_bytes_seen += len(line)
            if MEDIA_ERROR.search(line.decode("utf-8", "replace")):
                self.media_error_count += 1
                if len(self.media_error_lines) < 32:
                    self.media_error_lines.append(line.decode("utf-8", "replace")[:512].strip())
            if len(self.stderr) < 65536:
                self.stderr.extend(line[:65536 - len(self.stderr)])

    def read_progress(self):
        values = {}
        for line in self.process.stdout:
            text = line.decode("ascii", "replace").strip()
            if "=" not in text:
                continue
            key, value = text.split("=", 1)
            values[key] = value
            if key != "progress":
                continue
            try:
                row = {"wall_seconds": time.monotonic() - self.started,
                       "frames": int(values.get("frame", "0")),
                       "media_seconds": int(values.get("out_time_us", "0")) / 1_000_000,
                       "ended": value == "end"}
                if len(self.progress) < 512:
                    self.progress.append(row)
                if self.callback:
                    self.callback(row)
            except (ValueError, OSError):
                pass
            values = {}

    def join(self):
        for thread in self.threads:
            thread.join(timeout=3)

    def summary(self):
        advancing = []
        for row in self.progress:
            if row["frames"] > 0 and row["media_seconds"] > 0 and (not advancing or
                    row["media_seconds"] > advancing[-1]["media_seconds"]):
                advancing.append(row)
        gaps = [b["wall_seconds"] - a["wall_seconds"] for a, b in zip(advancing, advancing[1:])]
        return {"updates": len(self.progress), "advancing_updates": len(advancing),
                "first_advancing": advancing[0] if advancing else None,
                "last_progress": self.progress[-1] if self.progress else None,
                "largest_wall_gap_between_advancing_updates_seconds": max(gaps, default=None),
                "progress_samples": self.progress,
                "media_error_count_all_lines": self.media_error_count,
                "media_error_lines": self.media_error_lines,
                "stderr_bytes_seen": self.stderr_bytes_seen,
                "stderr_capture_truncated": self.stderr_bytes_seen > len(self.stderr),
                "stderr": self.stderr.decode("utf-8", "replace")[-4000:]}


def packet_gaps(ffprobe, output, fps):
    probe = probe_json(ffprobe, output, packets=True)
    rows = []
    for stream in probe.get("streams", []):
        packets = [packet for packet in probe.get("packets", []) if packet["stream_index"] == stream["index"]]
        deltas, gaps = [], []
        for previous, current in zip(packets, packets[1:]):
            if previous.get("pts_time") in (None, "N/A") or current.get("pts_time") in (None, "N/A"):
                continue
            delta = float(current["pts_time"]) - float(previous["pts_time"])
            if stream.get("codec_type") == "audio":
                duration = int(previous["size"]) / (22050 * 2)
            else:
                duration = 1 / fps
            deltas.append(delta)
            gaps.append(delta - duration)
        rows.append({"codec_type": stream.get("codec_type"), "packet_count": len(packets),
                     "largest_pts_step_seconds": max(deltas, default=None),
                     "largest_gap_seconds": max(gaps, default=None),
                     "largest_overlap_seconds": -min(gaps, default=0),
                     "strictly_increasing_pts": bool(deltas) and all(delta > 0 for delta in deltas),
                     "no_gaps_over_3ms": bool(gaps) and max(gaps) <= 0.003,
                     "no_overlaps_over_3ms": bool(gaps) and min(gaps) >= -0.003})
    return {"streams": rows, "passed": len(rows) == 2 and all(row["strictly_increasing_pts"] and
            row["no_gaps_over_3ms"] and row["no_overlaps_over_3ms"] for row in rows),
            "scope": "FLV media timestamps; wall-clock playback cadence is reported separately"}


def launch(command):
    return subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def verify_client_audio(path, seconds):
    """Independently check the actual HTTP decoder's completed WAV output."""
    try:
        with wave.open(str(path), "rb") as source:
            parameters = {"channels": source.getnchannels(), "sample_rate_hz": source.getframerate(),
                          "sample_bytes": source.getsampwidth(), "header_frames": source.getnframes(),
                          "compression": source.getcomptype()}
            pcm = source.readframes(22050 * seconds + 4096)
        actual_frames = len(pcm) // max(1, parameters["channels"] * parameters["sample_bytes"])
        duration = actual_frames / max(1, parameters["sample_rate_hz"])
        rms = audio_rms(path)
        checks = {"pcm_mono_22050_s16": (parameters["channels"], parameters["sample_rate_hz"],
                  parameters["sample_bytes"], parameters["compression"]) == (1, 22050, 2, "NONE"),
                  "header_matches_decoded_bytes": actual_frames == parameters["header_frames"],
                  "decoded_duration": abs(duration - seconds) <= 0.15,
                  "non_silent": rms is not None and rms > 1}
        return {"parameters": parameters, "actual_frames": actual_frames,
                "decoded_duration_seconds": duration, "first_three_seconds_rms": rms,
                "pcm_sha256": hashlib.sha256(pcm).hexdigest(), "checks": checks,
                "passed": all(checks.values())}
    except Exception as exc:
        return {"passed": False, "error_class": type(exc).__name__}


def run_case(args, sources, profile):
    started = time.monotonic()
    deadline = started + 90
    output = args.output_dir / (profile["id"] + "-http-live.flv")
    decoded_audio = args.output_dir / (profile["id"] + "-http-decoded.wav")
    upstream = RelayState(sources, "https://www.bilibili.com/video/" + args.bvid + "/", deadline)
    source_server = RelayServer(("127.0.0.1", 0), RelayHandler)
    source_server.state = upstream
    tail = RelayServer(("127.0.0.1", 0), TailHandler)
    tail.token, tail.output, tail.deadline, tail.started = secrets.token_hex(16), output, deadline, started
    tail.stop, tail.finished, tail.stats_lock = threading.Event(), threading.Event(), threading.Lock()
    tail.stats = {"requests": 0, "bytes": 0, "first_body_seconds": None,
                  "last_body_seconds": None, "downstream_closed_early": False,
                  "terminal_chunk_sent": False, "framing": "HTTP/1.1 chunked with terminal zero chunk"}
    serving = [threading.Thread(target=server.serve_forever, daemon=True) for server in (source_server, tail)]
    for thread in serving:
        thread.start()
    base = "http://127.0.0.1:%d/%s/" % (source_server.server_address[1], upstream.token)
    downstream_url = "http://127.0.0.1:%d/%s/live.flv" % (tail.server_address[1], tail.token)
    encoder_command = [args.ffmpeg, "-hide_banner", "-nostdin", "-y", "-v", "warning",
        "-filter_threads", "2", "-filter_complex_threads", "2",
        "-threads", "2", "-re", "-rw_timeout", "15000000", "-i", base + "video",
        "-threads", "2", "-re", "-rw_timeout", "15000000", "-i", base + "audio",
        "-t", str(args.seconds), "-map", "0:v:0", "-map", "1:a:0",
        "-vf", "scale=%d:%d:flags=bilinear,fps=%d" % (profile["width"], profile["height"], profile["target_fps"]),
        "-c:v", "mjpeg", "-threads:v", "2", "-q:v", str(args.quality), "-pix_fmt", "yuvj420p",
        "-c:a", "pcm_s16le", "-ar", "22050", "-ac", "1", "-progress", "pipe:1",
        "-stats_period", "0.25", "-nostats", "-flush_packets", "1", "-f", "flv", str(output)]
    decoder_command = [args.ffmpeg, "-hide_banner", "-nostdin", "-y", "-v", "warning",
        "-filter_threads", "2", "-threads", "2", "-rw_timeout", "15000000", "-seekable", "0",
        "-multiple_requests", "1",
        "-probesize", "131072", "-analyzeduration", "500000", "-f", "flv", "-i", downstream_url,
        "-progress", "pipe:1", "-stats_period", "0.25", "-nostats",
        "-map", "0:v:0", "-c:v", "rawvideo", "-threads:v", "2", "-f", "null", os.devnull,
        "-map", "0:a:0", "-c:a", "pcm_s16le", "-ar", "22050", "-ac", "1",
        "-flush_packets", "1", "-f", "wav", str(decoded_audio)]
    encoder = decoder = encoder_capture = decoder_capture = None
    witness = None
    encoder_code = decoder_code = None
    timed_out = False
    encoder_exit_seconds = None

    def decoded_progress(row):
        nonlocal witness
        if witness is not None or row["frames"] <= 0 or row["media_seconds"] <= 0:
            return
        rms = audio_rms(decoded_audio)
        if rms is not None and rms > 1 and encoder is not None and encoder.poll() is None:
            upstream.output_witness(output.stat().st_size)
            witness = {**row, "audio_rms": rms, "decoded_audio_file_bytes": decoded_audio.stat().st_size,
                       "encoder_running": True, "source": "HTTP decoder video progress plus non-silent decoded WAV"}

    try:
        decoder = launch(decoder_command)
        decoder_capture = Capture(decoder, started, decoded_progress)
        encoder = launch(encoder_command)
        encoder_capture = Capture(encoder, started)
        try:
            encoder_code = encoder.wait(timeout=max(1, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            timed_out = True
            encoder.kill()
            encoder_code = encoder.wait(timeout=5)
        encoder_exit_seconds = time.monotonic() - started
        tail.finished.set()
        try:
            decoder_code = decoder.wait(timeout=max(1, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            timed_out = True
            decoder.kill()
            decoder_code = decoder.wait(timeout=5)
        encoder_capture.join()
        decoder_capture.join()
    finally:
        tail.finished.set()
        tail.stop.set()
        upstream.stop.set()
        for process in (encoder, decoder):
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=5)
        for server in (source_server, tail):
            server.shutdown()
            server.server_close()
        for thread in serving:
            thread.join(timeout=2)
        cleanup_deadline = time.monotonic() + 10
        while upstream.active and time.monotonic() < cleanup_deadline:
            time.sleep(0.1)
    relay = upstream.snapshot()
    result = {"resolution": profile, "file": output.name, "decoded_audio_file": decoded_audio.name,
              "encoder_exit_code": encoder_code, "decoder_exit_code": decoder_code,
              "timed_out": timed_out, "wall_seconds": time.monotonic() - started,
              "encoder_exit_seconds": encoder_exit_seconds, "first_live_http_decode": witness,
              "encoder": encoder_capture.summary() if encoder_capture else None,
              "decoder": decoder_capture.summary() if decoder_capture else None,
              "upstream": relay, "downstream_http": dict(tail.stats),
              "first_output_semantics": "First live HTTP decoded video frame AND non-silent decoded audio while encoder is running",
              "jpeg_quality": args.quality, "rate_control_scope": "Fixed JPEG quality; no bitrate-control claim"}
    if output.exists() and encoder_code == 0 and decoder_code == 0:
        result["verification"] = verify_output(args.ffmpeg, args.ffprobe, output, profile,
                                               {"sample_rate_hz": 22050, "channels": 1}, args.seconds)
        result["packet_cadence"] = packet_gaps(args.ffprobe, output, profile["target_fps"])
        result["file_bytes"] = output.stat().st_size
        result["sha256"] = hashlib.sha256(output.read_bytes()).hexdigest()
        result["total_file_kbps"] = output.stat().st_size * 8 / args.seconds / 1000
        result["client_audio_verification"] = verify_client_audio(decoded_audio, args.seconds)
    final_progress = (result["decoder"] or {}).get("last_progress") or {}
    checks = {"encoder_and_http_decoder_exit_zero": encoder_code == decoder_code == 0,
              "within_deadline": not timed_out,
              "decoded_both_tracks_before_encoder_exit": witness is not None,
              "video_upstream_continued_after_live_decode": relay["tracks"]["video"]["bytes_after_output"] > 0,
              "audio_upstream_continued_after_live_decode": relay["tracks"]["audio"]["bytes_after_output"] > 0,
              "all_relay_requests_valid": all(not request.get("error") and request.get("range_validated")
                                             for request in relay["requests"]),
              "relay_shutdown_complete": relay["active_after_shutdown"] == 0,
              "http_output_received": tail.stats["bytes"] > 4096,
              "http_chunked_clean_eof": tail.stats["terminal_chunk_sent"],
              "http_output_not_closed_early": not tail.stats["downstream_closed_early"] and not tail.stats.get("error_class"),
              "decoder_no_media_errors": decoder_capture is not None and decoder_capture.media_error_count == 0,
              "encoder_no_media_errors": encoder_capture is not None and encoder_capture.media_error_count == 0,
              "client_decoded_frame_count": abs(final_progress.get("frames", -10000) - args.seconds * profile["target_fps"]) <= 1,
              "client_decoded_duration": abs(final_progress.get("media_seconds", -10000) - args.seconds) <= 0.15,
              "client_decode_ended": final_progress.get("ended", False),
              "client_audio_pcm_and_duration_verified": result.get("client_audio_verification", {}).get("passed", False),
              "complete_output_verified": result.get("verification", {}).get("passed", False),
              "output_pts_cadence": result.get("packet_cadence", {}).get("passed", False)}
    result["checks"] = checks
    result["passed"] = all(checks.values())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--ffprobe", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--bvid", default="BV13x41117TL")
    parser.add_argument("--seconds", type=int, choices=range(10, 21), default=20)
    parser.add_argument("--quality", type=int, choices=range(2, 32), default=8)
    args = parser.parse_args()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    if os.name == "posix":
        os.nice(10)
    report = {"schema": 1, "started_utc": datetime.now(timezone.utc).isoformat(),
              "scope": "Real public DASH transfer -> concurrent MJPEG/PCM FLV encoding -> concurrent loopback HTTP client audio/video decode",
              "source_files_created": False, "esp32_playback_tested": False,
              "human_listening_tested": False, "public_listener_opened": False, "cases": []}
    try:
        from curl_cffi import requests
        with requests.Session(impersonate="chrome", trust_env=False) as client:
            info, sources = resolve_sources(client, args.bvid)
        report["source_info"] = info
        if not isinstance(info["source_duration_seconds"], (int, float)) or info["source_duration_seconds"] < args.seconds:
            raise ValueError("source_too_short")
        for profile in ({"id": "standard", "width": 320, "height": 180, "target_fps": 20},
                        {"id": "clear", "width": 480, "height": 270, "target_fps": 15}):
            case = run_case(args, sources, profile)
            report["cases"].append(case)
            dump(args.output_dir / "report.json", report)
            print("LIVE_HTTP_CASE", profile["id"], "PASS" if case["passed"] else "FAIL", flush=True)
        report["passed"] = len(report["cases"]) == 2 and all(case["passed"] for case in report["cases"])
    except Exception as exc:
        report["passed"] = False
        report["error_class"] = type(exc).__name__
    dump(args.output_dir / "report.json", report)
    print("LIVE_HTTP_PROBE", "PASS" if report["passed"] else "FAIL", flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
