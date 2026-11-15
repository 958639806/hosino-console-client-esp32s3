#!/usr/bin/env python3
"""Bounded real HTTP DASH pull -> live FFmpeg -> local MJPEG/PCM FLV probe.

Run only in an isolated user directory. No accounts, public listener or service
changes. Signed source URLs and anonymous cookies remain in RAM. The loopback
relay uses synchronous libcurl callbacks, not curl_cffi's unbounded stream queue.
This verifies concurrent upstream transfer/transcoding and server decoding, not
ESP32 delivery or human playback.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import threading
import time
from urllib.parse import urljoin

from real_stream_probe import dump, media_url, resolve_sources, verify_output


RANGE = re.compile(r"bytes=(\d+)-(\d*)\Z")
CONTENT_RANGE = re.compile(r"bytes (\d+)-(\d+)/(\d+)\Z")


def request_range(value, window):
    """Keep every upstream request finite; FFmpeg may reconnect for next range."""
    match = RANGE.fullmatch(value or "bytes=0-")
    if not match:
        raise ValueError("unsupported_range")
    start = int(match[1])
    end = int(match[2]) if match[2] else start + window - 1
    if start > 2**40 or end < start:
        raise ValueError("invalid_range")
    return start, min(end, start + window - 1)


def valid_response(headers, start, end):
    match = CONTENT_RANGE.fullmatch(headers.get("content-range", ""))
    if not match:
        raise ValueError("missing_content_range")
    actual_start, actual_end, total = map(int, match.groups())
    if actual_start != start or actual_end < start or actual_end > end or total <= actual_end:
        raise ValueError("invalid_content_range")
    length = actual_end - actual_start + 1
    if "content-length" in headers and int(headers["content-length"]) != length:
        raise ValueError("content_length_mismatch")
    if headers.get("content-encoding", "identity").lower() not in ("identity", ""):
        raise ValueError("unexpected_content_encoding")
    return length


class RelayState:
    def __init__(self, urls, referer, deadline):
        self.urls, self.referer, self.deadline = urls, referer, deadline
        self.token = secrets.token_hex(16)
        self.lock = threading.Lock()
        self.slots = threading.BoundedSemaphore(4)
        self.stop = threading.Event()
        self.requests = []
        self.active = 0
        self.total_bytes = 0
        self.first_output = None
        self.bytes_after_output = 0
        self.tracks = {kind: {"bytes": 0, "first_byte_seconds": None,
                              "last_byte_seconds": None, "bytes_after_output": 0}
                       for kind in ("video", "audio")}
        self.started = time.monotonic()

    def output_witness(self, output_size):
        with self.lock:
            if self.first_output is None:
                self.first_output = {"seconds": time.monotonic() - self.started,
                                     "active_upstream_transfers": self.active,
                                     "upstream_bytes_at_output": self.total_bytes,
                                     "flv_bytes_at_output": output_size}

    def received(self, kind, count):
        with self.lock:
            if self.total_bytes + count > 32 * 1024 * 1024:
                raise ValueError("aggregate_source_limit")
            now = time.monotonic() - self.started
            track = self.tracks[kind]
            track["bytes"] += count
            track["first_byte_seconds"] = track["first_byte_seconds"] or now
            track["last_byte_seconds"] = now
            self.total_bytes += count
            if self.first_output is not None:
                self.bytes_after_output += count
                track["bytes_after_output"] += count

    def snapshot(self):
        with self.lock:
            witness = self.first_output
            return {"source_bytes": self.total_bytes, "tracks": self.tracks.copy(),
                    "requests": [dict(row) for row in self.requests], "active_after_shutdown": self.active,
                    "first_output": witness,
                    "upstream_bytes_after_first_output": self.bytes_after_output,
                    "concurrent_stream_witness": bool(witness and
                        witness["active_upstream_transfers"] > 0 and
                        witness["flv_bytes_at_output"] > 0 and self.bytes_after_output > 0),
                    "relay_queue": "No body queue or source file; blocking libcurl write callback",
                    "listener": "ephemeral IPv4 loopback only",
                    "libcurl_receive_limits_bytes_per_second": {"video": 96 * 1024, "audio": 24 * 1024},
                    "finite_upstream_range_bytes": {"video": 4 * 1024 * 1024, "audio": 1024 * 1024}}


class RelayServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False


class RelayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass  # Paths include the ephemeral nonce; upstream URLs must never log.

    def do_GET(self):
        state = self.server.state
        if state.stop.is_set() or time.monotonic() >= state.deadline:
            self.send_error(503)
            return
        prefix = "/" + state.token + "/"
        kind = self.path[len(prefix):] if self.path.startswith(prefix) else ""
        if kind not in state.urls or "?" in self.path:
            self.send_error(404)
            return
        if not state.slots.acquire(blocking=False):
            self.send_error(503)
            return
        self.connection.settimeout(12)
        try:
            window = (4 if kind == "video" else 1) * 1024 * 1024
            start, end = request_range(self.headers.get("Range"), window)
            self.relay(kind, start, end)
        except Exception:
            # Exception representations from libcurl can contain signed URLs.
            if not getattr(self, "upstream_headers_sent", False):
                try:
                    self.send_error(502, "bounded upstream transfer failed")
                except OSError:
                    pass
        finally:
            self.close_connection = True
            state.slots.release()

    def relay(self, kind, start, end):
        from curl_cffi import Curl, CurlOpt
        from curl_cffi.curl import CURL_WRITEFUNC_ERROR
        from curl_cffi.requests.impersonate import DEFAULT_CHROME

        state = self.server.state
        self.upstream_headers_sent = False
        row = {"kind": kind, "range_start": start, "range_end": end,
               "bytes": 0, "statuses": [], "completed": False,
               "cancelled_by_downstream": False, "error": None}
        with state.lock:
            state.requests.append(row)
            state.active += 1
        try:
            # Bilibili API cookies are intentionally not forwarded to CDN hosts.
            url = state.urls[kind][0]
            for _ in range(4):
                media_url(url)
                headers, status, header_size = {}, 0, 0
                content_length = None
                redirect = None
                aborted = None
                handle = Curl()

                def receive_header(data):
                    nonlocal status, header_size, content_length, redirect, aborted, headers
                    try:
                        header_size += len(data)
                        if len(data) > 16384 or header_size > 65536:
                            raise ValueError("header_limit")
                        line = data.decode("iso-8859-1").strip()
                        if line.startswith("HTTP/"):
                            status = int(line.split()[1])
                            headers = {}
                        elif not line and status >= 200:
                            row["statuses"].append(status)
                            if status in (301, 302, 303, 307, 308):
                                redirect = media_url(urljoin(url, headers.get("location", "")))
                            elif status != 206:
                                raise ValueError("upstream_requires_valid_206")
                            else:
                                content_length = valid_response(headers, start, end)
                                self.send_response(206)
                                self.send_header("Content-Type", "video/mp4" if kind == "video" else "audio/mp4")
                                self.send_header("Content-Range", headers["content-range"])
                                self.send_header("Content-Length", str(content_length))
                                self.send_header("Accept-Ranges", "bytes")
                                self.send_header("Connection", "close")
                                self.end_headers()
                                self.upstream_headers_sent = True
                                row["range_validated"] = True
                        elif ":" in line:
                            key, value = line.split(":", 1)
                            headers[key.lower()] = value.strip()
                        return len(data)
                    except Exception as exc:
                        aborted = str(exc) if isinstance(exc, ValueError) else type(exc).__name__
                        return CURL_WRITEFUNC_ERROR

                def receive_body(data):
                    nonlocal aborted
                    if redirect:
                        return CURL_WRITEFUNC_ERROR
                    if not self.upstream_headers_sent or state.stop.is_set() or time.monotonic() >= state.deadline:
                        aborted = "transfer_stopped"
                        return CURL_WRITEFUNC_ERROR
                    try:
                        if content_length is None or row["bytes"] + len(data) > content_length:
                            raise ValueError("body_length_limit")
                        # Blocks on FFmpeg's socket; no detached reader/queue.
                        self.wfile.write(data)
                        self.wfile.flush()
                        row["bytes"] += len(data)
                        state.received(kind, len(data))
                        return len(data)
                    except (BrokenPipeError, ConnectionResetError, TimeoutError, OSError):
                        row["cancelled_by_downstream"] = True
                        aborted = "downstream_closed"
                        return CURL_WRITEFUNC_ERROR
                    except Exception as exc:
                        aborted = type(exc).__name__
                        return CURL_WRITEFUNC_ERROR

                try:
                    if handle.impersonate(DEFAULT_CHROME) != 0:
                        raise RuntimeError("impersonation_unavailable")
                    handle.setopt(CurlOpt.URL, url.encode())
                    handle.setopt(CurlOpt.HTTPHEADER, [
                        ("Referer: " + state.referer).encode(),
                        ("Range: bytes=%d-%d" % (start, end)).encode(),
                        b"Accept-Encoding: identity"])
                    handle.setopt(CurlOpt.FOLLOWLOCATION, 0)
                    handle.setopt(CurlOpt.PROXY, b"")
                    handle.setopt(CurlOpt.CONNECTTIMEOUT, 8)
                    handle.setopt(CurlOpt.TIMEOUT, max(1, min(75, int(state.deadline - time.monotonic()))))
                    handle.setopt(CurlOpt.MAX_RECV_SPEED_LARGE, (96 if kind == "video" else 24) * 1024)
                    handle.setopt(CurlOpt.HEADERFUNCTION, receive_header)
                    handle.setopt(CurlOpt.WRITEFUNCTION, receive_body)
                    try:
                        handle.perform()
                    except Exception as exc:
                        if not redirect and not row["cancelled_by_downstream"] and not state.stop.is_set():
                            row["error"] = aborted or type(exc).__name__
                    if redirect:
                        url = redirect
                        continue
                    row["completed"] = content_length is not None and row["bytes"] == content_length
                    if aborted and not row["cancelled_by_downstream"] and not state.stop.is_set():
                        row["error"] = aborted
                    break
                finally:
                    handle.close()
            else:
                row["error"] = "redirect_limit"
        except Exception as exc:
            row["error"] = type(exc).__name__
            raise
        finally:
            with state.lock:
                state.active -= 1


def run_case(args, sources, resolution):
    seconds = args.seconds
    output = args.output_dir / (resolution["id"] + "-live.flv")
    state = RelayState(sources, "https://www.bilibili.com/video/" + args.bvid + "/", time.monotonic() + 90)
    server = RelayServer(("127.0.0.1", 0), RelayHandler)
    server.state = state
    serving = threading.Thread(target=server.serve_forever, daemon=True)
    serving.start()
    base = "http://127.0.0.1:%d/%s/" % (server.server_address[1], state.token)
    command = [args.ffmpeg, "-hide_banner", "-nostdin", "-y", "-v", "warning",
               "-filter_threads", "2", "-filter_complex_threads", "2",
               "-threads", "2", "-re", "-rw_timeout", "15000000", "-i", base + "video",
               "-threads", "2", "-re", "-rw_timeout", "15000000", "-i", base + "audio",
               "-t", str(seconds), "-map", "0:v:0", "-map", "1:a:0",
               "-vf", "scale=%d:%d:flags=bilinear,fps=%d" %
                   (resolution["width"], resolution["height"], resolution["target_fps"]),
               "-c:v", "mjpeg", "-threads:v", "2", "-q:v", str(args.quality), "-pix_fmt", "yuvj420p",
               "-c:a", "pcm_s16le", "-ar", "22050", "-ac", "1", "-progress", "pipe:1",
               "-stats_period", "0.25", "-nostats", "-f", "flv", str(output)]
    # No upstream source URL or Cookie is placed in argv, environment or disk.
    started = time.monotonic()
    process = None
    diagnostic = bytearray()
    progress_count = 0
    timed_out = False
    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        def read_progress():
            nonlocal progress_count
            frames = 0
            for line in process.stdout:
                if line.startswith(b"frame="):
                    try:
                        frames = int(line.split(b"=", 1)[1])
                    except ValueError:
                        pass
                if line.startswith(b"out_time_us="):
                    try:
                        if frames > 0 and int(line.split(b"=", 1)[1]) > 0 and output.exists() and output.stat().st_size > 4096:
                            progress_count += 1
                            state.output_witness(output.stat().st_size)
                    except (ValueError, OSError):
                        pass

        def read_errors():
            for line in process.stderr:
                if len(diagnostic) < 65536:
                    diagnostic.extend(line[:65536 - len(diagnostic)])

        readers = [threading.Thread(target=read_progress, daemon=True),
                   threading.Thread(target=read_errors, daemon=True)]
        for reader in readers:
            reader.start()
        try:
            code = process.wait(timeout=90)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()
            code = process.wait(timeout=5)
        for reader in readers:
            reader.join(timeout=3)
    finally:
        state.stop.set()
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        server.shutdown()
        server.server_close()
        serving.join(timeout=2)
        # Every callback checks stop; allow a bounded interval for an in-flight
        # socket read to return before reporting the final transfer counters.
        cleanup_deadline = time.monotonic() + 10
        while state.active and time.monotonic() < cleanup_deadline:
            time.sleep(0.1)
    wall = time.monotonic() - started
    relay = state.snapshot()
    result = {"resolution": resolution, "file": output.name, "wall_seconds": wall,
              "exit_code": code, "timed_out": timed_out, "progress_updates": progress_count,
              "relay": relay, "jpeg_quality": args.quality,
              "rate_control_scope": "Fixed JPEG quality for live path verification; no target-rate or adaptive-rate claim",
              "stderr": diagnostic.decode("utf-8", "replace")[-4000:]}
    if code == 0 and not timed_out and output.exists():
        audio = {"sample_rate_hz": 22050, "channels": 1}
        result["verification"] = verify_output(args.ffmpeg, args.ffprobe, output, resolution, audio, seconds)
        result["file_bytes"] = output.stat().st_size
        result["sha256"] = hashlib.sha256(output.read_bytes()).hexdigest()
        result["total_file_kbps"] = output.stat().st_size * 8 / seconds / 1000
    errors = [row["error"] for row in relay["requests"] if row.get("error")]
    result["passed"] = bool(code == 0 and not timed_out and relay["concurrent_stream_witness"]
                            and not errors and relay["active_after_shutdown"] == 0
                            and result.get("verification", {}).get("passed"))
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
              "scope": "Real public upstream HTTP range transfer concurrent with server transcoding; local file decode only",
              "source_files_created": False, "esp32_playback_tested": False, "cases": []}
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
            print("LIVE_CASE", profile["id"], "PASS" if case["passed"] else "FAIL", flush=True)
        report["passed"] = len(report["cases"]) == 2 and all(case["passed"] for case in report["cases"])
    except Exception as exc:
        report["passed"] = False
        report["error_class"] = type(exc).__name__  # Never serialize URL-bearing exceptions.
    dump(args.output_dir / "report.json", report)
    print("LIVE_PROBE", "PASS" if report["passed"] else "FAIL", flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
