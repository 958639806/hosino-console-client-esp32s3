#!/usr/bin/env python3
"""Real public-source backend HTTP integration smoke test, loopback only.

The bearer is generated in RAM, passed only in HTTP headers and never persisted.
No login, QR issuance, account mutation, TLS proxy or public listener is enabled.
Video/audio are decoded from the actual authenticated bridge response, not a
completed source file. Passing proves host integration, not ESP32 playback.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import http.client
import json
import math
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import threading
import time
from urllib.parse import urlencode

from live_http_playback_probe import Capture, audio_rms, verify_client_audio
from real_stream_probe import dump


class RangeTrace:
    """Test-only metadata observers; production function return values unchanged."""
    def __init__(self):
        self.lock = threading.Lock()
        self.started = time.monotonic()
        self.profile, self.phase = None, "idle"
        self.rows, self.latest, self.omitted = [], {}, 0
        self.originals = None

    def phase_is(self, profile, phase):
        with self.lock:
            self.profile, self.phase = profile, phase

    def new_range(self, start, end, window):
        with self.lock:
            identity = threading.get_ident()
            if len(self.rows) >= 256:
                self.omitted += 1
                self.latest[identity] = None
                return
            row = {"profile": self.profile, "phase": self.phase,
                   "seconds": time.monotonic() - self.started,
                   "kind": {512 * 1024: "video", 128 * 1024: "audio"}.get(window, "unknown"),
                   "start": start, "end": end, "window": window,
                   "validated_206": False, "forwarded_body_bytes": 0}
            self.rows.append(row)
            self.latest[identity] = row

    def validated(self, start, length):
        with self.lock:
            row = self.latest.get(threading.get_ident())
            if row is not None:
                row.update({"validated_206": True, "response_start": start,
                            "response_end": start + length - 1, "response_length": length})

    def forwarded(self, count):
        with self.lock:
            row = self.latest.get(threading.get_ident())
            if row is not None and row["validated_206"]:
                row["forwarded_body_bytes"] += count

    def ended(self):
        with self.lock:
            row = self.latest.pop(threading.get_ident(), None)
            if row is not None:
                row["ended_seconds"] = time.monotonic() - self.started

    def install(self, module):
        original_range, original_valid = module.request_range, module.valid_response
        original_get, original_headers = module.CDNHandler.do_GET, module.CDNHandler.end_headers
        trace = self

        def observed_range(value, window):
            result = original_range(value, window)
            trace.new_range(result[0], result[1], window)
            return result

        def observed_valid(headers, start, end):
            result = original_valid(headers, start, end)
            trace.validated(start, result)
            return result

        class ObservedWriter:
            def __init__(self, writer, handler):
                self.writer, self.handler = writer, handler

            def __getattr__(self, name):
                return getattr(self.writer, name)

            def write(self, value):
                result = self.writer.write(value)
                if getattr(self.handler, "_smoke_body_started", False):
                    trace.forwarded(result if isinstance(result, int) else len(value))
                return result

        def observed_headers(handler):
            result = original_headers(handler)
            handler._smoke_body_started = True
            return result

        def observed_get(handler):
            writer = handler.wfile
            handler._smoke_body_started = False
            handler.wfile = ObservedWriter(writer, handler)
            try:
                return original_get(handler)
            finally:
                trace.ended()
                handler.wfile = writer

        self.originals = (module, original_range, original_valid, original_get, original_headers)
        module.request_range, module.valid_response = observed_range, observed_valid
        module.CDNHandler.do_GET, module.CDNHandler.end_headers = observed_get, observed_headers

    def uninstall(self):
        if self.originals:
            module, original_range, original_valid, original_get, original_headers = self.originals
            module.request_range, module.valid_response = original_range, original_valid
            module.CDNHandler.do_GET, module.CDNHandler.end_headers = original_get, original_headers

    def summary(self, profile):
        with self.lock:
            rows = [dict(row) for row in self.rows if row["profile"] == profile]
            omitted = self.omitted
        evidence = {}
        for kind in ("video", "audio"):
            live = [row for row in rows if row["phase"] == "live" and row["kind"] == kind
                    and row["validated_206"] and row["forwarded_body_bytes"] > 0]
            evidence[kind] = {"live_positive_start_body_transferred": any(row["start"] > 0 for row in live),
                "live_adjacent_window_body_transferred": any(
                    row["start"] > 0 and row["start"] == earlier.get("response_end", -2) + 1
                    for index, row in enumerate(live) for earlier in live[:index]),
                "live_validated_ranges_with_body": len(live)}
        return {"requests": rows, "omitted_requests_global": omitted, "evidence": evidence,
                "scope": "Test-only wrappers preserve request_range/valid_response results; body counter advances only after actual downstream write returns"}


def job_snapshot(job):
    public = job.public()
    result = {key: public.get(key) for key in ("status", "error", "selected_quality", "sample_video_kbps",
        "sample_peak_1s_video_kbps", "target_video_kbps", "position_seconds", "duration")}
    if isinstance(public.get("diagnostics"), dict):
        result["diagnostics"] = public["diagnostics"]
    with job.lock:
        result.update({"source_bytes": job.source_bytes, "output_bytes": job.output_bytes,
                       "child_returncodes": [process.poll() for process in list(job.processes)[:8]]})
    return result


def flv_prefix_metadata(prefix):
    result = {"bytes_captured": len(prefix), "tags": []}
    if len(prefix) < 9:
        return result
    result.update({"signature": bytes(prefix[:3]).decode("ascii", "replace"), "version": prefix[3],
                   "header_flags": prefix[4], "header_declares_audio": bool(prefix[4] & 4),
                   "header_declares_video": bool(prefix[4] & 1),
                   "data_offset": int.from_bytes(prefix[5:9], "big")})
    position = result["data_offset"] + 4
    if not 13 <= position <= 4100:
        result["invalid_data_offset"] = True
        return result
    while position + 11 <= len(prefix) and len(result["tags"]) < 256:
        kind = prefix[position] & 31
        length = int.from_bytes(prefix[position + 1:position + 4], "big")
        timestamp = int.from_bytes(prefix[position + 4:position + 7], "big") | (prefix[position + 7] << 24)
        payload = position + 11
        row = {"type": kind, "payload_bytes": length, "timestamp_ms": timestamp,
               "prefix_offset": position, "complete_in_prefix": payload + length + 4 <= len(prefix)}
        if length and payload < len(prefix):
            first = prefix[payload]
            if kind == 9:
                row.update({"video_codec_id": first & 15, "frame_type": first >> 4})
            elif kind == 8:
                row.update({"audio_format_id": first >> 4, "audio_rate_code": (first >> 2) & 3,
                            "audio_16bit": bool(first & 2), "audio_stereo": bool(first & 1)})
        result["tags"].append(row)
        if not row["complete_in_prefix"]:
            break
        position = payload + length + 4
    result["video_tag_headers"] = sum(row["type"] == 9 for row in result["tags"])
    result["audio_tag_headers"] = sum(row["type"] == 8 for row in result["tags"])
    result["first_video_tag"] = next((row for row in result["tags"] if row["type"] == 9), None)
    result["first_audio_tag"] = next((row for row in result["tags"] if row["type"] == 8), None)
    return result


def api(port, token, method, path, body=None, authenticated=True):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=55)
    headers = {"Accept": "application/json", "Connection": "close"}
    if authenticated:
        headers["Authorization"] = "Bearer " + token
    encoded = None
    if body is not None:
        encoded = json.dumps(body, separators=(",", ":")).encode()
        headers["Content-Type"] = "application/json"
    try:
        connection.request(method, path, encoded, headers)
        response = connection.getresponse()
        data = response.read(128 * 1024 + 1)
        if len(data) > 128 * 1024:
            raise ValueError("api_response_limit")
        value = json.loads(data)
        return response.status, value
    finally:
        connection.close()


def result_summary(status, value):
    if not isinstance(value, dict):
        return {"http_status": status, "passed": False, "error": "non_object_response"}
    result = {"http_status": status, "passed": status == 200 and value.get("ok") is True}
    if not result["passed"]:
        error = value.get("error") or {}
        result["error"] = {"code": str(error.get("code", ""))[:80],
                           "message": str(error.get("message", ""))[:180]}
    return result


def source_position_argument(value):
    try:
        number = float(value)
    except ValueError:
        raise argparse.ArgumentTypeError("position must be a finite nonnegative number") from None
    if not math.isfinite(number) or not 0 <= number <= 86400:
        raise argparse.ArgumentTypeError("position must be between0 and86400 seconds")
    return number


def playback_request(args, profile, cid):
    return {"bvid": args.bvid, "cid": cid, "profile": profile,
            "video_kbps": args.video_kbps, "position_seconds": args.position_seconds}


def requested_control_checks(data, requested, prepared=False):
    expected_target = requested["video_kbps"] or {"standard": 1800, "clear": 2500}[requested["profile"]]
    echoed_position = data.get("position_seconds")
    position_matches = (type(echoed_position) in (int, float) and math.isfinite(echoed_position)
                        and math.isclose(echoed_position, requested["position_seconds"], rel_tol=0, abs_tol=0.000001))
    checks = {"source_position_echoed": position_matches,
              "profile_echoed": data.get("profile") == requested["profile"],
              "video_kbps_echoed": data.get("video_kbps") == requested["video_kbps"],
              "video_target_echoed": data.get("target_video_kbps") == expected_target}
    if prepared:
        measured = data.get("sample_video_kbps")
        quality = data.get("selected_quality")
        checks["sample_average_within_requested_budget"] = (type(measured) in (int, float)
            and math.isfinite(measured) and 0 < measured <= expected_target + 0.000001)
        checks["selected_quality_valid"] = type(quality) is int and 2 <= quality <= 31
    return checks


def normalized_flv_timestamps(prefix):
    """Check source HTTP tag PTS, independently of decoder timestamp shifting."""
    metadata = flv_prefix_metadata(prefix)
    first = {kind: (metadata.get("first_" + kind + "_tag") or {}).get("timestamp_ms")
             for kind in ("video", "audio")}
    checks = {kind + "_first_pts_normalized": type(first[kind]) is int and 0 <= first[kind] <= 150
              for kind in ("video", "audio")}
    checks["initial_av_offset_within_150ms"] = (all(type(first[kind]) is int for kind in first)
                                               and abs(first["video"] - first["audio"]) <= 150)
    for kind, tag_type in (("video", 9), ("audio", 8)):
        timestamps = [tag["timestamp_ms"] for tag in metadata.get("tags", []) if tag["type"] == tag_type]
        checks[kind + "_prefix_pts_monotonic"] = bool(timestamps) and all(a <= b for a, b in zip(timestamps, timestamps[1:]))
    return {"first_tag_pts_ms": first, "checks": checks, "passed": all(checks.values()),
            "scope": "Raw FLV tag timestamps from actual HTTP response prefix before any FFmpeg input normalization"}


def semaphore_idle(semaphore, capacity):
    acquired = 0
    try:
        for _ in range(capacity):
            if not semaphore.acquire(blocking=False):
                break
            acquired += 1
        return acquired == capacity
    finally:
        for _ in range(acquired):
            semaphore.release()


def clean_job(job, manager, seconds=12):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        with job.lock:
            processes = list(job.processes)
        if (job.stopped.is_set() and not job.worker.is_alive()
                and (job.serving is None or not job.serving.is_alive())
                and all(process.poll() is not None for process in processes)
                and semaphore_idle(manager.relay_slots, 4)):
            break
        time.sleep(0.1)
    with job.lock:
        processes = list(job.processes)
    checks = {"job_stopped": job.stopped.is_set(), "prepare_worker_stopped": not job.worker.is_alive(),
              "relay_serving_stopped": job.serving is None or not job.serving.is_alive(),
              "all_child_processes_exited": all(process.poll() is not None for process in processes),
              "all_four_cdn_handler_slots_released": semaphore_idle(manager.relay_slots, 4),
              "sample_directory_removed": not Path(job.directory.name).exists()}
    return {"checks": checks, "passed": all(checks.values()),
            "tracked_processes_at_check": len(processes)}


def run_case(args, app, port, token, profile, cid):
    started = time.monotonic()
    requested = playback_request(args, profile, cid)
    result = {"profile": profile, "seconds": args.seconds, "passed": False,
              "requested_playback": requested,
              "requested_source_window_seconds": {"start": args.position_seconds, "end": args.position_seconds + args.seconds}}
    session_id = None
    job = process = capture = response = connection = pump = None
    stop = threading.Event()
    stats = {"body_bytes": 0, "first_body_seconds": None, "last_body_seconds": None,
             "pump_error_class": None, "intentional_consumer_close": False,
             "first_video_tag_header_body_seconds": None, "first_audio_tag_header_body_seconds": None,
             "largest_gap_between_body_reads_seconds": 0, "body_read_samples": []}
    prefix = bytearray()
    witness = None
    wav_path = args.output_dir / (profile + "-backend-client.wav")
    args.range_trace.phase_is(profile, "preparing")
    try:
        status, value = api(port, token, "POST", "/v1/playbacks",
            requested)
        result["create"] = result_summary(status, value)
        if not result["create"]["passed"]:
            return result
        data = value["data"]
        session_id = data["session_id"]
        result["create"]["control_checks"] = requested_control_checks(data, requested)
        result["create"]["passed"] = bool(result["create"]["passed"] and all(result["create"]["control_checks"].values()))
        if not result["create"]["passed"]:
            return result
        stream_path = data["stream_path"]
        if (not isinstance(stream_path, str) or not stream_path.startswith("/v1/streams/")
                or any(char in stream_path for char in "?#\r\n")):
            raise ValueError("unsafe_backend_stream_path")
        job = app.playbacks.get(session_id)
        polls = 0
        prepare_deadline = time.monotonic() + 65
        while time.monotonic() < prepare_deadline:
            status, value = api(port, token, "GET", "/v1/playbacks?" + urlencode({"session_id": session_id}))
            polls += 1
            if status != 200 or not value.get("ok"):
                result["prepare"] = result_summary(status, value)
                return result
            data = value["data"]
            if data.get("status") == "ready":
                break
            if data.get("status") in ("error", "stopped", "ended"):
                result["prepare"] = {"passed": False, "status": data.get("status"),
                                      "error": data.get("error")}
                return result
            time.sleep(0.25)
        else:
            result["prepare"] = {"passed": False, "error": "readiness_timeout"}
            return result
        result["prepare"] = {"passed": True, "polls": polls, "wall_seconds": time.monotonic() - started,
            **{key: data.get(key) for key in ("profile", "video_kbps", "selected_quality", "target_video_kbps",
                                           "sample_video_kbps", "sample_peak_1s_video_kbps", "rate_control", "position_seconds")}}
        result["prepare"]["control_checks"] = requested_control_checks(data, requested, prepared=True)
        result["prepare"]["passed"] = all(result["prepare"]["control_checks"].values())
        if not result["prepare"]["passed"]:
            return result
        decode_started = time.monotonic()
        command = [args.ffmpeg, "-hide_banner", "-nostdin", "-y", "-v", "warning",
                   "-filter_threads", "2", "-threads", "2", "-probesize", "131072",
                   "-analyzeduration", "500000", "-f", "flv", "-i", "pipe:0",
                   "-progress", "pipe:1", "-stats_period", "0.25", "-nostats",
                   "-t", str(args.seconds), "-map", "0:v:0", "-c:v", "rawvideo", "-threads:v", "2",
                   "-f", "null", os.devnull,
                   "-t", str(args.seconds), "-map", "0:a:0", "-c:a", "pcm_s16le",
                   "-ar", "22050", "-ac", "1", "-flush_packets", "1", "-f", "wav", str(wav_path)]
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        def observe(row):
            nonlocal witness
            if witness is None and row["frames"] > 0 and row["media_seconds"] > 0:
                rms = audio_rms(wav_path)
                if rms is not None and rms > 1:
                    with job.lock:
                        source_bytes = job.source_bytes
                    witness = {"seconds_since_decode_start": time.monotonic() - decode_started,
                               "decoded_frames": row["frames"], "decoded_seconds": row["media_seconds"],
                               "audio_rms": rms, "authenticated_http_body_bytes": stats["body_bytes"],
                               "source_bytes_at_first_decode": source_bytes}

        capture = Capture(process, decode_started, observe)
        args.range_trace.phase_is(profile, "live")
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
        connection.request("GET", stream_path, headers={"Authorization": "Bearer " + token,
                           "Accept-Encoding": "identity", "Connection": "close"})
        response = connection.getresponse()
        result["stream"] = {"http_status": response.status,
                            "transfer_encoding": response.getheader("Transfer-Encoding"),
                            "content_type": response.getheader("Content-Type"),
                            "authorization_transport": "Python HTTP header only; FFmpeg reads stdin"}
        if response.status != 200 or response.getheader("Transfer-Encoding", "").lower() != "chunked":
            raise ValueError("unexpected_authenticated_stream_response")

        def feed_decoder():
            try:
                digest = hashlib.sha256()
                while not stop.is_set() and time.monotonic() - decode_started < 70:
                    block = response.read1(32768)
                    if not block:
                        break
                    now = time.monotonic() - decode_started
                    if stats["first_body_seconds"] is None:
                        stats["first_body_seconds"] = now
                    if stats["last_body_seconds"] is not None:
                        stats["largest_gap_between_body_reads_seconds"] = max(
                            stats["largest_gap_between_body_reads_seconds"], now - stats["last_body_seconds"])
                    stats["last_body_seconds"] = now
                    stats["body_bytes"] += len(block)
                    if len(stats["body_read_samples"]) < 256:
                        stats["body_read_samples"].append({"seconds": now, "bytes": len(block)})
                    if len(prefix) < 128 * 1024:
                        prefix.extend(block[:128 * 1024 - len(prefix)])
                        parsed = flv_prefix_metadata(prefix)
                        if parsed.get("first_video_tag") and stats["first_video_tag_header_body_seconds"] is None:
                            stats["first_video_tag_header_body_seconds"] = now
                        if parsed.get("first_audio_tag") and stats["first_audio_tag_header_body_seconds"] is None:
                            stats["first_audio_tag_header_body_seconds"] = now
                    if stats["body_bytes"] > 32 * 1024 * 1024:
                        raise ValueError("stream_receive_limit")
                    digest.update(block)
                    process.stdin.write(block)
                    process.stdin.flush()
                stats["received_prefix_sha256"] = digest.hexdigest()
            except (BrokenPipeError, ConnectionResetError):
                stats["intentional_consumer_close"] = process.poll() is not None
            except Exception as exc:
                if not stop.is_set():
                    stats["pump_error_class"] = type(exc).__name__
            finally:
                try:
                    process.stdin.close()
                except (BrokenPipeError, OSError):
                    pass

        pump = threading.Thread(target=feed_decoder, daemon=True, name="backend-smoke-http-pump")
        pump.start()
        try:
            code = process.wait(timeout=70)
        except subprocess.TimeoutExpired:
            process.kill(); code = process.wait(timeout=5)
            result["decoder_timed_out"] = True
        result["job_at_decoder_exit_before_client_close"] = job_snapshot(job)
        stop.set()
        if connection.sock:
            try:
                connection.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        pump.join(timeout=5)
        capture.join()
        decoded = capture.summary()
        audio = verify_client_audio(wav_path, args.seconds)
        with job.lock:
            source_bytes = job.source_bytes
        fps = 20 if profile == "standard" else 15
        progress = decoded.get("last_progress") or {}
        timestamps = normalized_flv_timestamps(prefix)
        checks = {"decoder_exit_zero": code == 0, "no_decode_timeout": not result.get("decoder_timed_out", False),
                  "both_video_and_non_silent_pcm_observed": witness is not None,
                  "full_frame_count": abs(progress.get("frames", -10000) - args.seconds * fps) <= 1,
                  "full_media_duration": abs(progress.get("media_seconds", -10000) - args.seconds) <= 0.15,
                  "decoder_progress_ended": progress.get("ended") is True,
                  "no_media_errors_all_diagnostics": decoded["media_error_count_all_lines"] == 0,
                  "audio_output_verified": audio["passed"],
                  "raw_http_av_pts_normalized": timestamps["passed"],
                  "requested_controls_echoed_on_create": all(result["create"]["control_checks"].values()),
                  "requested_controls_ready_and_sample_budget_valid": all(result["prepare"]["control_checks"].values()),
                  "http_body_received": stats["body_bytes"] > 4096,
                  "http_pump_no_error": stats["pump_error_class"] is None,
                  "http_pump_stopped": not pump.is_alive()}
        result.update({"decode_exit_code": code, "decode_wall_seconds": time.monotonic() - decode_started,
                       "witness": witness, "http_transfer": stats, "decoder": decoded,
                       "decoded_audio": audio, "source_bytes_including_parameter_samples": source_bytes,
                       "raw_http_flv_timestamps": timestamps,
                       "checks": checks, "passed": all(checks.values())})
    except Exception as exc:
        result["error_class"] = type(exc).__name__
    finally:
        stop.set()
        if process is not None and process.poll() is None:
            process.kill(); process.wait(timeout=5)
        if connection is not None and connection.sock:
            try:
                connection.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        if pump is not None:
            pump.join(timeout=5)
        if response is not None:
            response.close()
        if connection is not None:
            connection.close()
        if capture is not None:
            capture.join()
        if prefix:
            prefix_path = args.output_dir / (profile + "-public-flv-prefix.bin")
            prefix_path.write_bytes(prefix)
            result["public_flv_prefix"] = {"file": prefix_path.name,
                "sha256": hashlib.sha256(prefix).hexdigest(), **flv_prefix_metadata(prefix),
                "scope": "At most128KiB from actual public-video bridge response; incomplete final tag is not a decode failure"}
        if job is not None:
            # Preserve the failure state before DELETE changes it to stopped.
            result["job_before_delete"] = job_snapshot(job)
        if session_id:
            try:
                status, value = api(port, token, "DELETE", "/v1/playbacks?" + urlencode({"session_id": session_id}))
                result["explicit_delete"] = result_summary(status, value)
            except Exception as exc:
                result["explicit_delete"] = {"passed": False, "error_class": type(exc).__name__}
                app.playbacks.stop()
        if job is not None:
            result["cleanup"] = clean_job(job, app.playbacks)
            result["passed"] = bool(result["passed"] and result["cleanup"]["passed"]
                                    and result.get("explicit_delete", {}).get("passed"))
        result["range_trace"] = args.range_trace.summary(profile)
        proof = result["range_trace"]["evidence"]
        result["finite_range_continuation_proven"] = all(proof[kind]["live_positive_start_body_transferred"]
                                                         for kind in ("video", "audio"))
        result["passed"] = bool(result["passed"] and result["finite_range_continuation_proven"])
        args.range_trace.phase_is(None, "idle")
        result["wall_seconds"] = time.monotonic() - started
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--ffprobe", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--backend-dir", type=Path, default=Path(__file__).resolve().parents[1] / "backend")
    parser.add_argument("--bvid", default="BV13x41117TL")
    parser.add_argument("--seconds", type=int, default=20, choices=range(10, 21))
    parser.add_argument("--position-seconds", type=source_position_argument, default=0)
    parser.add_argument("--video-kbps", type=int, choices=(0, 800, 1200, 1800, 2500, 3500), default=0)
    args = parser.parse_args()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=False, mode=0o700)
    sys.path.insert(0, str(args.backend_dir.resolve()))
    from bridge import Application, BridgeServer, Handler
    import playback
    args.range_trace = RangeTrace()
    args.range_trace.install(playback)
    token = secrets.token_urlsafe(32)
    app = server = serving = None
    report = {"created_at_utc": datetime.now(timezone.utc).isoformat(), "passed": False,
              "scope": "Actual anonymous public backend API and authenticated loopback HTTP stream decode; no ESP32/account/public deployment test",
              "bearer_persisted": False, "account_mutations_invoked": False,
              "account_route_test_is_https_guard_only": True, "trusted_tls_proxy": False,
              "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "requested_playback_controls": {"position_seconds": args.position_seconds, "video_kbps": args.video_kbps},
              "backend_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(args.backend_dir.glob("*.py"))},
              "apis": {}, "cases": []}
    if os.name == "posix":
        os.nice(10)
    try:
        app = Application(token, args.output_dir / "private-state", args.ffmpeg, args.ffprobe)
        server = BridgeServer(("127.0.0.1", 0), Handler, app)
        port = server.server_address[1]
        serving = threading.Thread(target=server.serve_forever, daemon=True, name="backend-smoke-api")
        serving.start()
        checks = {}
        for name, auth_token, path, authenticated, expected in (
            ("health", token, "/v1/health", False, 200),
            ("missing_auth_rejected", token, "/v1/feed", False, 401),
            ("wrong_auth_rejected", secrets.token_urlsafe(32), "/v1/feed", True, 401),
            ("account_https_guard", token, "/v1/me", True, 403)):
            status, value = api(port, auth_token, "GET", path, authenticated=authenticated)
            checks[name] = status == expected
        report["boundary_checks"] = checks
        cid = None
        for name, path in (("feed", "/v1/feed?page=1"),
                           ("search", "/v1/search?" + urlencode({"q": "哔哩哔哩", "page": 1})),
                           ("detail", "/v1/video?" + urlencode({"bvid": args.bvid}))):
            status, value = api(port, token, "GET", path)
            result = result_summary(status, value)
            if result["passed"]:
                data = value.get("data") or {}
                if name == "detail":
                    cid = data.get("cid") or next((p.get("cid") for p in data.get("pages", []) if p.get("cid")), None)
                    result.update({"bvid": data.get("bvid"), "title": data.get("title"), "pages": len(data.get("pages", [])), "cid_nonempty": bool(cid)})
                    result["passed"] = bool(cid and data.get("bvid") == args.bvid)
                else:
                    items = data.get("items") or []
                    result.update({"items": len(items), "page": data.get("page"), "has_more": data.get("has_more")})
                    result["passed"] = 0 < len(items) <= 10 and all(item.get("bvid") for item in items)
            report["apis"][name] = result
            print("BACKEND_PUBLIC_API", name, result["passed"], flush=True)
            dump(args.output_dir / "report.json", report)
        if cid:
            for profile in ("standard", "clear"):
                case = run_case(args, app, port, token, profile, cid)
                report["cases"].append(case)
                print("BACKEND_HTTP_CASE", profile, case["passed"], flush=True)
                dump(args.output_dir / "report.json", report)
        report["passed"] = (all(checks.values()) and len(report["apis"]) == 3
            and all(row["passed"] for row in report["apis"].values()) and len(report["cases"]) == 2
            and all(row["passed"] for row in report["cases"]))
    except Exception as exc:
        report["error_class"] = type(exc).__name__
    finally:
        if app is not None:
            app.playbacks.stop()
        if server is not None:
            deadline = time.monotonic() + 10
            while not semaphore_idle(server.handler_slots, 8) and time.monotonic() < deadline:
                time.sleep(0.1)
            idle = semaphore_idle(server.handler_slots, 8)
            server.shutdown(); server.server_close()
            if serving:
                serving.join(timeout=3)
            report["api_shutdown"] = {"all_handler_slots_released": idle, "listener_thread_stopped": not serving.is_alive()}
            report["passed"] = bool(report["passed"] and idle and not serving.is_alive())
        if app is not None:
            app.client.session.close()
        args.range_trace.uninstall()
        token = None
        dump(args.output_dir / "report.json", report)
    print("BACKEND_SMOKE_DONE", report["passed"], flush=True)
    return 0 if report["passed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
