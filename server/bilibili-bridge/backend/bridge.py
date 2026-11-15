#!/usr/bin/env python3
"""Isolated Bilibili bridge, API-CONTRACT v1. Default: loopback only.

Run with a private bearer-token file and a private state directory. It never
changes services, firewall rules, existing applications or browser profiles.
"""
from __future__ import annotations

import argparse
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import select
import signal
import socket
import sys
import threading
from urllib.parse import parse_qs, urlsplit

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

from bili import BiliClient
from common import ApiError, page, private_directory, private_read, require
from playback import PlaybackManager

VERSION = "0.1.0"
ACCOUNT_ROUTES = {"/v1/auth/qr", "/v1/auth/logout", "/v1/me", "/v1/folders", "/v1/favorites", "/v1/favorite"}


class BridgeServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False

    def __init__(self, address, handler, application):
        self.application = application
        self.handler_slots = threading.BoundedSemaphore(8)
        super().__init__(address, handler)

    def process_request(self, request, client_address):
        if not self.handler_slots.acquire(blocking=False):
            request.close()
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self.handler_slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.handler_slots.release()

    def handle_error(self, *_):
        # Tracebacks can contain private response objects and request headers.
        pass


class Application:
    def __init__(self, token, state_dir, ffmpeg, ffprobe, trusted_tls_proxy=False, client=None):
        require(isinstance(token, str) and 24 <= len(token) <= 192 and token.isascii()
                and all(32 < ord(char) < 127 for char in token), "token_invalid", "设备令牌需要24至192个非空ASCII字符", 500)
        self.token, self.trusted_tls_proxy = token, trusted_tls_proxy
        self.state_dir = private_directory(state_dir)
        self.client = client or BiliClient(self.state_dir / "cookies.json")
        work = private_directory(self.state_dir / "work")
        self.playbacks = PlaybackManager(self.client, ffmpeg, ffprobe, work)

    def dispatch(self, method, path, query, body):
        client = self.client
        if method == "GET" and path == "/v1/health":
            return {"version": VERSION, "source_session_ready": client.ready,
                    "account_logged_in": client.verified_logged_in}
        if method == "GET" and path == "/v1/feed":
            return client.feed(page(query.get("page", "1")))
        if method == "GET" and path == "/v1/search":
            return client.search(query.get("q", ""), page(query.get("page", "1")))
        if method == "GET" and path == "/v1/video":
            return client.detail(query.get("bvid"))
        if method == "POST" and path == "/v1/auth/qr":
            require(not body, message="二维码请求不需要额外参数")
            return client.qr_new()
        if method == "GET" and path == "/v1/auth/qr":
            return client.qr_poll(query.get("login_id", ""))
        if method == "POST" and path == "/v1/auth/logout":
            require(not body, message="退出请求不需要额外参数")
            self.playbacks.stop()
            return client.logout()
        if method == "GET" and path == "/v1/me":
            return client.me()
        if method == "GET" and path == "/v1/folders":
            return client.folders(page(query.get("page", "1")))
        if method == "GET" and path == "/v1/favorites":
            return client.favorites(query.get("folder_id"), page(query.get("page", "1")))
        if method == "POST" and path == "/v1/favorite":
            require(set(body) == {"bvid", "folder_id", "action"}, message="收藏请求字段不正确")
            return client.favorite(body["bvid"], body["folder_id"], body["action"])
        if path == "/v1/playbacks":
            if method == "POST":
                return self.playbacks.create(body)
            if method == "GET":
                return self.playbacks.get(query.get("session_id", "")).public()
            if method == "DELETE":
                return self.playbacks.stop(query.get("session_id", ""))
        raise ApiError("not_found", "接口不存在", 404)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(15)
        self.stream_started = False

    def log_message(self, *_):
        pass

    def peer_gone(self):
        try:
            ready, _, _ = select.select([self.connection], [], [], 0)
            return bool(ready and self.connection.recv(1, socket.MSG_PEEK) == b"")
        except (OSError, ValueError):
            return True

    def json_reply(self, status, value):
        encoded = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(encoded)
        self.close_connection = True

    def run_request(self):
        try:
            require(len(self.path) <= 4096, "uri_too_long", "请求地址过长", 414)
            parsed = urlsplit(self.path)
            require(not parsed.scheme and not parsed.netloc and not parsed.fragment, message="只接受相对接口路径")
            path = parsed.path
            app = self.server.application
            if path != "/v1/health":
                authorization = self.headers.get("Authorization", "")
                require(authorization.isascii() and authorization.startswith("Bearer ") and hmac.compare_digest(authorization[7:], app.token),
                        "unauthorized", "设备令牌无效", 401)
            query_values = parse_qs(parsed.query, keep_blank_values=True, max_num_fields=12)
            require(all(len(values) == 1 for values in query_values.values()), message="不接受重复查询参数")
            query = {key: values[0] for key, values in query_values.items()}
            # No device secrets in URLs, even if the bearer header is also valid.
            require(not any(key.lower() in {"token", "authorization", "cookie", "url"} for key in query), message="查询参数不受支持")
            if path in ACCOUNT_ROUTES:
                require(app.trusted_tls_proxy and self.headers.get("X-Forwarded-Proto", "").lower() == "https",
                        "https_required", "账户功能需要通过HTTPS安全连接", 403)
            body = {}
            if self.command == "POST":
                require(not self.headers.get("Transfer-Encoding"), message="JSON请求不接受分块编码")
                raw_length = self.headers.get("Content-Length", "0")
                require(raw_length.isdigit() and int(raw_length) <= 16384, "body_too_large", "请求正文过大", 413)
                require(self.headers.get("Content-Type", "application/json").split(";", 1)[0] == "application/json",
                        "content_type", "请求需要JSON格式", 415)
                raw = self.rfile.read(int(raw_length))
                require(len(raw) == int(raw_length), message="请求正文不完整")
                try:
                    body = json.loads(raw) if raw else {}
                except (UnicodeDecodeError, json.JSONDecodeError):
                    raise ApiError("invalid_json", "JSON格式不正确") from None
                require(isinstance(body, dict), message="请求需要JSON对象")
            if self.command == "GET" and path.startswith("/v1/streams/"):
                require(not query and not self.headers.get("Range"), message="生成中的视频不支持字节跳转")
                app.playbacks.get(path.removeprefix("/v1/streams/")).stream(self)
                return
            result = app.dispatch(self.command, path, query, body)
            self.json_reply(200, {"ok": True, "data": result})
        except ApiError as exc:
            if not self.stream_started:
                self.json_reply(exc.status, {"ok": False, "error": {"code": exc.code, "message": exc.message}})
        except (OSError, ValueError):
            if not self.stream_started:
                try:
                    self.json_reply(400, {"ok": False, "error": {"code": "invalid_request", "message": "请求格式不正确"}})
                except OSError:
                    pass
        except Exception:
            if not self.stream_started:
                self.json_reply(500, {"ok": False, "error": {"code": "internal_error", "message": "服务处理失败，请稍后重试"}})
        finally:
            self.close_connection = True

    do_GET = run_request
    do_POST = run_request
    do_DELETE = run_request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", choices=("127.0.0.1", "::1"))
    parser.add_argument("--port", type=int, default=18791)
    parser.add_argument("--token-file", type=Path)
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--ffprobe", required=True)
    parser.add_argument("--trusted-tls-proxy", action="store_true",
                        help="Enable account routes only when a trusted loopback proxy sends X-Forwarded-Proto:https")
    args = parser.parse_args()
    try:
        token = private_read(args.token_file).strip() if args.token_file else os.environ.get("BILI_DEVICE_TOKEN", "")
        app = Application(token, args.state_dir, args.ffmpeg, args.ffprobe, args.trusted_tls_proxy)
        if args.host == "::1":
            BridgeServer.address_family = socket.AF_INET6
        server = BridgeServer((args.host, args.port), Handler, app)
    except Exception:
        print("桥接服务初始化失败；请检查私有文件权限、令牌、依赖和端口。", file=sys.stderr)
        return 1
    if os.name == "posix":
        os.nice(10)

    def shutdown(*_):
        app.playbacks.stop()
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)
    print("Bilibili bridge %s listening on loopback port %d" % (VERSION, server.server_address[1]), flush=True)
    try:
        server.serve_forever()
    finally:
        app.playbacks.stop()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
