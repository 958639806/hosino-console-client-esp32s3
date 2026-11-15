#!/usr/bin/env python3
"""Anonymous, bounded Bilibili metadata probes. No cookies or media downloads."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
import zipfile


def redact(text):
    # Signed CDN queries and any accidental credential-looking assignments do
    # not belong in a shareable diagnostics file.
    text = re.sub(r"(https?://[^\s?]+)\?[^\s]+", r"\1?[redacted]", text)
    return re.sub(r"(?i)(sessdata|cookie|password|token)(\s*[:=]\s*)\S+", r"\1\2[redacted]", text)[:2500]


def metadata(value):
    keys = ('id', 'title', 'duration', 'uploader', 'extractor_key', 'playlist_count')
    result = {key: value.get(key) for key in keys if key in value}
    result['format_count'] = len(value.get('formats') or [])
    result['formats'] = [{key: row.get(key) for key in
        ('format_id', 'ext', 'vcodec', 'acodec', 'width', 'height', 'fps', 'tbr', 'filesize')}
        for row in (value.get('formats') or [])]
    if 'entries' in value:
        result['entries'] = [{key: row.get(key) for key in ('id', 'title', 'duration')}
                             for row in (value.get('entries') or []) if row][:3]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wheel', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    with zipfile.ZipFile(args.wheel) as archive:
        source = archive.read('yt_dlp/extractor/bilibili.py').decode('utf-8')
    # Use the upstream extractor's first video fixture; this is only a public
    # metadata probe, not a claim of authority to download arbitrary media.
    target = re.search(r"'url': '(https://www\.bilibili\.com/video/[^']+)'", source)
    if not target:
        raise SystemExit('No upstream Bilibili video fixture found')
    url = target.group(1)
    runner = "import sys,runpy;sys.path.insert(0,sys.argv.pop(1));runpy.run_module('yt_dlp',run_name='__main__')"
    base = [sys.executable, '-c', runner, str(args.wheel.resolve()), '--ignore-config',
            '--no-cache-dir', '--no-warnings', '--skip-download', '--dump-single-json',
            '--socket-timeout', '8', '--retries', '0', '--extractor-retries', '0']
    rows = []
    for name, tail in [('video_metadata', ['--no-playlist', url]),
                       ('search_metadata', ['--flat-playlist', 'bilisearch3:ESP32'])]:
        try:
            run = subprocess.run(base + tail, capture_output=True, text=True, timeout=55)
            row = {'name': name, 'exit_code': run.returncode, 'stderr': redact(run.stderr)}
            if run.returncode == 0:
                row['metadata'] = metadata(json.loads(run.stdout))
        except subprocess.TimeoutExpired:
            row = {'name': name, 'timed_out': True}
        except (ValueError, OSError) as exc:
            row = {'name': name, 'error': redact(str(exc))}
        rows.append(row)
        print(name, 'OK' if row.get('exit_code') == 0 else 'FAILED', flush=True)
    result = {
        'created_at_utc': datetime.now(timezone.utc).isoformat(),
        'scope': 'Anonymous metadata/search only; no login, cookies, source media download, service, or device test.',
        'python': platform.python_version(),
        'wheel_name': args.wheel.name,
        'wheel_sha256': hashlib.sha256(args.wheel.read_bytes()).hexdigest(),
        'fixture_url': url,
        'results': rows,
        'all_passed': all(row.get('exit_code') == 0 for row in rows),
    }
    (args.output_dir / 'bilibili-probe.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', 'utf-8')
    print('BILIBILI_PROBE_DONE', json.dumps({'all_passed': result['all_passed']}), flush=True)


if __name__ == '__main__':
    main()
