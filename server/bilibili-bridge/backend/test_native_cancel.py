#!/usr/bin/env python3
"""Finite native curl_cffi cancellation-bound test; loopback only, no account.

Requires the pinned Linux curl_cffi environment. The allowlist is overridden
only inside this test process to point the production CDN handler at a local
HTTP source that sends one byte then deliberately stalls. No public listener.
"""
from __future__ import annotations

from collections import deque
from http.server import BaseHTTPRequestHandler
import http.client
import json
from pathlib import Path
import sys
import threading
import time
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from live_relay_probe import RelayServer
from playback import CDNHandler


class Stalled(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        self.send_response(206)
        self.send_header("Content-Length", str(512 * 1024))
        self.send_header("Content-Range", "bytes 0-524287/1048576")
        self.end_headers()
        self.wfile.write(b"x")
        self.wfile.flush()
        self.server.release.wait(12)


def main():
    from curl_cffi import __version__
    if __version__ != "0.17.0":
        raise RuntimeError("test_requires_pinned_curl_cffi_0_17_0")
    source = RelayServer(("127.0.0.1", 0), Stalled)
    source.release = threading.Event()
    relay = RelayServer(("127.0.0.1", 0), CDNHandler)
    job = SimpleNamespace(nonce="native-bound", bvid="BV13x41117TL", stopped=threading.Event(),
        sources={"video": ["http://127.0.0.1:%d/source" % source.server_address[1]]},
        relay_slots=threading.BoundedSemaphore(4), lock=threading.Lock(),
        source_bytes=0, deadline=time.monotonic() + 30, report={"cdn_requests": deque(maxlen=64)})
    relay.job = job
    threads = [threading.Thread(target=server.serve_forever, daemon=True) for server in (source, relay)]
    for thread in threads:
        thread.start()
    connection = http.client.HTTPConnection("127.0.0.1", relay.server_address[1], timeout=10)
    result = {"scope": "Real native Curl + production CDN handler + deliberately stalled loopback source"}
    try:
        # Explicitly scoped test-only target; production host validation remains
        # unchanged, and there are no external requests in this process.
        with patch("playback.media_url", lambda value: value):
            connection.request("GET", "/native-bound/video")
            response = connection.getresponse()
            assert response.status == 206 and response.read(1) == b"x"
            # One of the four global slots is occupied by this source.
            acquired = [job.relay_slots.acquire(blocking=False) for _ in range(3)]
            assert all(acquired) and not job.relay_slots.acquire(blocking=False)
            started = time.monotonic()
            job.stopped.set()
            released = job.relay_slots.acquire(timeout=9)
            elapsed = time.monotonic() - started
            assert released and elapsed <= 8.5, (released, elapsed)
            job.relay_slots.release()
            for _ in range(3):
                job.relay_slots.release()
            result.update({"passed": True, "stop_to_slot_release_seconds": elapsed,
                           "bound_seconds": 8.5, "source_received_bytes": job.source_bytes})
    finally:
        job.stopped.set()
        source.release.set()
        connection.close()
        for server in (relay, source):
            server.shutdown()
            server.server_close()
        for thread in threads:
            thread.join(timeout=2)
    print(json.dumps(result))


if __name__ == "__main__":
    main()
