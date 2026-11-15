"""Compile verbatim production managed-input helpers against native/GMF boundaries.

The host runs the actual input helpers. Native tail-return behavior is a small
explicit model of pinned Xtensa instructions, not a native-binary/device pass.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'overlay-apps/components/esp_player_compat/esp_player/src/extractor/player_extractor_io.c'
text=SOURCE.read_text('utf-8')
managed=text[text.index('/* HOST_MANAGED_INPUT_BEGIN'):text.index('/* HOST_MANAGED_INPUT_END */')]
read=text[text.index('int _extractor_read('):text.index('\nint _extractor_seek(')]
fixture=Path(__file__).with_suffix('.c').read_text('utf-8')
fixture=fixture.replace('/* PRODUCTION_MANAGED_INPUT */',managed).replace('/* PRODUCTION_EXTRACTOR_READ */',read)
tcc=ROOT/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'
with tempfile.TemporaryDirectory(prefix='bili_managed_input_') as folder:
    folder=Path(folder);(folder/'test.c').write_text(fixture,'utf-8')
    compiled=subprocess.run([str(tcc),'-Wall',str(folder/'test.c'),'-o',str(folder/'test.exe')],capture_output=True,text=True)
    assert compiled.returncode==0,compiled.stderr
    prefix=ROOT/'evidence/bili-public-stream-20261115/public-prefix.bin'
    result=subprocess.run([str(folder/'test.exe'),str(prefix)],capture_output=True,text=True,timeout=10)
    report={'passed':result.returncode==0,'scope':'Verbatim production input helpers; deterministic GMF/time boundaries and explicitly modeled pinned native cache-tail return. Not execution of native Xtensa binary or hardware/network acceptance.',
            'source':str(SOURCE.relative_to(ROOT)),'source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
            'public_flv_prefix':str(prefix.relative_to(ROOT)),'public_flv_prefix_sha256':hashlib.sha256(prefix.read_bytes()).hexdigest(),
            'compile_exit':compiled.returncode,'compile_diagnostics':compiled.stdout+compiled.stderr,
            'exit_code':result.returncode,'stdout':result.stdout,'stderr':result.stderr}
    out=ROOT/'evidence/bili-managed-input-fix-20261115';out.mkdir(exist_ok=True)
    (out/'host.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n','utf-8')
    print(json.dumps(report,ensure_ascii=False))
    raise SystemExit(result.returncode)
