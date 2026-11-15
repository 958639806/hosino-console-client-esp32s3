"""Verify preserved board paths and staged media inputs; no device access."""
import hashlib
import json
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parents[1]
stage = Path(r'C:\esp\hosino-qspi\project-apps')
archive = root / 'evidence/apps-v5/brookesia-qspi-apps-80mhz.zip'
preserved = [
    'overlay-apps/main/hosino_lvgl_memory.c',
    'overlay-apps/main/hosino_sdcard_override.c',
    'overlay-apps/main/hosino_sdcard_override.h',
    'overlay-apps/components/bsp_extra/src/hosino_audio_board.c',
    'overlay-apps/main/hosino_lcd_staging.c',
    'overlay-apps/main/display_buffer_override.c',
    'overlay/main/qspi_clock_override.c',
]
report = {'preserved_v5_sources': [], 'staged_media_inputs': []}
with zipfile.ZipFile(archive) as z:
    for path in preserved:
        selected = path
        if not (root / selected).exists() or selected not in z.namelist():
            raise SystemExit(f'Cannot locate preserved source: {path}')
        data = (root / selected).read_bytes()
        assert data == z.read(selected), f'Unexpected board path change: {selected}'
        report['preserved_v5_sources'].append({'path': selected, 'sha256': hashlib.sha256(data).hexdigest()})

media_roots = ['components/MusicPlayer', 'components/VideoPlayer', 'components/hosino_media_player', 'main/media_samples']
inputs = [p for folder in media_roots for p in (root / 'overlay-apps' / folder).rglob('*') if p.is_file() and p.suffix in {'.cpp', '.c', '.h', '.hpp', '.yml', '.avi', '.flac'}]
inputs.extend(root / 'overlay-apps' / name for name in ['main/main.cpp', 'main/CMakeLists.txt', 'main/hosino_media_samples.c', 'main/hosino_media_samples.h'])
for p in inputs:
    relative = p.relative_to(root / 'overlay-apps')
    data = p.read_bytes()
    assert (stage / relative).read_bytes() == data, f'Stage differs: {relative}'
    report['staged_media_inputs'].append({'path': str(relative), 'sha256': hashlib.sha256(data).hexdigest()})
(root / 'evidence/apps-v6-source-check.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(f"Preserved {len(report['preserved_v5_sources'])} board paths; staged {len(inputs)} media inputs match.")
