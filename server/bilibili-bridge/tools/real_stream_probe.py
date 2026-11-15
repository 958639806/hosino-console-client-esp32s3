#!/usr/bin/env python3
"""Fetch real public DASH sources and verify MJPEG/PCM conversion.

This first-stage baseline downloads bounded complete source files before
transcoding. It does not prove concurrent Internet streaming or ESP32 playback.
Only a fresh anonymous session is used; cookies and signed URLs stay in memory.
"""
from __future__ import annotations

import argparse
import array
from datetime import datetime, timezone
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import wave
from urllib.parse import urljoin, urlsplit


MEDIA_SUFFIXES = ('bilivideo.com', 'bilivideo.cn')


def dump(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', 'utf-8')


def safe_error(exc):
    # Curl exceptions can contain the complete signed URL. Keep only its class.
    return type(exc).__name__


def media_url(url):
    if not isinstance(url, str) or len(url) > 8192:
        raise ValueError('invalid_media_location')
    p = urlsplit(url)
    host = (p.hostname or '').lower()
    if (p.scheme != 'https' or p.username or p.password or p.port not in (None, 443)
            or not any(host == suffix or host.endswith('.' + suffix) for suffix in MEDIA_SUFFIXES)):
        raise ValueError('unsupported_media_location')
    return url


def _json_get(client, url, params=None, referer='https://www.bilibili.com/'):
    response = client.get(url, params=params, headers={'Referer': referer}, timeout=12,
                          allow_redirects=False)
    if response.status_code != 200:
        raise RuntimeError('api_http_' + str(response.status_code))
    if len(response.content) > 2_000_000:
        raise ValueError('api_response_too_large')
    value = response.json()
    if value.get('code') != 0:
        raise RuntimeError('api_business_' + str(value.get('code')))
    return value.get('data') or {}


def resolve_sources(client, bvid):
    """Return safe metadata and RAM-only video/audio candidate URL lists.

    Caller creates a fresh curl_cffi Session(impersonate='chrome', trust_env=False).
    No existing account/browser cookie jar is loaded or saved.
    """
    if not re.fullmatch(r'BV[0-9A-Za-z]{10}', bvid):
        raise ValueError('invalid_bvid')
    home = client.get('https://www.bilibili.com/', timeout=12, allow_redirects=False)
    if home.status_code != 200:
        raise RuntimeError('homepage_http_' + str(home.status_code))
    time.sleep(1)
    info = _json_get(client, 'https://api.bilibili.com/x/web-interface/view', {'bvid': bvid})
    if not isinstance(info.get('cid'), int) or info['cid'] <= 0:
        raise ValueError('invalid_video_info')
    time.sleep(1)
    play = _json_get(client, 'https://api.bilibili.com/x/player/playurl',
                     {'bvid': bvid, 'cid': info['cid'], 'qn': 16, 'fnval': 16, 'fourk': 0},
                     'https://www.bilibili.com/video/' + bvid + '/')
    dash = play.get('dash') or {}
    videos = [x for x in (dash.get('video') or [])
              if isinstance(x, dict) and str(x.get('codecs', '')).startswith('avc1')]
    audios = [x for x in (dash.get('audio') or [])
              if isinstance(x, dict) and str(x.get('codecs', '')).startswith('mp4a')]
    if not videos or not audios:
        raise ValueError('missing_h264_or_aac_source')
    video = min(videos, key=lambda x: (int(x.get('bandwidth') or 2**62), int(x.get('id') or 0)))
    audio = min(audios, key=lambda x: int(x.get('bandwidth') or 2**62))

    def candidates(row):
        values = [row.get('baseUrl') or row.get('base_url')]
        values += (row.get('backupUrl') or row.get('backup_url') or [])[:2]
        result = []
        for value in values:
            if value:
                value = media_url(value)
                if value not in result:
                    result.append(value)
        if not result:
            raise ValueError('empty_media_location')
        return result

    metadata = {'bvid': bvid, 'title': str(info.get('title', ''))[:512],
                'owner': str((info.get('owner') or {}).get('name', ''))[:128],
                'source_duration_seconds': info.get('duration'), 'cid': info['cid'],
                'video': {k: video.get(k) for k in ('id', 'codecs', 'width', 'height', 'bandwidth')},
                'audio': {k: audio.get(k) for k in ('id', 'codecs', 'bandwidth')},
                'video_url_nonempty': True, 'audio_url_nonempty': True}
    return metadata, {'video': candidates(video), 'audio': candidates(audio)}


def fetch_source(client, urls, destination, referer, byte_limit, aggregate, deadline_seconds=60):
    """Bounded, identity-encoded complete-file transfer; no redirect escape."""
    attempts = []
    for initial in urls[:3]:
        url, response, size = initial, None, 0
        part = destination.with_suffix(destination.suffix + '.part')
        started = time.monotonic()
        try:
            for redirects in range(4):
                media_url(url)
                response = client.get(url, stream=True, allow_redirects=False, timeout=(10, 60),
                                      headers={'Referer': referer, 'Accept-Encoding': 'identity'})
                if response.status_code in (301, 302, 303, 307, 308):
                    location = response.headers.get('Location')
                    response.close(); response = None
                    if not location or redirects == 3:
                        raise ValueError('redirect_limit')
                    url = media_url(urljoin(url, location))
                    continue
                break
            if response is None or response.status_code != 200:
                raise RuntimeError('cdn_http_' + str(response.status_code if response else 'none'))
            encoding = response.headers.get('Content-Encoding', 'identity').lower()
            if encoding not in ('', 'identity'):
                raise ValueError('unexpected_content_encoding')
            length = response.headers.get('Content-Length')
            if length and int(length) > byte_limit:
                raise ValueError('source_exceeds_byte_limit')
            digest = hashlib.sha256()
            with part.open('xb') as output:
                for chunk in response.iter_content(chunk_size=64 * 1024):
                    if not chunk:
                        continue
                    if (time.monotonic() - started > deadline_seconds or
                            size + len(chunk) > byte_limit or
                            aggregate['bytes'] + len(chunk) > aggregate['limit']):
                        raise ValueError('source_transfer_limit')
                    output.write(chunk); digest.update(chunk); size += len(chunk)
                    aggregate['bytes'] += len(chunk)
            if not size or (length and size != int(length)):
                raise ValueError('incomplete_source')
            part.replace(destination)
            elapsed = time.monotonic() - started
            return {'passed': True, 'file': destination.name, 'bytes': size,
                    'sha256': digest.hexdigest(), 'wall_seconds': elapsed,
                    'average_mbps': size * 8 / elapsed / 1_000_000,
                    'attempt_count': len(attempts) + 1, 'earlier_attempts': attempts,
                    'http_status': 200, 'scope': 'Bounded complete-source transfer, before conversion'}
        except Exception as exc:
            attempts.append({'error_type': safe_error(exc), 'received_bytes': size})
            if part.exists():
                part.unlink()
        finally:
            if response is not None:
                response.close()
    raise RuntimeError('all_source_candidates_failed_' + json.dumps(attempts))


def execute(argv, timeout=60, binary=False):
    def low_priority():
        os.nice(10)
    started = time.monotonic()
    run = subprocess.run(argv, capture_output=True, timeout=timeout,
                         preexec_fn=low_priority if os.name == 'posix' else None)
    if run.returncode:
        # argv contains only local files, never upstream signed media URLs.
        raise RuntimeError('media_command_exit_%d: %s' %
                           (run.returncode, run.stderr.decode('utf-8', 'replace')[-1600:]))
    return run.stdout if binary else {'stdout': run.stdout.decode('utf-8', 'replace'),
                                     'wall_seconds': time.monotonic() - started}


def probe_json(ffprobe, path, packets=False):
    command = [ffprobe, '-v', 'error', '-show_streams', '-show_format']
    if packets:
        command += ['-show_packets', '-show_data_hash', 'sha256', '-show_entries',
                    'packet=stream_index,size,pts_time,duration_time,data_hash:stream=index,codec_type,codec_name,width,height,sample_rate,channels:format=duration,size']
    command += ['-of', 'json', str(path)]
    return json.loads(execute(command)['stdout'])


def verify_output(ffmpeg, ffprobe, path, resolution, audio, seconds):
    raw = probe_json(ffprobe, path, packets=True)
    rows = []
    for stream in raw.get('streams', []):
        packets = [x for x in raw.get('packets', []) if x['stream_index'] == stream['index']]
        pts = [float(x['pts_time']) for x in packets if x.get('pts_time') not in (None, 'N/A')]
        sizes = sum(int(x.get('size', 0)) for x in packets)
        rows.append({**{k: stream.get(k) for k in ('index', 'codec_type', 'codec_name', 'width', 'height', 'sample_rate', 'channels')},
                     'packet_count': len(packets), 'payload_bytes': sizes,
                     'payload_kbps': sizes * 8 / seconds / 1000,
                     'first_pts': pts[0] if pts else None, 'last_pts': pts[-1] if pts else None,
                     'pts_monotonic': bool(pts) and all(a <= b for a, b in zip(pts, pts[1:]))})
    video = next((x for x in rows if x['codec_type'] == 'video'), {})
    sound = next((x for x in rows if x['codec_type'] == 'audio'), {})
    wav = execute([ffmpeg, '-hide_banner', '-nostdin', '-v', 'error', '-i', str(path),
                   '-t', '3', '-map', '0:a:0', '-c:a', 'pcm_s16le', '-ar', '22050', '-ac', '1',
                   '-f', 'wav', 'pipe:1'], binary=True)
    # WAV avoids reliance on the raw PCM muxer.
    with wave.open(io.BytesIO(wav), 'rb') as sound_file:
        pcm = sound_file.readframes(22050 * 3 + 1024)
    samples = array.array('h'); samples.frombytes(pcm)
    if sys.byteorder != 'little':
        samples.byteswap()
    rms = math.sqrt(sum(float(x) * x for x in samples) / len(samples)) if samples else 0.0
    packets = [x for x in raw.get('packets', []) if x['stream_index'] == video.get('index')]
    stride = resolution['target_fps']
    packet_hashes = [x['data_hash'] for x in packets[::stride][:5] if x.get('data_hash')]
    pixels = execute([ffmpeg, '-hide_banner', '-nostdin', '-v', 'error', '-threads', '2',
                      '-i', str(path), '-map', '0:v:0', '-vf', 'fps=1', '-frames:v', '5', '-c:v', 'rawvideo',
                      '-threads:v', '2', '-pix_fmt', 'yuv420p', '-an', '-f', 'rawvideo', 'pipe:1'], binary=True)
    frame_bytes = resolution['width'] * resolution['height'] * 3 // 2
    hashes = [hashlib.sha256(pixels[i:i + frame_bytes]).hexdigest()
              for i in range(0, len(pixels), frame_bytes) if len(pixels[i:i + frame_bytes]) == frame_bytes]
    decoded = execute([ffmpeg, '-hide_banner', '-nostdin', '-v', 'error', '-filter_threads', '2',
                       '-threads', '2', '-i', str(path), '-map', '0:v:0', '-map', '0:a:0',
                       '-c:v', 'rawvideo', '-threads:v', '2', '-c:a', 'pcm_s16le',
                       '-progress', 'pipe:1', '-nostats', '-f', 'null', '-'])
    progress = dict(line.split('=', 1) for line in decoded['stdout'].splitlines() if '=' in line)
    decoded_frames = int(progress.get('frame', '-10000'))
    decoded_seconds = int(progress.get('out_time_us', '-100000000')) / 1_000_000
    audio_seconds = sound.get('payload_bytes', 0) / (audio['sample_rate_hz'] * audio['channels'] * 2)
    video_end = (video.get('last_pts') or 0) + 1 / resolution['target_fps']
    audio_end = (sound.get('first_pts') or 0) + audio_seconds
    checks = {
        'video_mjpeg': video.get('codec_name') == 'mjpeg',
        'audio_pcm': sound.get('codec_name') == 'pcm_s16le',
        'dimensions': (video.get('width'), video.get('height')) == (resolution['width'], resolution['height']),
        'frame_count': abs(video.get('packet_count', -10000) - round(seconds * resolution['target_fps'])) <= 1,
        'audio_parameters': (sound.get('sample_rate'), sound.get('channels')) == (str(audio['sample_rate_hz']), audio['channels']),
        'audio_duration': abs(audio_seconds - seconds) <= 0.15,
        'video_duration': abs(video_end - seconds) <= 0.15,
        'av_first_pts_close': video.get('first_pts') is not None and sound.get('first_pts') is not None and abs(video['first_pts'] - sound['first_pts']) <= 0.15,
        'av_end_pts_close': abs(video_end - audio_end) <= 0.15,
        'pts_monotonic': len(rows) == 2 and all(x['pts_monotonic'] for x in rows),
        'non_silent_first_three_seconds': rms > 1.0,
        'five_video_packet_hashes': len(packet_hashes) == 5,
        'video_packet_payloads_vary': len(set(packet_hashes)) > 1,
        'five_video_frames_decoded': len(hashes) == 5,
        'decoded_video_frames_vary': len(set(hashes)) > 1,
        'full_video_frame_count_decoded': abs(decoded_frames - round(seconds * resolution['target_fps'])) <= 1,
        'full_decode_duration': abs(decoded_seconds - seconds) <= 0.15,
        'full_both_track_decode': progress.get('progress') == 'end',
    }
    return {'passed': all(checks.values()), 'checks': checks, 'streams': rows,
            'audio_pcm_duration_seconds': audio_seconds, 'audio_sample_rms': rms,
            'audio_samples_measured': len(samples), 'video_encoded_packet_sha256': packet_hashes,
            'decoded_video_frame_sha256': hashes,
            'decoded_video_frames': decoded_frames, 'decoded_output_seconds': decoded_seconds,
            'video_end_pts_seconds': video_end, 'audio_end_pts_seconds': audio_end,
            'decoded_all_media_wall_seconds': decoded['wall_seconds'],
            'validation_scope': 'Server decoder validation; no human listening, browser playback, network streaming, or ESP32 test'}


def transcode_case(args, video, audio_source, resolution, budget, audio, seconds):
    candidates = [2, 4, 8, 12, 20, 31]
    attempts = []
    for quality in candidates:
        output = args.output_dir / ('%s-%dk-q%d.flv' % (resolution['id'], budget, quality))
        command = [args.ffmpeg, '-hide_banner', '-nostdin', '-v', 'error', '-filter_threads', '2',
                   '-threads', '2', '-i', str(video), '-i', str(audio_source), '-t', str(seconds),
                   '-map', '0:v:0', '-map', '1:a:0', '-vf',
                   'scale=%d:%d:flags=bilinear,fps=%d' % (resolution['width'], resolution['height'], resolution['target_fps']),
                   '-c:v', 'mjpeg', '-threads:v', '2', '-pix_fmt', 'yuvj420p', '-q:v', str(quality),
                   '-c:a', 'pcm_s16le', '-ar', str(audio['sample_rate_hz']), '-ac', str(audio['channels']),
                   '-progress', 'pipe:1', '-nostats', '-f', 'flv', str(output)]
        result = execute(command)
        meta = probe_json(args.ffprobe, output, packets=True)
        video_index = next(x['index'] for x in meta['streams'] if x['codec_type'] == 'video')
        kbps = sum(int(x.get('size', 0)) for x in meta['packets'] if x['stream_index'] == video_index) * 8 / seconds / 1000
        attempts.append({'quality': quality, 'video_kbps': kbps, 'wall_seconds': result['wall_seconds']})
        if kbps <= budget:
            verification = verify_output(args.ffmpeg, args.ffprobe, output, resolution, audio, seconds)
            return {'resolution': resolution, 'video_budget_kbps': budget, 'quality': quality,
                    'video_actual_kbps': kbps, 'total_file_kbps': output.stat().st_size * 8 / seconds / 1000,
                    'file': output.name, 'sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                    'encode_wall_seconds': result['wall_seconds'], 'encode_speed_x': seconds / result['wall_seconds'],
                    'quality_attempts': attempts, 'verification': verification, 'passed': verification['passed'],
                    'rate_control_scope': 'Offline full-clip quality selection meeting measured average video budget; not live adaptation or peak CBR'}
    return {'resolution': resolution, 'video_budget_kbps': budget, 'quality_attempts': attempts,
            'passed': False, 'reason': 'No tested JPEG quality met this clip average budget'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg', required=True)
    parser.add_argument('--ffprobe', required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--bvid', default='BV13x41117TL')
    parser.add_argument('--seconds', type=int, default=20, choices=range(5, 31))
    parser.add_argument('--max-source-mib', type=int, default=30, choices=range(1, 41))
    parser.add_argument('--all-bitrates', action='store_true')
    args = parser.parse_args()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    config_bytes = args.config.read_bytes(); config = json.loads(config_bytes)
    report = {'created_at_utc': datetime.now(timezone.utc).isoformat(),
              'scope': 'Real public Bilibili source fetch, completed-file conversion and server decode baseline',
              'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'config_sha256': hashlib.sha256(config_bytes).hexdigest(),
              'passed': False, 'concurrent_network_transcode_validated': False,
              'esp32_validated': False, 'account_used': False, 'cookie_values_persisted': False,
              'signed_media_urls_persisted': False, 'cases': []}
    try:
        from curl_cffi import requests
        with requests.Session(impersonate='chrome', trust_env=False) as client:
            info, sources = resolve_sources(client, args.bvid)
            report['source_info'] = info
            if not isinstance(info['source_duration_seconds'], (int, float)) or info['source_duration_seconds'] < args.seconds:
                raise ValueError('test_video_shorter_than_requested_clip')
            aggregate = {'bytes': 0, 'limit': args.max_source_mib * 2 * 1024 * 1024}
            report['source_transfers'] = {}
            for kind in ('video', 'audio'):
                path = args.output_dir / ('source-' + kind + '.m4s')
                report['source_transfers'][kind] = fetch_source(client, sources[kind], path,
                    'https://www.bilibili.com/video/' + args.bvid + '/', args.max_source_mib * 1024 * 1024, aggregate)
                meta = probe_json(args.ffprobe, path)
                expected = 'h264' if kind == 'video' else 'aac'
                if not any(x.get('codec_name') == expected for x in meta.get('streams', [])):
                    raise ValueError('source_codec_mismatch_' + kind)
                report['source_transfers'][kind]['codec_verified'] = expected
                print('SOURCE_FETCHED', kind, report['source_transfers'][kind]['bytes'], flush=True)
        for resolution in config['resolutions']:
            rates = config['bitrate_selector']['video_target_kbps'] if args.all_bitrates else [resolution['recommended_video_bitrate_kbps']]
            for budget in rates:
                case = transcode_case(args, args.output_dir / 'source-video.m4s', args.output_dir / 'source-audio.m4s',
                                      resolution, budget, config['audio_candidate'], args.seconds)
                report['cases'].append(case)
                dump(args.output_dir / 'results.json', report)
                print('REAL_SOURCE_CASE', resolution['id'], budget, case['passed'], flush=True)
        report['passed'] = bool(report['cases']) and all(x['passed'] for x in report['cases'])
    except Exception as exc:
        report['failure'] = {'type': type(exc).__name__, 'stage_message': str(exc)[:1800] if isinstance(exc, (RuntimeError, ValueError)) else safe_error(exc)}
        print('REAL_SOURCE_PROBE_FAILED', type(exc).__name__, flush=True)
    finally:
        dump(args.output_dir / 'results.json', report)
    print('REAL_SOURCE_PROBE_DONE', report['passed'], flush=True)
    return 0 if report['passed'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
