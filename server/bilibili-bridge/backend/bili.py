"""Normal web API session. QR authorization always requires the user's phone.

Only fixed API endpoints and a bounded allowlisted QR handoff are fetched. No
browser profile, existing application cookie store, password or access token is
read. API changes/risk-control responses remain explicit failures.
"""
from __future__ import annotations

import hashlib
import html
from http.cookies import SimpleCookie
import json
import re
import secrets
import threading
import time
from urllib.parse import parse_qsl, quote, urlencode, urljoin, urlsplit

from common import ApiError, bvid, page, positive, private_read, private_write, require

API = "https://api.bilibili.com"
PASSPORT = "https://passport.bilibili.com"
REFERER = "https://www.bilibili.com/"
COOKIE_NAMES = {"SESSDATA", "bili_jct", "DedeUserID", "DedeUserID__ckMd5", "sid"}
HANDOFF_HOSTS = {"passport.bilibili.com", "passport.biligame.com", "www.bilibili.com"}
MIXIN = (46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
         27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13)


def clean(value, limit=160):
    text = html.unescape(re.sub(r"<[^>]*>", "", str(value or "")))
    return "".join(char for char in text if char >= " " or char == "\n")[:limit]


def duration(value):
    if isinstance(value, (int, float)):
        return max(0, int(value))
    if isinstance(value, str) and re.fullmatch(r"\d+(?::\d+){1,2}", value):
        result = 0
        for part in value.split(":"):
            result = result * 60 + int(part)
        return result
    return None


def video_row(raw):
    owner = raw.get("owner") or raw.get("upper") or {}
    cover = raw.get("pic") or raw.get("cover") or ""
    if isinstance(cover, str) and cover.startswith("//"):
        cover = "https:" + cover
    return {"bvid": raw.get("bvid") or "", "cid": raw.get("cid"),
            "title": clean(raw.get("title")), "author": clean(raw.get("author") or owner.get("name"), 64),
            "duration": duration(raw.get("duration")), "cover": cover if str(cover).startswith("https://") else ""}


def wbi_parameters(params, key, timestamp=None):
    require(len(key) >= 64, "wbi_key_invalid", "B站签名参数不可用", 502)
    mixin = "".join(key[index] for index in MIXIN)
    result = {str(k): "".join(char for char in str(v) if char not in "!'()*") for k, v in params.items()}
    result["wts"] = str(int(time.time() if timestamp is None else timestamp))
    query = urlencode(sorted(result.items()), quote_via=quote)
    result["w_rid"] = hashlib.md5((query + mixin).encode()).hexdigest()
    return result


def handoff_url(url):
    require(isinstance(url, str) and len(url) <= 8192, "qr_handoff_invalid", "扫码回调地址不正确", 502)
    parsed = urlsplit(url)
    require(parsed.scheme == "https" and parsed.hostname in HANDOFF_HOSTS and not parsed.username
            and not parsed.password and parsed.port in (None, 443),
            "qr_handoff_host", "扫码回调域名不受支持", 502)
    require((parsed.path.startswith(("/x/passport-login/", "/crossDomain")) or parsed.path in ("/", ""))
            and "\r" not in url and "\n" not in url,
            "qr_handoff_invalid", "扫码回调地址不正确", 502)
    return url


def qr_display_url(link):
    """Allow observed official web-scan origins; this URL is displayed only.

    Current passport generate responses also use account.bilibili.com's exact
    account-h5 scan route. Callback/query values remain opaque QR data and are
    never fetched by this bridge.
    """
    require(isinstance(link, str) and 0 < len(link) <= 4096
            and not any(ord(char) < 32 or ord(char) == 127 for char in link),
            "qr_url_invalid", "二维码地址不受支持", 502)
    try:
        parsed = urlsplit(link)
        port = parsed.port
    except ValueError:
        raise ApiError("qr_url_invalid", "二维码地址不受支持", 502) from None
    legacy = parsed.hostname in {"passport.bilibili.com", "www.bilibili.com"}
    current = (parsed.hostname == "account.bilibili.com"
               and parsed.path == "/h5/account-h5/auth/scan-web" and not parsed.fragment)
    require(parsed.scheme == "https" and (legacy or current) and parsed.username is None
            and parsed.password is None and port in (None, 443),
            "qr_url_invalid", "二维码地址不受支持", 502)
    return link


