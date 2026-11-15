"""Syntax-check latest VideoPlayer with current Xtensa build flags, without staging."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
tmp = Path('C:/esp/hosino-qspi/syntax-video-pinch-v22')
tmp.mkdir(exist_ok=True)
src = root / 'overlay-apps/components/VideoPlayer'
for name in ['VideoPlayer.cpp', 'VideoPlayer.hpp']:
    shutil.copy2(src / name, tmp / name)
build = Path('C:/esp/hosino-qspi/build-apps-80')
config = (build / 'config/sdkconfig.h').read_text('utf-8')
for option in ['CONFIG_LV_USE_FLOAT', 'CONFIG_LV_USE_GESTURE_RECOGNITION']:
    assert re.search(r'^#define ' + option + r'\s+1$', config, re.M), option
rows = json.loads((build / 'compile_commands.json').read_text('utf-8'))
row = next(x for x in rows if x['file'].replace('\\', '/').endswith('/VideoPlayer.cpp'))
command = row['command'].split(' -o ')[0]
executable, arguments = re.match(r'^("[^"]+"|\S+)\s+(.*)$', command).groups()
arguments = '-I' + tmp.as_posix() + ' ' + arguments
arguments += ' -fsyntax-only ' + (tmp / 'VideoPlayer.cpp').as_posix()
# ESP-IDF's full include flags exceed Windows' direct CreateProcess limit.
response = tmp / 'syntax.rsp'
response.write_text(arguments, 'utf-8')
result = subprocess.run([executable.strip('"'), '@' + str(response)], cwd=row['directory'], capture_output=True, text=True)
diagnostics = result.stdout + result.stderr
print('Xtensa syntax exit', result.returncode, diagnostics[:6000])
out = {'scope': __doc__, 'exit_code': result.returncode, 'diagnostic_bytes': len(diagnostics.encode()),
       'cpp_sha256': hashlib.sha256((src / 'VideoPlayer.cpp').read_bytes()).hexdigest(),
       'hpp_sha256': hashlib.sha256((src / 'VideoPlayer.hpp').read_bytes()).hexdigest(),
       'current_config_sha256': hashlib.sha256((build / 'config/sdkconfig.h').read_bytes()).hexdigest(),
       'current_flags': ['CONFIG_LV_USE_FLOAT=y', 'CONFIG_LV_USE_GESTURE_RECOGNITION=y'],
       'diagnostics': diagnostics}
(root / 'evidence/video-pinch-v22-syntax-20261114.json').write_text(json.dumps(out, indent=2) + '\n', 'utf-8')
assert result.returncode == 0
