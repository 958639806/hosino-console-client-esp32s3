"""Compile the production WAV helper; validate a synthetic file with stdlib wave.
No device, network, audio recording or persistent audio files are involved.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import wave

parser = argparse.ArgumentParser()
parser.add_argument("--cc", type=Path)
args = parser.parse_args()
component = Path(__file__).resolve().parents[1]
workspace = component.parents[2]
cc = args.cc or workspace / "tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe"
with tempfile.TemporaryDirectory(prefix="hosino-asr-wav-") as temp:
    folder = Path(temp)
    for source in (component / "console_voice_wav.c", component / "console_voice_wav.h", Path(__file__).with_name("test_voice_wav.c")):
        shutil.copyfile(source, folder / source.name)
    executable = folder / "test.exe"
    subprocess.run([str(cc), "-I" + str(folder), str(folder / "console_voice_wav.c"),
                    str(folder / "test_voice_wav.c"), "-o", str(executable)], check=True)
    audio = folder / "synthetic.wav"
    subprocess.run([str(executable), str(audio)], check=True)
    with wave.open(str(audio), "rb") as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()) == (1, 2, 16000, 240000)
        samples = struct.unpack("<240000h", wav.readframes(240000))
        assert all(sample == index % 30000 for index, sample in enumerate(samples))
    assert audio.stat().st_size == 480044
    print(json.dumps({"passed": True, "file_bytes": 480044, "seconds": 15,
                      "rate": 16000, "channels": 1, "bits": 16,
                      "production_helper_sha256": hashlib.sha256((component / "console_voice_wav.c").read_bytes()).hexdigest(),
                      "scope": "Host production helper, RIFF parser and 240000 MIC1 samples; not microphone or server validation"}))
