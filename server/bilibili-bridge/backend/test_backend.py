"""Offline contract/security/lifecycle tests. No Bilibili account/network used."""
from __future__ import annotations

from collections import deque
from http.cookiejar import CookieJar
import http.client
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from bili import BiliClient, handoff_url, video_row, wbi_parameters
from bridge import Application, BridgeServer, Handler
from common import ApiError, bvid, position, private_directory, private_read, private_write
from playback import Playback, PlaybackManager, SourceServer, safe_diagnostic, sample_statistics
from live_relay_probe import RelayServer
from real_stream_probe import media_url


class Cookies:
    def __init__(self):
        self.jar = CookieJar()

    def set(self, key, value, domain="", path="/"):
        from http.cookiejar import Cookie
        self.jar.set_cookie(Cookie(0, key, value, None, False, domain, True, domain.startswith("."),
                                   path, True, False, None, True, None, None, {}, False))

    def clear(self):
        self.jar.clear()


class Response:
    def __init__(self, value, status=200, headers=None):
        self.value, self.status_code = value, status
        self.content = json.dumps(value).encode()
        self.headers = headers or Headers()

    def json(self):
        return self.value


class Headers(dict):
    def get_list(self, key):
        value = self.get(key, [])
        return value if isinstance(value, list) else [value]


class Session:
    def __init__(self, responses=()):
        self.cookies, self.responses, self.calls = Cookies(), list(responses), []

    def request(self, method, url, **kwargs):
        self.calls.append((method, url, kwargs))
        return self.responses.pop(0)

    def get(self, url, **kwargs):
        return self.request("GET", url, **kwargs)


class FakeClient:
    ready = True
    verified_logged_in = False

    def __init__(self):
        self.actions = []

    def feed(self, page):
        return {"items": [{"bvid": "BV13x41117TL", "cid": 1, "title": "测试", "author": "作者", "duration": 30, "cover": ""}], "page": page, "has_more": False}

    def me(self):
        return {"logged_in": False, "mid": None}

    def favorite(self, identifier, folder, action):
        self.actions.append((identifier, folder, action))
        return {"changed": True}

    def detail_raw(self, identifier):
        return {"bvid": identifier, "cid": 1, "pages": [{"cid": 1, "duration": 30}]}


class HTTPTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.client = FakeClient()
        self.token = "unit-test-device-bearer-value-12345"
        self.app = Application(self.token, Path(self.temp.name) / "state", "unused", "unused", client=self.client)
        self.server = BridgeServer(("127.0.0.1", 0), Handler, self.app)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.app.playbacks.stop()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def request(self, method, path, body=None, authorized=True, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1], timeout=3)
        merged = {"Content-Type": "application/json"}
        if authorized:
            merged["Authorization"] = "Bearer " + self.token
        merged.update(headers or {})
        connection.request(method, path, json.dumps(body) if body is not None else None, merged)
        response = connection.getresponse()
        status, result = response.status, json.loads(response.read())
        connection.close()
        return status, result

    def test_health_is_public_but_feed_requires_bearer(self):
        self.assertEqual(self.request("GET", "/v1/health", authorized=False)[0], 200)
        self.assertEqual(self.request("GET", "/v1/feed", authorized=False)[0], 401)
        status, result = self.request("GET", "/v1/feed?page=1")
        self.assertEqual(status, 200)
        self.assertEqual(result["data"]["items"][0]["title"], "测试")

    def test_account_https_boundary_cannot_be_enabled_by_header_alone(self):
        self.assertEqual(self.request("GET", "/v1/me", headers={"X-Forwarded-Proto": "https"})[0], 403)
        self.app.trusted_tls_proxy = True
        self.assertEqual(self.request("GET", "/v1/me")[0], 403)
        self.assertEqual(self.request("GET", "/v1/me", headers={"X-Forwarded-Proto": "https"})[0], 200)

    def test_get_or_invalid_body_cannot_write_favorite(self):
        self.app.trusted_tls_proxy = True
        headers = {"X-Forwarded-Proto": "https"}
        self.assertEqual(self.request("GET", "/v1/favorite", headers=headers)[0], 404)
        self.assertEqual(self.request("POST", "/v1/favorite", {"bvid": "BV13x41117TL"}, headers=headers)[0], 400)
        self.assertEqual(self.client.actions, [])
        self.assertEqual(self.request("POST", "/v1/favorite", {"bvid": "BV13x41117TL", "folder_id": 10, "action": "add"}, headers=headers)[0], 200)
        self.assertEqual(self.client.actions, [("BV13x41117TL", 10, "add")])

    def test_rejects_url_token_duplicate_query_and_arbitrary_media_url(self):
        for path in ("/v1/feed?token=forbidden", "/v1/feed?page=1&page=2", "/v1/feed?url=http://127.0.0.1"):
            self.assertEqual(self.request("GET", path)[0], 400)
        self.assertEqual(self.request("POST", "/v1/playbacks", {"bvid": "BV13x41117TL", "url": "http://127.0.0.1"})[0], 400)

    def test_http_disconnect_stops_real_stream_child(self):
        # Exercise the actual authenticated HTTP streaming/cancellation path;
        # the child emits arbitrary bytes, so this makes no codec/playback claim.
        producer = Path(self.temp.name) / "producer.py"
        producer.write_text("import sys,time\nwhile True:\n sys.stdout.buffer.write(b'x'*65536)\n sys.stdout.buffer.flush()\n time.sleep(.02)\n")
        job = Playback.__new__(Playback)
        job.lock, job.stopped, job.ready = threading.RLock(), threading.Event(), threading.Event()
        job.source_condition = threading.Condition(job.lock)
        job.ready.set()
        job.manager = self.app.playbacks
        job.session_id = "unit-stream"
        job.claimed, job.status, job.error = False, "ready", None
        job.created, job.deadline, job.seconds = time.monotonic(), time.monotonic() + 30, 30
        job.server, job.serving, job.quality, job.output_bytes = None, None, 8, 0
        job.processes = set()
        job.report = {"encoder_exit_code": None, "encoder_errors": deque(maxlen=8), "cdn_requests": deque(maxlen=64)}
        job.directory = tempfile.TemporaryDirectory(dir=self.temp.name)
        job.base_command = lambda: [sys.executable, str(producer)]
        job.input_command = lambda *_args: []
        job.video_command = lambda *_args: []
        launched = []
        original_start = job.start_process
        def start(command, **kwargs):
            process = original_start(command, **kwargs)
            launched.append(process)
            return process
        job.start_process = start
        self.app.playbacks.current = job
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1], timeout=3)
        connection.request("GET", "/v1/streams/unit-stream", headers={"Authorization": "Bearer " + self.token})
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        self.assertEqual(len(response.read(1024)), 1024)
        response.close()
        connection.close()
        self.assertTrue(job.stopped.wait(5), "disconnect did not cancel the producer")
        limit = time.monotonic() + 3
        while launched[0].poll() is None and time.monotonic() < limit:
            time.sleep(.01)
        self.assertIsNotNone(launched[0].poll())


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "cookies.json"

    def tearDown(self):
        self.temp.cleanup()

    def client(self, responses):
        result = BiliClient(self.path, Session(responses))
        result.ready = True
        return result

    def test_qr_wait_scan_expire_and_poll_throttling(self):
        client = self.client([Response({"code": 0, "data": {"code": 86090}})])
        client.qr["id"] = {"key": "private-key", "expires": time.monotonic() + 10, "last_poll": 0, "status": "waiting"}
        self.assertEqual(client.qr_poll("id"), {"status": "scanned"})
        self.assertEqual(client.qr_poll("id"), {"status": "scanned"})
        self.assertEqual(len(client.session.calls), 1)
        client.qr["id"]["expires"] = 0
        self.assertEqual(client.qr_poll("id"), {"status": "expired"})

    def test_success_without_new_credentials_is_not_previous_account_success(self):
        client = self.client([Response({"code": 0, "data": {"code": 0}})])
        client._set_cookie("SESSDATA", "old-value")
        client.qr["id"] = {"key": "private", "expires": time.monotonic() + 10, "last_poll": 0, "status": "waiting"}
        with self.assertRaises(ApiError) as caught:
            client.qr_poll("id")
        self.assertEqual(caught.exception.code, "qr_cookie_missing")
        self.assertFalse(self.path.exists())

    def test_normal_confirm_captures_new_cookie_and_persists_privately(self):
        client = self.client([Response({"code": 0, "data": {"code": 0,
            "url": "https://passport.bilibili.com/crossDomain?SESSDATA=new-value&bili_jct=new-csrf&DedeUserID=123"}}),
            Response({"code": 0, "data": {"isLogin": True, "mid": 123, "uname": "测试"}})])
        client.qr["id"] = {"key": "private", "expires": time.monotonic() + 10, "last_poll": 0, "status": "waiting"}
        self.assertEqual(client.qr_poll("id"), {"status": "confirmed"})
        self.assertEqual(json.loads(private_read(self.path))["cookies"]["SESSDATA"], "new-value")
        if os.name == "posix":
            self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)

    def test_normal_confirm_accepts_direct_set_cookie_without_handoff(self):
        client = self.client([Response({"code": 0, "data": {"code": 0}}, headers=Headers({"Set-Cookie": [
            "SESSDATA=direct-session; Path=/", "bili_jct=direct-csrf; Path=/", "DedeUserID=456; Path=/"]})),
            Response({"code": 0, "data": {"isLogin": True, "mid": 456}})])
        client.qr["id"] = {"key": "private", "expires": time.monotonic() + 10, "last_poll": 0, "status": "waiting"}
        self.assertEqual(client.qr_poll("id"), {"status": "confirmed"})
        self.assertEqual(client.cookie("SESSDATA"), "direct-session")

    def test_crossdomain_set_cookie_only_allowlisted_normal_login_fields(self):
        client = self.client([Response({}, headers=Headers({"Set-Cookie": [
            "SESSDATA=normal; Domain=.biligame.com; Path=/", "bili_jct=csrf; Path=/", "tracking=ignored; Path=/"]}))])
        client._capture_handoff("https://passport.biligame.com/x/passport-login/web/crossDomain?ticket=test")
        self.assertEqual(client.cookie("SESSDATA"), "normal")
        self.assertFalse(any(cookie.name == "tracking" for cookie in client.session.cookies.jar))

    def test_upstream_412_has_explicit_error_and_cooldown(self):
        client = self.client([Response({}, 412)])
        with self.assertRaises(ApiError) as caught:
            client.request("/x/web-interface/popular")
        self.assertEqual(caught.exception.code, "upstream_http_412")
        with self.assertRaises(ApiError) as caught:
            client.request("/x/web-interface/popular")
        self.assertEqual(caught.exception.code, "upstream_cooldown")
        self.assertEqual(len(client.session.calls), 1)

    def test_folder_and_favorite_pages_split_upstream_twenty(self):
        client = self.client([])
        client.own_folder = lambda value: int(value)
        client.request = lambda *_args, **_kwargs: {"medias": [{"bvid": "BV13x41117TL", "title": str(i)} for i in range(20)], "has_more": False}
        first, second = client.favorites(1, 1), client.favorites(1, 2)
        self.assertEqual([row["title"] for row in first["items"]], list(map(str, range(10))))
        self.assertEqual([row["title"] for row in second["items"]], list(map(str, range(10, 20))))
        self.assertTrue(first["has_more"])
        self.assertFalse(second["has_more"])