class BiliClient:
    def __init__(self, cookie_file, session=None):
        if session is None:
            from curl_cffi import requests
            session = requests.Session(impersonate="chrome", trust_env=False)
        self.session, self.cookie_file = session, cookie_file
        self.lock = threading.RLock()
        self.ready = False
        self.verified_logged_in = False
        self.qr = {}
        self.cache = {}
        self.cooldown = 0
        self.last_response_cookies = {}
        if cookie_file.exists():
            saved = json.loads(private_read(cookie_file))
            for key, value in saved.get("cookies", {}).items():
                self._set_cookie(key, value)

    def _set_cookie(self, key, value):
        if key in COOKIE_NAMES and isinstance(value, str) and len(value) <= 8192 and not re.search(r"[\x00-\x20\x7f]", value):
            self.session.cookies.set(key, value, domain=".bilibili.com", path="/")

    def cookie(self, key):
        return next((cookie.value for cookie in self.session.cookies.jar
                     if cookie.name == key and cookie.domain.lstrip(".") == "bilibili.com"), "")

    def _save(self):
        private_write(self.cookie_file, {"schema": 1, "cookies": {key: self.cookie(key) for key in COOKIE_NAMES if self.cookie(key)}})

    def _raw(self, method, url, **kwargs):
        require(time.monotonic() >= self.cooldown, "upstream_cooldown", "B站请求受限，请稍后重试", 429)
        try:
            response = self.session.request(method, url, timeout=12, allow_redirects=False,
                                            headers={"Referer": REFERER}, **kwargs)
        except Exception:
            raise ApiError("upstream_network", "连接B站失败，请稍后重试", 502) from None
        if response.status_code in (412, 429):
            self.cooldown = time.monotonic() + 60
        require(response.status_code == 200, "upstream_http_" + str(response.status_code),
                "B站暂时拒绝请求（HTTP %s）" % response.status_code, 502)
        require(len(response.content) <= 2_000_000, "upstream_too_large", "B站响应过大", 502)
        return response

    def request(self, path, params=None, form=None, passport=False, allow_not_logged=False):
        with self.lock:
            if not self.ready:
                self._raw("GET", REFERER)
                self.ready = True
                time.sleep(1)
            response = self._raw("POST" if form is not None else "GET", (PASSPORT if passport else API) + path,
                                 params=params, data=form)
            self.last_response_cookies = {}
            for header in response.headers.get_list("Set-Cookie"):
                parsed = SimpleCookie()
                try:
                    parsed.load(header)
                    self.last_response_cookies.update({key: morsel.value for key, morsel in parsed.items() if key in COOKIE_NAMES})
                except Exception:
                    pass
            try:
                body = response.json()
            except Exception:
                raise ApiError("upstream_json", "B站返回了无法识别的数据", 502) from None
            code = body.get("code")
            if code == -101:
                self.verified_logged_in = False
                if allow_not_logged:
                    return body.get("data") or {}
                raise ApiError("login_required", "请先扫码登录B站", 401)
            require(code == 0, "upstream_code_" + str(code), "B站接口返回错误（%s）" % code, 502)
            return body.get("data") or {}

    def cached(self, key, seconds, loader):
        with self.lock:
            entry = self.cache.get(key)
            if entry and entry[0] > time.monotonic():
                return entry[1]
            result = loader()
            self.cache[key] = (time.monotonic() + seconds, result)
            if len(self.cache) > 128:
                self.cache.pop(next(iter(self.cache)))
            return result

    def feed(self, number):
        number = page(number)
        def load():
            result = self.request("/x/web-interface/popular", {"pn": number, "ps": 10})
            rows = [video_row(row) for row in result.get("list", [])[:10] if row.get("bvid")]
            return {"items": rows, "page": number, "has_more": not result.get("no_more", len(rows) < 10)}
        return self.cached(("feed", number), 30, load)

    def search(self, query, number):
        number = page(number)
        require(isinstance(query, str) and 0 < len(query.strip()) <= 80, message="搜索词需要1至80个字")
        def load():
            nav = self.request("/x/web-interface/nav", allow_not_logged=True)
            wbi = nav.get("wbi_img") or {}
            key = "".join(urlsplit(wbi.get(name, "")).path.rsplit("/", 1)[-1].split(".")[0]
                          for name in ("img_url", "sub_url"))
            # Upstream search uses 20 results; split its page into two device pages.
            upstream_page, offset = (number + 1) // 2, 0 if number % 2 else 10
            params = wbi_parameters({"search_type": "video", "keyword": query, "page": upstream_page, "page_size": 20}, key)
            result = self.request("/x/web-interface/wbi/search/type", params)
            source = result.get("result") or []
            items = [video_row(row) for row in source[offset:offset + 10] if row.get("bvid")]
            total = int(result.get("numResults") or 0)
            return {"items": items, "page": number, "has_more": number * 10 < total}
        return self.cached(("search", query, number), 20, load)

    def detail_raw(self, identifier):
        identifier = bvid(identifier)
        return self.cached(("detail", identifier), 120,
                           lambda: self.request("/x/web-interface/view", {"bvid": identifier}))

    def detail(self, identifier):
        raw = self.detail_raw(identifier)
        return {**video_row(raw), "description": clean(raw.get("desc"), 512),
                "pages": [{"cid": row.get("cid"), "title": clean(row.get("part")),
                           "duration": duration(row.get("duration"))} for row in raw.get("pages", [])[:100]]}

    def me(self):
        nav = self.request("/x/web-interface/nav", allow_not_logged=True)
        self.verified_logged_in = bool(nav.get("isLogin"))
        if not self.verified_logged_in:
            return {"logged_in": False, "mid": None, "name": "", "avatar": "", "level": None, "vip_label": "", "coins": None}
        return {"logged_in": True, "mid": nav.get("mid"), "name": clean(nav.get("uname"), 64),
                "avatar": nav.get("face") or "", "level": (nav.get("level_info") or {}).get("current_level"),
                "vip_label": clean(((nav.get("vip") or {}).get("label") or {}).get("text"), 32),
                "coins": nav.get("money")}

    def require_account(self):
        result = self.cached(("me",), 10, self.me)
        require(result.get("logged_in"), "login_required", "请先扫码登录B站", 401)
        return result

    def folders_all(self):
        me = self.require_account()
        return self.cached(("folders", me["mid"]), 20, lambda: self.request(
            "/x/v3/fav/folder/created/list-all", {"up_mid": me["mid"], "type": 2}).get("list") or [])

    def folders(self, number):
        number = page(number)
        rows = self.folders_all()
        return {"items": [{"id": row.get("id"), "title": clean(row.get("title"), 64),
                           "count": row.get("media_count")} for row in rows[(number - 1) * 10:number * 10]],
                "page": number, "has_more": number * 10 < len(rows)}

    def own_folder(self, identifier):
        identifier = positive(identifier, "folder_id")
        require(any(int(row.get("id") or 0) == identifier for row in self.folders_all()),
                "folder_not_owned", "请选择当前账号的收藏夹", 403)
        return identifier

    def favorites(self, identifier, number):
        number, identifier = page(number), self.own_folder(identifier)
        upstream_page, offset = (number + 1) // 2, 0 if number % 2 else 10
        result = self.request("/x/v3/fav/resource/list", {"media_id": identifier, "pn": upstream_page,
                              "ps": 20, "order": "mtime", "type": 0, "tid": 0, "platform": "web"})
        rows = result.get("medias") or []
        items = [video_row(row) for row in rows[offset:offset + 10] if row.get("bvid")]
        return {"items": items, "page": number, "has_more": len(rows) > offset + 10 or bool(result.get("has_more"))}

    def favorite(self, identifier, folder, action):
        require(action in ("add", "remove"), message="收藏操作不正确")
        folder = self.own_folder(folder)
        raw = self.detail_raw(bvid(identifier))
        csrf = self.cookie("bili_jct")
        require(bool(csrf), "csrf_missing", "登录信息缺少CSRF，请重新扫码", 401)
        self.request("/x/v3/fav/resource/deal", form={"rid": positive(raw.get("aid"), "aid"), "type": 2,
                     "add_media_ids": str(folder) if action == "add" else "",
                     "del_media_ids": str(folder) if action == "remove" else "", "csrf": csrf})
        self.cache.clear()
        return {"bvid": identifier, "folder_id": folder, "action": action, "changed": True}

    def qr_new(self):
        with self.lock:
            result = self.request("/x/passport-login/web/qrcode/generate", {"source": "main-fe-header"}, passport=True)
            key, link = result.get("qrcode_key"), result.get("url")
            require(isinstance(key, str) and 0 < len(key) <= 256 and isinstance(link, str) and len(link) <= 4096,
                    "qr_response_invalid", "B站二维码数据不完整", 502)
            link = qr_display_url(link)
            identifier = secrets.token_urlsafe(18)
            self.qr.clear()  # One phone authorization flow at a time.
            self.qr[identifier] = {"key": key, "expires": time.monotonic() + 180, "last_poll": 0,
                                   "status": "waiting"}
            return {"login_id": identifier, "qr_url": link, "expires_in": 180}

    def _capture_handoff(self, link):
        link = handoff_url(link)
        values = dict(parse_qsl(urlsplit(link).query))
        for key in COOKIE_NAMES:
            if values.get(key):
                self._set_cookie(key, values[key])
        if values.get("SESSDATA"):
            return
        # Some current QR responses issue cookies on a biligame crossDomain host;
        # explicitly collect only normal Bilibili login cookie names from it.
        for _ in range(4):
            link = handoff_url(link)
            try:
                response = self.session.get(link, timeout=12, allow_redirects=False,
                                            headers={"Referer": PASSPORT + "/"})
            except Exception:
                raise ApiError("qr_handoff_network", "扫码确认后获取登录信息失败，请重新扫码", 502) from None
            require(len(response.content) <= 1_000_000, "qr_handoff_size", "扫码回调响应过大", 502)
            for header in response.headers.get_list("Set-Cookie"):
                parsed_cookie = SimpleCookie()
                try:
                    parsed_cookie.load(header)
                    for key, morsel in parsed_cookie.items():
                        self._set_cookie(key, morsel.value)
                except Exception:
                    pass
            if response.status_code in (301, 302, 303, 307, 308):
                link = urljoin(link, response.headers.get("Location", ""))
                continue
            require(response.status_code == 200, "qr_handoff_http", "扫码回调失败，请重新扫码", 502)
            return
        raise ApiError("qr_handoff_redirects", "扫码回调重定向过多", 502)

    def qr_poll(self, identifier):
        with self.lock:
            row = self.qr.get(identifier)
            require(row is not None, "qr_not_found", "二维码已失效，请重新生成", 404)
            if row["status"] == "confirmed":
                return {"status": "confirmed"}
            if time.monotonic() >= row["expires"]:
                row["status"] = "expired"
            if row["status"] == "expired" or time.monotonic() - row["last_poll"] < 2:
                return {"status": row["status"]}
            row["last_poll"] = time.monotonic()
            result = self.request("/x/passport-login/web/qrcode/poll", {"qrcode_key": row["key"],
                                  "source": "main-fe-header"}, passport=True)
            code = result.get("code")
            if code in (86101, 86090, 86038):
                row["status"] = {86101: "waiting", 86090: "scanned", 86038: "expired"}[code]
            elif code == 0:
                direct_cookies = dict(self.last_response_cookies)
                # Never mistake a previous account's still-valid cookies for
                # credentials returned by this new phone authorization.
                for cookie in list(self.session.cookies.jar):
                    if cookie.name in COOKIE_NAMES:
                        self.session.cookies.jar.clear(cookie.domain, cookie.path, cookie.name)
                for name, value in direct_cookies.items():
                    self._set_cookie(name, value)
                if result.get("url"):
                    self._capture_handoff(result["url"])
                require(all(self.cookie(name) for name in ("SESSDATA", "bili_jct", "DedeUserID")),
                        "qr_cookie_missing", "扫码返回的登录信息不完整，请重新扫码", 502)
                require(self.me().get("logged_in"), "qr_verify_failed", "扫码后登录校验失败，请重新扫码", 502)
                self._save()
                self.cache.clear()
                row["status"], row["key"] = "confirmed", ""
            else:
                raise ApiError("qr_status_" + str(code), "B站扫码状态异常，请重新扫码", 502)
            return {"status": row["status"]}

    def logout(self):
        with self.lock:
            revoked = False
            try:
                csrf = self.cookie("bili_jct")
                if csrf and self.cookie("SESSDATA"):
                    self.request("/login/exit/v2", form={"biliCSRF": csrf}, passport=True)
                    revoked = True
            except ApiError:
                pass
            finally:
                self.session.cookies.clear()
                self.verified_logged_in = self.ready = False
                self.qr.clear()
                self.cache.clear()
                if self.cookie_file.exists():
                    self.cookie_file.unlink()
            return {"logged_in": False, "remote_session_revoked": revoked}

    def sources(self, identifier, cid):
        result = self.request("/x/player/playurl", {"bvid": bvid(identifier), "cid": positive(cid, "cid"),
                              "qn": 16, "fnval": 16, "fourk": 0})
        dash = result.get("dash") or {}
        sources = {}
        for kind, prefix in (("video", "avc1"), ("audio", "mp4a")):
            rows = [row for row in dash.get(kind, []) if str(row.get("codecs", "")).startswith(prefix)]
            require(bool(rows), "source_codec_unavailable", "该视频暂无可用的H264/AAC音轨", 422)
            row = min(rows, key=lambda item: int(item.get("bandwidth") or 2**60))
            values = [row.get("baseUrl") or row.get("base_url")]
            backups = row.get("backupUrl") or row.get("backup_url") or []
            if isinstance(backups, (list, tuple)):
                values.extend(backups[:2])
            from live_relay_probe import media_url
            accepted = []
            for value in values:
                if not value:
                    continue
                try:
                    candidate = media_url(value)
                except ValueError:
                    continue
                if candidate not in accepted:
                    accepted.append(candidate)
            require(bool(accepted), "source_host_rejected", "视频源域名不受支持", 502)
            sources[kind] = accepted
        return sources
