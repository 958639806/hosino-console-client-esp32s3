"""Generate small original media fixtures, never transcode user media."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / '.local/media-tools'))
import imageio_ffmpeg
ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
out = root / 'overlay-apps/main/media_samples'
out.mkdir(parents=True, exist_ok=True)
clips = {
    'generated-motion480.avi': [
        '-f', 'lavfi', '-i', 'testsrc=size=480x270:rate=30',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=22050',
        '-t', '4', '-c:v', 'mjpeg', '-q:v', '8', '-pix_fmt', 'yuvj420p',
        '-c:a', 'pcm_s16le', '-ac', '1', '-af', 'volume=0.25', '-f', 'avi'],
    'generated-motion.avi': [
        '-f', 'lavfi', '-i', 'testsrc=size=320x240:rate=30',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=22050',
        '-t', '4', '-c:v', 'mjpeg', '-q:v', '8', '-pix_fmt', 'yuvj420p',
        '-c:a', 'pcm_s16le', '-ac', '1', '-af', 'volume=0.25', '-f', 'avi'],
    'generated-tone.flac': [
        '-f', 'lavfi', '-i', 'sine=frequency=523.25:sample_rate=44100',
        '-t', '6', '-af', 'volume=0.25', '-c:a', 'flac', '-sample_fmt', 's16', '-ac', '2'],
    'generated-tone24.flac': [
        '-f', 'lavfi', '-i', 'sine=frequency=659.25:sample_rate=48000',
        '-t', '6', '-af', 'volume=0.25', '-c:a', 'flac', '-sample_fmt', 's32', '-bits_per_raw_sample', '24', '-ac', '2'],
}
report = {'generator': subprocess.check_output([ffmpeg, '-version'], text=True).splitlines()[0], 'files': []}
for name, args in clips.items():
    target = out / name
    subprocess.run([ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-y', *args, str(target)], check=True)
    subprocess.run([ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-i', str(target), '-f', 'null', '-'], check=True)
    data = target.read_bytes()
    report['files'].append({'name': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'arguments': args, 'host_full_decode': 'passed'})
(root / 'evidence/media-samples.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps(report, ensure_ascii=False, indent=2))
