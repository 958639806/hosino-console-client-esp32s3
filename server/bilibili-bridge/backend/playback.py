"""One bounded MJPEG/PCM playback job; stream bytes never enter a source cache."""
from __future__ import annotations

from collections import deque
from http.server import BaseHTTPRequestHandler
import json
import os
from pathlib import Path
import re
import select
import secrets
import subprocess
import tempfile
import threading
import time
from urllib.parse import urljoin

from common import ApiError, bvid, position, positive, require
from live_relay_probe import RelayServer, request_range, valid_response
from real_stream_probe import media_url

PROFILES = {"standard": {"width": 320, "height": 180, "fps": 20, "auto_kbps": 1800},
            "clear": {"width": 480, "height": 270, "fps": 15, "auto_kbps": 2500}}
RATES = (0, 800, 1200, 1800, 2500, 3500)
MEDIA_ERROR = re.compile(r"\b(?:error|invalid|corrupt(?:ed)?|premature(?:ly)?|partial|failed)\b", re.I)


def sample_statistics(packets, seconds, fps):
    """Validate the complete actual sample before using its measured bitrate."""
    expected = round(seconds * fps)
    require(bool(packets) and abs(len(packets) - expected) <= 1,
            "sample_incomplete", "视频预处理画面不完整，请重试", 502)
    try:
        timestamps = [float(packet["pts_time"]) for packet in packets]
        sizes = [int(packet["size"]) for packet in packets]
    except (ValueError, TypeError, KeyError):
        raise ApiError("sample_timestamps", "视频预处理时间戳无效", 502) from None
    require(all(timestamp == timestamp and abs(timestamp) < 86400 for timestamp in timestamps)
            and all(size > 0 for size in sizes)
            and all(a < b for a, b in zip(timestamps, timestamps[1:])),
            "sample_timestamps", "视频预处理时间戳无效", 502)
    first, end = timestamps[0], timestamps[-1] + 1 / fps
    actual_seconds = end - first
    tolerance = max(1 / fps, 0.15)
    require(-0.002 <= first <= tolerance and actual_seconds > 0
            and abs(end - seconds) <= tolerance and abs(actual_seconds - seconds) <= tolerance,
            "sample_duration", "视频预处理时长不完整，请重试", 502)
    bins = {}
    for timestamp, size in zip(timestamps, sizes):
        second = int(max(0, timestamp))
        bins[second] = bins.get(second, 0) + size * 8 / 1000
    return {"frames": len(packets), "first_pts": first, "end_pts": end,
            "duration_seconds": actual_seconds,
            "average_video_kbps": sum(sizes) * 8 / actual_seconds / 1000,
            "peak_1s_video_kbps": max(bins.values()) / min(1, actual_seconds)}


class SourceServer(RelayServer):
    """Count source connections from accept through complete handler cleanup."""
    def __init__(self, job):
        self.job = job
        super().__init__(("127.0.0.1", 0), CDNHandler)

    def get_request(self):
        # Accept+publication are atomic with the preparation drain barrier.
        with self.job.source_condition:
            request, address = super().get_request()
            request.settimeout(2)
            self.job.source_active_connections += 1
            with self.job.manager.global_source_condition:
                self.job.manager.global_source_active_connections += 1
            return request, address

    def connection_finished(self):
        with self.job.source_condition:
            self.job.source_active_connections -= 1
            with self.job.manager.global_source_condition:
                self.job.manager.global_source_active_connections -= 1
                self.job.manager.global_source_condition.notify_all()
            self.job.source_condition.notify_all()

    def process_request(self, request, address):
        try:
            super().process_request(request, address)
        except Exception:
            self.connection_finished()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.connection_finished()

    def handle_error(self, *_):
        pass