class BoundaryTests(unittest.TestCase):
    def test_diagnostic_redacts_urls_and_private_headers(self):
        value = safe_diagnostic(b'error https://cdn.invalid/video?secret=yes Cookie: private Authorization=private SESSDATA=private\n')
        self.assertNotIn('secret', value)
        self.assertNotIn('private', value.replace('<private>', ''))
        self.assertIn('<media-url>', value)
        bearer = safe_diagnostic('Authorization: Bearer synthetic-test-token')
        self.assertNotIn('synthetic-test-token', bearer)
        self.assertEqual(bearer, '<private>')

    def test_hosts_reject_private_targets_userinfo_ports_and_bad_protocols(self):
        for url in ("http://x.bilivideo.com/", "https://127.0.0.1/", "https://bilivideo.com.evil.invalid/", "https://u:p@x.bilivideo.com/", "https://x.bilivideo.com:444/"):
            with self.assertRaises(ValueError):
                media_url(url)
        for url in ("https://127.0.0.1/", "https://passport.bilibili.com.evil.invalid/", "http://passport.bilibili.com/", "https://passport.bilibili.com:444/"):
            with self.assertRaises(ApiError):
                handoff_url(url)

    def test_ui_rows_strip_html_and_preserve_unknowns(self):
        row = video_row({"title": '<em class="keyword">中文</em>&amp;测试', "duration": "01:02", "bvid": "BV13x41117TL"})
        self.assertEqual(row["title"], "中文&测试")
        self.assertEqual(row["duration"], 62)
        self.assertIsNone(row["cid"])

    def test_wbi_stable_vector_and_special_char_filter(self):
        key = "7cd084941338484aae1ad9425b84077c" + "4932caff0ff746eab6f01bf08b70ac45"
        result = wbi_parameters({"keyword": "中!'()文*", "page": 1}, key, 1700000000)
        self.assertEqual(result["keyword"], "中文")
        self.assertEqual(result["wts"], "1700000000")
        self.assertEqual(len(result["w_rid"]), 32)
        # Published independent vector: docs/misc/sign/wbi.md.
        vector = wbi_parameters({"foo": "114", "bar": "514", "zab": 1919810}, key, 1702204169)
        self.assertEqual(vector["w_rid"], "8f6f2b5b3d485fe1886cec6a0be8c5d4")

    def test_token_policy_matches_device_authorization_buffer(self):
        with tempfile.TemporaryDirectory() as directory:
            for token in ("x" * 193, "x" * 23, "中" * 24, "x" * 24 + " "):
                with self.assertRaises(ApiError):
                    Application(token, Path(directory) / "state", "unused", "unused", client=FakeClient())

    def test_invalid_cid_and_profile_cannot_start_worker(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = PlaybackManager(FakeClient(), "unused", "unused", directory)
            for extra in ({"cid": 0}, {"cid": 2}, {"profile": []}, {"video_kbps": True}, {"video_kbps": 1000}):
                with self.assertRaises(ApiError):
                    manager.create({"bvid": "BV13x41117TL", **extra})
            self.assertIsNone(manager.current)

    def test_input_boundaries(self):
        for value in (float("nan"), float("inf"), -1, True, "2"):
            with self.assertRaises(ApiError):
                position(value)
        for value in ("http://127.0.0.1", "BVbad", "../../secret"):
            with self.assertRaises(ApiError):
                bvid(value)

    def test_stop_terminates_real_child_process(self):
        with tempfile.TemporaryDirectory() as directory:
            job = Playback.__new__(Playback)
            job.lock, job.stopped, job.ready = threading.RLock(), threading.Event(), threading.Event()
            job.source_condition = threading.Condition(job.lock)
            job.status, job.server = "playing", None
            job.directory = tempfile.TemporaryDirectory(dir=directory)
            process = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
            job.processes = {process}
            job.stop()
            self.assertIsNotNone(process.poll())
            self.assertTrue(job.stopped.is_set())
            self.assertEqual(job.status, "stopped")

    def test_cancel_during_relay_creation_cannot_publish_after_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            client = FakeClient()
            client.sources = lambda *_: {"video": [], "audio": []}
            manager = PlaybackManager(client, "unused", "unused", directory)
            entered, release, stop_done = threading.Event(), threading.Event(), threading.Event()
            servers = []
            def delayed_server(job):
                server = SourceServer(job)
                servers.append(server)
                entered.set()
                release.wait(3)
                return server
            def stop():
                manager.stop()
                stop_done.set()
            with patch("playback.SourceServer", delayed_server), patch.object(Playback, "run_small", side_effect=ApiError("cancelled", "测试取消")):
                manager.create({"bvid": "BV13x41117TL"})
                self.assertTrue(entered.wait(3))
                stopper = threading.Thread(target=stop)
                stopper.start()
                self.assertFalse(stop_done.wait(.05), "stop returned before pending relay publication")
                release.set()
                stopper.join(3)
                manager.current.worker.join(3)
                self.assertTrue(stop_done.is_set())
                self.assertFalse(manager.current.serving.is_alive())
                self.assertEqual(servers[0].fileno(), -1)

    def test_sample_drain_waits_for_handler_and_retires_old_paths(self):
        job = Playback.__new__(Playback)
        job.lock, job.stopped = threading.RLock(), threading.Event()
        job.source_condition = threading.Condition(job.lock)
        job.source_active_connections = 1
        job.server = None
        job.deadline = time.monotonic() + 3
        job.nonce = "previous-generation"
        job.report = {"source_drains": deque(maxlen=8)}
        done = threading.Event()
        def drain():
            job.wait_sources_idle("sample")
            done.set()
        worker = threading.Thread(target=drain)
        worker.start()
        self.assertFalse(done.wait(.05))
        self.assertNotEqual(job.nonce, "previous-generation")
        with job.source_condition:
            job.source_active_connections = 0
            job.source_condition.notify_all()
        self.assertTrue(done.wait(1))
        worker.join()
        self.assertEqual(job.report["source_drains"][0]["initial_connections"], 1)

    def test_new_job_waits_for_callbacks_from_any_previous_job(self):
        manager = PlaybackManager(FakeClient(), "unused", "unused", "unused")
        manager.global_source_active_connections = 1
        job = SimpleNamespace(lock=threading.RLock(), stopped=threading.Event(),
                              deadline=time.monotonic() + 3, report={"source_drains": deque(maxlen=8)})
        done = threading.Event()
        worker = threading.Thread(target=lambda: (manager.wait_prior_sources(job), done.set()))
        worker.start()
        self.assertFalse(done.wait(.05))
        with manager.global_source_condition:
            manager.global_source_active_connections = 0
            manager.global_source_condition.notify_all()
        self.assertTrue(done.wait(1))
        worker.join()
        self.assertEqual(job.report["source_drains"][0]["reason"], "previous_jobs")
        self.assertEqual(job.report["source_drains"][0]["initial_connections"], 1)

    def test_sample_statistics_rejects_truncation_and_uses_actual_duration(self):
        packets = [{"pts_time": str(index / 20), "size": 1000} for index in range(60)]
        measured = sample_statistics(packets, 3, 20)
        self.assertAlmostEqual(measured["duration_seconds"], 3)
        self.assertAlmostEqual(measured["average_video_kbps"], 160)
        with self.assertRaises(ApiError) as caught:
            sample_statistics(packets[:10], 3, 20)
        self.assertEqual(caught.exception.code, "sample_incomplete")
        changed = [dict(packet) for packet in packets]
        changed[-1]["pts_time"] = "2.90"
        with self.assertRaises(ApiError):
            sample_statistics(changed, 3, 20)
        shifted = [{"pts_time": str(index / 20), "size": 1000} for index in range(59)]
        measured = sample_statistics(shifted, 3, 20)
        self.assertAlmostEqual(measured["duration_seconds"], 2.95)
        self.assertAlmostEqual(measured["average_video_kbps"], 59 * 1000 * 8 / 2.95 / 1000)

    def test_truncated_sample_cannot_publish_ready_or_quality(self):
        with tempfile.TemporaryDirectory() as directory:
            client = FakeClient()
            client.sources = lambda *_: {"video": [], "audio": []}
            manager = PlaybackManager(client, "unused", "unused", directory)
            truncated = json.dumps({"packets": [{"pts_time": str(index / 20), "size": 1000} for index in range(10)]}).encode()
            def run_small(_job, command, timeout=12):
                return truncated if command[0] == "ffprobe-test" else b""
            manager.ffprobe = "ffprobe-test"
            with patch.object(Playback, "run_small", run_small):
                manager.create({"bvid": "BV13x41117TL"})
                job = manager.current
                self.assertTrue(job.ready.wait(3))
                job.worker.join(3)
                self.assertEqual(job.status, "error")
                self.assertEqual(job.error["code"], "sample_incomplete")
                self.assertIsNone(job.quality)
                self.assertEqual(job.sample_attempts, [])
                self.assertFalse(job.serving.is_alive())

    def test_sample_stderr_error_rejected_even_with_exit_zero(self):
        job = Playback.__new__(Playback)
        job.lock, job.stopped, job.processes = threading.RLock(), threading.Event(), set()
        job.report = {"sample_errors": deque(maxlen=8)}
        with self.assertRaises(ApiError) as caught:
            job.run_small([sys.executable, "-c", "import sys; sys.stderr.write('Error during demuxing: partial file\\n')"])
        self.assertEqual(caught.exception.code, "sample_media_errors")
        self.assertIn("partial file", job.report["sample_errors"][0])
        self.assertEqual(job.processes, set())


if __name__ == "__main__":
    unittest.main(verbosity=2)
