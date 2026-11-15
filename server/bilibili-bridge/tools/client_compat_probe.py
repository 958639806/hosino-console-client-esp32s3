#!/usr/bin/env python3
"""Bounded anonymous HTTP-client comparison; no login, proxy, or media fetch."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import time
import urllib.error
import urllib.request


URL = 'https://api.bilibili.com/x/web-interface/view?bvid=BV13x41117TL'
HEADERS = {'User-Agent': 'Mozilla/5.0', 'Referer': 'https://www.bilibili.com/'}


def describe(status, body):
    result = {'http_status': status, 'body_bytes': len(body)}
    try:
        value = json.loads(body)
        result.update(bilibili_code=value.get('code'), message=str(value.get('message', ''))[:160])
        data = value.get('data') or {}
        if isinstance(data, dict):
            result['video_id'] = data.get('bvid')
            result['has_video_metadata'] = bool(data.get('bvid') and data.get('title'))
    except (ValueError, TypeError, AttributeError):
        result['json_response'] = False
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit('Refusing to overwrite an existing report')
    import curl_cffi
    from curl_cffi import requests
    rows = []
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(urllib.request.Request(URL, headers=HEADERS), timeout=10) as response:
            row = describe(response.status, response.read(500000))
    except urllib.error.HTTPError as exc:
        row = describe(exc.code, exc.read(500000))
    except Exception as exc:
        row = {'error_type': type(exc).__name__, 'error': str(exc)[:160]}
    rows.append({'case': 'urllib_anonymous', **row})
    time.sleep(2)
    # Only a fresh anonymous session created by this process. Existing browser
    # cookies and authenticated profiles are neither opened nor imported.
    with requests.Session(impersonate='chrome', trust_env=False) as session:
        for name, url in [('curl_chrome_anonymous', URL),
                          ('curl_chrome_homepage', 'https://www.bilibili.com/'),
                          ('curl_chrome_after_homepage', URL)]:
            try:
                response = session.get(url, headers={'Referer': 'https://www.bilibili.com/'}, timeout=10)
                row = describe(response.status_code, response.content[:500000])
                row['http_version'] = int(response.http_version)
                row['anonymous_cookie_names'] = sorted({cookie.name for cookie in session.cookies.jar})
            except Exception as exc:
                row = {'error_type': type(exc).__name__, 'error': str(exc)[:160]}
            rows.append({'case': name, **row})
            print(name, row.get('http_status'), row.get('bilibili_code'), flush=True)
            time.sleep(2)
    result = {
        'created_at_utc': datetime.now(timezone.utc).isoformat(),
        'scope': 'Four anonymous public-page/API requests, no retries, login, cookie import, media download, proxy or service.',
        'python': platform.python_version(), 'curl_cffi_version': curl_cffi.__version__,
        'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'endpoint': URL, 'results': rows,
        'any_video_metadata_obtained': any(row.get('has_video_metadata') for row in rows),
        'limitations': 'A successful metadata response would not validate search, media URL extraction, account access, or ESP32 playback.',
    }
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', 'utf-8')
    print('CLIENT_COMPARISON_DONE', result['any_video_metadata_obtained'], flush=True)


if __name__ == '__main__':
    main()