def safe_diagnostic(line):
    text = line.decode("utf-8", "replace") if isinstance(line, bytes) else str(line)
    text = re.sub(r"https?://[^\s\"'<>]+", "<media-url>", text)
    text = re.sub(r"(?i)\b(?:authorization|cookie)\s*[:=].*", "<private>", text)
    text = re.sub(r"(?i)\b(?:cookie|authorization|sessdata|bili_jct|qrcode_key)\s*[:=]\s*[^\s,;]+", "<private>", text)
    return "".join(char for char in text if ord(char) >= 32)[:300]


class CDNHandler(BaseHTTPRequestHandler):
    """Synchronous libcurl body callbacks reuse the proven Range validators.

    Production ingress caps are higher than the deliberately throttled probe:
    1MiB/s video and256KiB/s audio; OS socket backpressure remains effective.
    """
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_GET(self):
        job = self.server.job
        generation = job.nonce
        kind = self.path.removeprefix("/" + generation + "/")
        if self.path != "/" + generation + "/" + kind or kind not in job.sources or generation != job.nonce:
            with job.lock:
                job.report["stale_source_requests"] = job.report.get("stale_source_requests", 0) + 1
            self.send_error(404)
            return
        if job.stopped.is_set() or generation != job.nonce or time.monotonic() >= job.deadline:
            self.send_error(410)
            return
        if not job.relay_slots.acquire(blocking=False):
            with job.lock:
                job.report["cdn_requests"].append({"kind": kind, "phase": getattr(job, "status", "unknown"),
                    "range_start": None, "range_end": None, "statuses": [], "expected_bytes": None,
                    "received_bytes": 0, "curl_error_code": None, "error": "relay_slots_busy",
                    "seconds": 0, "cancelled": job.stopped.is_set(), "local_status": 503})
            self.send_error(503)
            return
        sent = False
        row = {"kind": kind, "phase": getattr(job, "status", "unknown"), "range_start": None, "range_end": None,
               "statuses": [], "expected_bytes": None, "received_bytes": 0,
               "curl_error_code": None, "error": None, "seconds": None, "cancelled": False, "local_status": None}
        started = time.monotonic()
        with job.lock:
            job.report["cdn_requests"].append(row)
        try:
            from curl_cffi import Curl, CurlOpt
            from curl_cffi.curl import CURL_WRITEFUNC_ERROR
            from curl_cffi.requests.impersonate import DEFAULT_CHROME
            self.connection.settimeout(2)
            start, end = request_range(self.headers.get("Range"), 512 * 1024 if kind == "video" else 128 * 1024)
            row["range_start"], row["range_end"] = start, end
            url = job.sources[kind][0]
            for _ in range(4):
                require(not job.stopped.is_set() and generation == job.nonce and time.monotonic() < job.deadline,
                        "playback_stopped", "播放已经停止", 409)
                media_url(url)
                status, header_bytes, expected, received = 0, 0, None, 0
                headers = {}
                redirect = None
                transfer_deadline = min(job.deadline, time.monotonic() + 8)
                handle = Curl()

                def header(data):
                    nonlocal status, headers, header_bytes, expected, sent, redirect
                    try:
                        if job.stopped.is_set() or generation != job.nonce or time.monotonic() >= transfer_deadline:
                            row["cancelled"] = True
                            return CURL_WRITEFUNC_ERROR
                        header_bytes += len(data)
                        if len(data) > 16384 or header_bytes > 65536:
                            return CURL_WRITEFUNC_ERROR
                        line = data.decode("iso-8859-1").strip()
                        if line.startswith("HTTP/"):
                            status, headers = int(line.split()[1]), {}
                        elif not line and status >= 200:
                            row["statuses"].append(status)
                            if status in (301, 302, 303, 307, 308):
                                redirect = media_url(urljoin(url, headers.get("location", "")))
                            elif status == 206:
                                expected = valid_response(headers, start, end)
                                row["expected_bytes"] = expected
                                self.send_response(206)
                                row["local_status"] = 206
                                self.send_header("Content-Type", "video/mp4" if kind == "video" else "audio/mp4")
                                self.send_header("Content-Range", headers["content-range"])
                                self.send_header("Content-Length", str(expected))
                                self.send_header("Accept-Ranges", "bytes")
                                self.send_header("Connection", "close")
                                remaining = transfer_deadline - time.monotonic()
                                if remaining <= 0:
                                    return CURL_WRITEFUNC_ERROR
                                self.connection.settimeout(min(2, remaining))
                                self.end_headers()
                                sent = True
                            else:
                                row["error"] = "upstream_http_" + str(status)
                                return CURL_WRITEFUNC_ERROR
                        elif ":" in line:
                            key, value = line.split(":", 1)
                            headers[key.lower()] = value.strip()
                        return len(data)
                    except Exception as exc:
                        row["error"] = "header_" + type(exc).__name__
                        return CURL_WRITEFUNC_ERROR

                def body(data):
                    nonlocal received
                    if redirect or not sent or job.stopped.is_set() or generation != job.nonce or time.monotonic() >= job.deadline:
                        if job.stopped.is_set() or generation != job.nonce:
                            row["cancelled"] = True
                        return CURL_WRITEFUNC_ERROR
                    try:
                        if expected is None or received + len(data) > expected:
                            row["error"] = "body_length"
                            return CURL_WRITEFUNC_ERROR
                        with job.lock:
                            if job.source_bytes + len(data) > 1024 * 1024 * 1024:
                                row["error"] = "job_byte_limit"
                                return CURL_WRITEFUNC_ERROR
                            job.source_bytes += len(data)
                        remaining = transfer_deadline - time.monotonic()
                        if remaining <= 0:
                            row["error"] = "body_deadline"
                            return CURL_WRITEFUNC_ERROR
                        self.connection.settimeout(min(2, remaining))
                        self.wfile.write(data)
                        self.wfile.flush()
                        received += len(data)
                        row["received_bytes"] += len(data)
                        return len(data)
                    except OSError as exc:
                        row["error"] = "downstream_" + type(exc).__name__
                        return CURL_WRITEFUNC_ERROR

                try:
                    require(handle.impersonate(DEFAULT_CHROME) == 0, "curl_impersonation", "上游连接初始化失败", 500)
                    handle.setopt(CurlOpt.URL, url.encode())
                    handle.setopt(CurlOpt.HTTPHEADER, [("Referer: https://www.bilibili.com/video/" + job.bvid + "/").encode(),
                        ("Range: bytes=%d-%d" % (start, end)).encode(), b"Accept-Encoding: identity"])
                    handle.setopt(CurlOpt.FOLLOWLOCATION, 0)
                    handle.setopt(CurlOpt.PROXY, b"")
                    # curl_cffi0.17 has no supported XFERINFO callback. Use
                    # finite small ranges plus a strict native timeout so even
                    # a stall without another WRITEFUNCTION releases its slot.
                    handle.setopt(CurlOpt.CONNECTTIMEOUT, 4)
                    handle.setopt(CurlOpt.TIMEOUT, max(1, min(8, int(job.deadline - time.monotonic()))))
                    handle.setopt(CurlOpt.LOW_SPEED_LIMIT, 1024)
                    handle.setopt(CurlOpt.LOW_SPEED_TIME, 4)
                    handle.setopt(CurlOpt.MAX_RECV_SPEED_LARGE, 1024 * 1024 if kind == "video" else 256 * 1024)
                    handle.setopt(CurlOpt.HEADERFUNCTION, header)
                    handle.setopt(CurlOpt.WRITEFUNCTION, body)
                    try:
                        handle.perform()
                    except Exception as exc:
                        row["curl_error_code"] = int(getattr(exc, "code", 0))
                        if job.stopped.is_set() or generation != job.nonce:
                            row["cancelled"] = True
                        elif not redirect and not row["error"]:
                            row["error"] = "curl_" + type(exc).__name__
                        if not redirect and not sent:
                            raise ApiError("source_transfer", "上游视频读取失败", 502) from None
                    if redirect:
                        url = redirect
                        continue
                    break
                finally:
                    handle.close()
        except Exception as exc:
            if not row["error"] and not job.stopped.is_set() and generation == job.nonce:
                row["error"] = type(exc).__name__
            row["cancelled"] = job.stopped.is_set() or generation != job.nonce
            if not sent:
                try:
                    self.send_error(502, "upstream media failed")
                    row["local_status"] = 502
                except OSError:
                    pass
        finally:
            row["seconds"] = time.monotonic() - started
            self.close_connection = True
            job.relay_slots.release()


