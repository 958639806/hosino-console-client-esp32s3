"""Generate original 30-second common-format clips for on-device decoder A/B.

These files are user-copy fixtures, never embedded in production firmware.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / '.local/media-tools'))
import imageio_ffmpeg

ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
out = root / 'firmware/media-tests'
out.mkdir(parents=True, exist_ok=True)
records = []
for width, height in ((320, 180), (480, 270)):
    path = out / f'decoder-ab-{width}.avi'
    args = [
        '-f', 'lavfi', '-i', f'testsrc=size={width}x{height}:rate=30',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=22050',
        '-t', '30', '-c:v', 'mjpeg', '-q:v', '8', '-pix_fmt', 'yuvj420p',
        '-c:a', 'pcm_s16le', '-ac', '1', '-af', 'volume=0.25', '-f', 'avi',
    ]
    subprocess.run([ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-n', *args, str(path)], check=True)
    subprocess.run([ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-i', str(path), '-f', 'null', '-'], check=True)
    records.append({'path': path.relative_to(root).as_posix(), 'bytes': path.stat().st_size,
                    'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                    'arguments': args, 'host_full_decode': True})
(root / 'evidence/apps-v11-ab-samples.json').write_text(json.dumps({
    'generator': subprocess.check_output([ffmpeg, '-version'], text=True).splitlines()[0],
    'note': 'Generated test pattern and sine; host decode is not a device performance test.',
    'files': records,
}, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps(records))