class Playback:
    def __init__(self, manager, raw, cid, profile, rate, source_position, seconds):
        self.manager, self.bvid, self.cid = manager, raw["bvid"], cid
        self.profile_name, self.profile = profile, PROFILES[profile]
        self.requested_kbps, self.target_kbps = rate, rate or self.profile["auto_kbps"]
        self.position, self.duration, self.seconds = source_position, seconds, seconds - source_position
        self.session_id, self.nonce = secrets.token_urlsafe(18), secrets.token_hex(16)
        self.created, self.deadline = time.monotonic(), time.monotonic() + min(3630, self.seconds + 90)
        self.status, self.error = "preparing", None
        self.quality, self.sample_kbps, self.sample_peak_kbps = None, None, None
        self.lock = threading.RLock()
        self.source_condition = threading.Condition(self.lock)
        self.source_active_connections = 0
        self.stopped, self.ready = threading.Event(), threading.Event()
        self.relay_slots = manager.relay_slots
        self.processes = set()
        self.sources, self.server, self.serving = {}, None, None
        self.source_bytes, self.output_bytes, self.claimed = 0, 0, False
        self.sample_attempts = []
        self.report = {"encoder_exit_code": None, "encoder_errors": deque(maxlen=8),
                       "sample_errors": deque(maxlen=8), "cdn_requests": deque(maxlen=64),
                       "source_drains": deque(maxlen=8), "stale_source_requests": 0}
        self.directory = tempfile.TemporaryDirectory(prefix="play-", dir=manager.work_dir)
        self.worker = threading.Thread(target=self.prepare, daemon=True)
        self.worker.start()

    def public(self):
        with self.lock:
            return {"session_id": self.session_id, "stream_path": "/v1/streams/" + self.session_id,
                    "duration": self.duration, "position_seconds": self.position, "profile": self.profile_name,
                    "video_kbps": self.requested_kbps, "status": self.status, "selected_quality": self.quality,
                    "target_video_kbps": self.target_kbps, "sample_video_kbps": self.sample_kbps,
                    "sample_peak_1s_video_kbps": self.sample_peak_kbps,
                    "rate_control": "3s_sample_average_not_CBR_or_full_video_guarantee",
                    "error": self.error,
                    "diagnostics": {"encoder_exit_code": self.report["encoder_exit_code"],
                        "encoder_errors": list(self.report["encoder_errors"]),
                        "sample_errors": list(self.report["sample_errors"]),
                        "cdn_requests": [dict(row) for row in list(self.report["cdn_requests"])[-8:]],
                        "source_bytes": self.source_bytes, "output_bytes": self.output_bytes,
                        "source_active_connections": self.source_active_connections,
                        "global_source_active_connections": self.manager.global_source_active_connections,
                        "source_drains": list(self.report["source_drains"]),
                        "stale_source_requests": self.report["stale_source_requests"],
                        "sample_attempts": list(self.sample_attempts)}}

    def start_process(self, command, **kwargs):
        with self.lock:
            require(not self.stopped.is_set(), "playback_stopped", "播放已经停止", 409)
            process = subprocess.Popen(command, **kwargs)
            self.processes.add(process)
            return process

    def finish_process(self, process):
        with self.lock:
            self.processes.discard(process)

    def source_url(self, kind):
        return "http://127.0.0.1:%d/%s/%s" % (self.server.server_address[1], self.nonce, kind)

    def base_command(self):
        return [self.manager.ffmpeg, "-hide_banner", "-nostdin", "-y", "-v", "error", "-filter_threads", "2", "-filter_complex_threads", "2"]

    def input_command(self, kind, realtime=False):
        return ["-threads", "2", *(["-re"] if realtime else []), "-rw_timeout", "15000000",
                "-ss", str(self.position), "-i", self.source_url(kind)]

    def video_command(self, quality):
        return ["-vf", "scale=%d:%d:flags=bilinear,fps=%d" %
                (self.profile["width"], self.profile["height"], self.profile["fps"]),
                "-c:v", "mjpeg", "-threads:v", "2", "-q:v", str(quality), "-pix_fmt", "yuvj420p"]

    def run_small(self, command, timeout=12):
        process = self.start_process(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            stdout, stderr = process.communicate(timeout=timeout)
            require(process.returncode == 0, "sample_failed", "视频预处理失败，请重试", 502)
            require(len(stdout) <= 4_000_000 and len(stderr) <= 65536, "sample_output_limit", "视频预处理数据过大", 502)
            media_errors = [safe_diagnostic(line) for line in stderr.splitlines()
                            if MEDIA_ERROR.search(line.decode("utf-8", "replace"))]
            if media_errors:
                with self.lock:
                    self.report["sample_errors"].extend(media_errors[-8:])
            require(not media_errors, "sample_media_errors", "视频预处理出现解码错误，请重试", 502)
            return stdout
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=3)
            raise ApiError("sample_timeout", "视频预处理超时，请重试", 504) from None
        finally:
            self.finish_process(process)

    def wait_sources_idle(self, reason, timeout=9):
        """A sample process exiting does not imply its Curl threads have ended.

        Wait for accepted handlers and the pending accept queue to drain before
        spending slots on another sample or publishing the actual stream.
        """
        started = time.monotonic()
        limit = min(self.deadline, started + timeout)
        with self.source_condition:
            # All FFmpeg processes that could use the previous private paths
            # have exited. Reject any late queued request from that generation;
            # existing native callbacks abort as soon as they resume.
            self.nonce = secrets.token_hex(16)
            initial = self.source_active_connections
            while True:
                require(not self.stopped.is_set(), "playback_stopped", "播放已经停止", 409)
                pending = bool(self.server and select.select([self.server.socket], [], [], 0)[0])
                if self.source_active_connections == 0 and not pending:
                    self.report["source_drains"].append({"reason": reason, "initial_connections": initial,
                                                         "seconds": time.monotonic() - started})
                    return
                remaining = limit - time.monotonic()
                require(remaining > 0, "source_drain_timeout", "视频预处理连接清理超时，请重试", 504)
                self.source_condition.wait(min(remaining, 0.05 if pending else remaining))

    def prepare(self):
        try:
            self.manager.wait_prior_sources(self)
            self.sources = self.manager.client.sources(self.bvid, self.cid)
            # Publication and start are atomic with respect to stop(). No relay
            # may appear after DELETE has already cleaned a preparing job.
            with self.lock:
                require(not self.stopped.is_set(), "playback_stopped", "播放已经停止", 409)
                self.server = SourceServer(self)
                self.serving = threading.Thread(target=self.server.serve_forever, daemon=True)
                self.serving.start()
            sample_seconds = min(3.0, self.seconds)
            for quality in (2, 4, 8, 12, 20, 31):
                require(time.monotonic() - self.created < 42, "prepare_timeout", "视频预处理超过时间限制", 504)
                sample = Path(self.directory.name) / "sample.flv"
                self.run_small(self.base_command() + self.input_command("video") +
                    ["-t", str(sample_seconds), "-map", "0:v:0", *self.video_command(quality), "-an", "-f", "flv", str(sample)])
                self.wait_sources_idle("sample_q" + str(quality))
                raw = self.run_small([self.manager.ffprobe, "-v", "error", "-show_packets", "-show_entries",
                    "packet=size,pts_time", "-of", "json", str(sample)], timeout=5)
                packets = json.loads(raw).get("packets") or []
                measured = sample_statistics(packets, sample_seconds, self.profile["fps"])
                rate, peak = measured["average_video_kbps"], measured["peak_1s_video_kbps"]
                self.sample_attempts.append({"quality": quality, **measured})
                self.quality, self.sample_kbps, self.sample_peak_kbps = quality, rate, peak
                if rate <= self.target_kbps:
                    break
            require(self.sample_kbps <= self.target_kbps, "bitrate_unattainable", "该内容在所选码率下仍过大，请提高码率", 422)
            sample.unlink(missing_ok=True)
            self.wait_sources_idle("before_ready")
            with self.lock:
                if not self.stopped.is_set():
                    self.status = "ready"
            self.ready.set()
            # A prepared job cannot keep resources forever without a consumer.
            for _ in range(240):
                if self.stopped.wait(0.25) or self.claimed:
                    return
            self.stop()
        except ApiError as exc:
            if self.stopped.is_set():
                self.ready.set()
                return
            self.error = {"code": exc.code, "message": exc.message}
            self.status = "error"
            self.ready.set()
            self.stop(preserve_error=True)
        except Exception:
            if self.stopped.is_set():
                self.ready.set()
                return
            self.error = {"code": "playback_prepare", "message": "播放准备失败，请重试"}
            self.status = "error"
            self.ready.set()
            self.stop(preserve_error=True)

    def stream(self, handler):
        require(not self.claimed, "stream_claimed", "该播放已经被打开", 409)
        limit = min(self.created + 45, self.deadline)
        while not self.ready.wait(0.1):
            require(time.monotonic() < limit, "prepare_timeout", "等待播放准备超时", 504)
            if handler.peer_gone():
                self.stop()
                return
        if self.error:
            raise ApiError(self.error["code"], self.error["message"], 502)
        with self.lock:
            require(not self.claimed and not self.stopped.is_set(), "playback_stopped", "播放已经停止", 409)
            self.claimed, self.status = True, "playing"
        command = self.base_command() + self.input_command("video", True) + self.input_command("audio", True) + [
            "-t", str(self.seconds), "-map", "0:v:0", "-map", "1:a:0", *self.video_command(self.quality),
            "-c:a", "pcm_s16le", "-ar", "22050", "-ac", "1", "-flush_packets", "1",
            "-flvflags", "no_duration_filesize", "-f", "flv", "pipe:1"]
        process = self.start_process(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        diagnostics = deque(maxlen=16)

        def read_errors():
            for line in process.stderr:
                # Only flag presence; upstream source URLs never enter argv/logs.
                diagnostics.append(bool(line.strip()))
                if line.strip():
                    with self.lock:
                        self.report["encoder_errors"].append(safe_diagnostic(line))

        errors = threading.Thread(target=read_errors, daemon=True)
        errors.start()
        watchdog = threading.Timer(max(1, self.deadline - time.monotonic()), self.stop)
        watchdog.daemon = True
        watchdog.start()
        try:
            handler.send_response(200)
            handler.send_header("Content-Type", "video/x-flv")
            handler.send_header("Transfer-Encoding", "chunked")
            handler.send_header("Cache-Control", "no-store")
            handler.send_header("Connection", "close")
            handler.end_headers()
            handler.stream_started = True
            while not self.stopped.is_set():
                block = process.stdout.read(32768)
                if not block:
                    break
                handler.wfile.write(("%x\r\n" % len(block)).encode() + block + b"\r\n")
                handler.wfile.flush()
                self.output_bytes += len(block)
            code = process.wait(timeout=5)
            self.report["encoder_exit_code"] = code
            errors.join(timeout=2)
            if code == 0 and not diagnostics and not self.stopped.is_set():
                handler.wfile.write(b"0\r\n\r\n")
                handler.wfile.flush()
                self.status = "ended"
            elif not self.stopped.is_set():
                self.status = "error"
                self.error = {"code": "transcode_failed", "message": "转码中断，请重新播放"}
        except (OSError, subprocess.TimeoutExpired):
            self.status = "stopped"
        finally:
            handler.close_connection = True
            watchdog.cancel()
            self.stop(preserve_error=self.status == "error", preserve_ended=self.status == "ended")
            self.report["encoder_exit_code"] = process.poll()
            self.finish_process(process)

    def stop(self, preserve_error=False, preserve_ended=False):
        with self.lock:
            self.stopped.set()
            self.source_condition.notify_all()
            global_condition = getattr(getattr(self, "manager", None), "global_source_condition", None)
            if global_condition is not None:
                with global_condition:
                    global_condition.notify_all()
            self.ready.set()
            if not preserve_error and not preserve_ended:
                self.status = "stopped"
            processes = list(self.processes)
        for process in processes:
            if process.poll() is None:
                try:
                    process.terminate()
                except ProcessLookupError:
                    continue
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
        if self.server is not None:
            if self.serving is not None and self.serving.is_alive():
                self.server.shutdown()
            self.server.server_close()
        # Small generated sample files only; never source caches or cookies.
        try:
            self.directory.cleanup()
        except OSError:
            pass


class PlaybackManager:
    def __init__(self, client, ffmpeg, ffprobe, work_dir):
        self.client, self.ffmpeg, self.ffprobe, self.work_dir = client, ffmpeg, ffprobe, work_dir
        self.lock, self.current = threading.RLock(), None
        self.relay_slots = threading.BoundedSemaphore(4)
        # Separate from manager.lock: stop() takes manager.lock then job.lock;
        # source accounting takes job.lock then this independent condition.
        self.global_source_condition = threading.Condition()
        self.global_source_active_connections = 0

    def wait_prior_sources(self, job, timeout=9):
        started = time.monotonic()
        limit = min(job.deadline, started + timeout)
        with self.global_source_condition:
            initial = self.global_source_active_connections
            while self.global_source_active_connections:
                require(not job.stopped.is_set(), "playback_stopped", "播放已经停止", 409)
                remaining = limit - time.monotonic()
                require(remaining > 0, "previous_source_drain_timeout", "上一段播放连接清理超时，请重试", 504)
                self.global_source_condition.wait(remaining)
        with job.lock:
            require(not job.stopped.is_set(), "playback_stopped", "播放已经停止", 409)
            job.report["source_drains"].append({"reason": "previous_jobs", "initial_connections": initial,
                                               "seconds": time.monotonic() - started})

    def create(self, body):
        require(set(body) <= {"bvid", "cid", "profile", "video_kbps", "position_seconds"}, message="播放参数包含未知字段")
        identifier = bvid(body.get("bvid"))
        profile, rate = body.get("profile", "standard"), body.get("video_kbps", 0)
        require(isinstance(profile, str) and profile in PROFILES and type(rate) is int and rate in RATES,
                message="清晰度或码率不正确")
        source_position = position(body.get("position_seconds", 0))
        with self.lock:
            require(self.current is None or self.current.stopped.is_set(), "playback_busy", "请先停止当前播放", 409)
            raw = self.client.detail_raw(identifier)
            pages = raw.get("pages") or []
            cid = positive(body["cid"] if "cid" in body else raw.get("cid"), "cid")
            part = next((part for part in pages if part.get("cid") == cid), None)
            require(part is not None, "cid_mismatch", "该分P不属于所选视频")
            seconds = positive(part.get("duration"), "duration", 86400)
            require(source_position < seconds and seconds - source_position <= 3600,
                    "duration_limit", "当前支持从所选位置连续播放最多一小时", 422)
            self.current = Playback(self, raw, cid, profile, rate, source_position, seconds)
            return self.current.public()

    def get(self, identifier):
        with self.lock:
            require(self.current is not None and self.current.session_id == identifier,
                    "playback_not_found", "播放会话不存在", 404)
            return self.current

    def stop(self, identifier=None):
        with self.lock:
            if self.current and (identifier is None or self.current.session_id == identifier):
                self.current.stop()
                return {"stopped": True}
            require(identifier is None, "playback_not_found", "播放会话不存在", 404)
            return {"stopped": True}
