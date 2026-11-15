"""Run real JPEGDEC C core tests using existing TCC/FFmpeg, with no installation.

Usage: python run_host_tests.py --tcc <tcc.exe> --ffmpeg <ffmpeg.exe>
    --codec-include <managed_components/espressif__esp_video_codec/include>
    --work <ASCII scratch directory>
"""
import argparse
import pathlib
import shutil
import subprocess
import struct

def canonical_jpeg(width, height, sampling, color=None):
    """Independent baseline encoder: DC-only 8x8 tiles, canonical unit chroma.

    Tests MCU order and edge geometry; FFmpeg independently decodes the result
    to a reference. FFmpeg's own complex 420 images exercise AC decoding too.
    """
    def marker(code, payload):
        return bytes((255, code)) + struct.pack(">H", len(payload) + 2) + payload
    hs, vs = sampling
    # JPEGDEC intentionally rejects inputs under 256 bytes; use a legal COM
    # segment so tiny single-colour fixtures still exercise that decoder.
    out = b"\xff\xd8" + marker(0xfe, b"host-test" + bytes(128))
    out += marker(0xdb, bytes((0,)) + bytes((1,)) * 64)
    out += marker(0xc0, bytes((8,)) + struct.pack(">HH", height, width) +
                  bytes((3, 1, hs * 16 + vs, 0, 2, 0x11, 0, 3, 0x11, 0)))
    out += marker(0xc4, bytes((0, 0, 0, 0, 12)) + bytes(12) + bytes(range(12)) +
                  bytes((0x10, 1)) + bytes(15) + bytes((0,)))
    out += marker(0xda, bytes((3, 1, 0, 2, 0, 3, 0, 0, 63, 0)))
    bits = []
    previous = [0, 0, 0]
    def block(value, component):
        dc = 8 * (value - 128)
        difference = dc - previous[component]
        previous[component] = dc
        category = abs(difference).bit_length()
        bits.append(f"{category:04b}")
        if category:
            magnitude = difference if difference >= 0 else difference + (1 << category) - 1
            bits.append(f"{magnitude:0{category}b}")
        bits.append("0")  # EOB
    for my in range((height + vs * 8 - 1) // (vs * 8)):
        for mx in range((width + hs * 8 - 1) // (hs * 8)):
            for by in range(vs):
                for bx in range(hs):
                    y = color[0] if color else 48 + ((mx * hs + bx) * 13 + (my * vs + by) * 7) % 160
                    block(y, 0)
            block(color[1] if color else 128, 1)
            block(color[2] if color else 128, 2)
    bitstream = "".join(bits)
    bitstream += "1" * ((-len(bitstream)) % 8)
    payload = bytes(int(bitstream[i:i+8], 2) for i in range(0, len(bitstream), 8))
    return out + payload.replace(b"\xff", b"\xff\x00") + b"\xff\xd9"

p = argparse.ArgumentParser()
for key in ("tcc", "ffmpeg", "codec-include", "work"):
    p.add_argument("--" + key, required=True)
p.add_argument("--avi", action="append", default=[], help="Also test first/middle/last original JPEG packets")
a = p.parse_args()
component = pathlib.Path(__file__).resolve().parents[1]
work = pathlib.Path(a.work).resolve()
work.mkdir(parents=True, exist_ok=True)
for name in ("hosino_jpegdec.c", "jpegdec_core.c"):
    shutil.copy2(component / name, work / name)
for name in ("include", "vendor", "tests"):
    shutil.copytree(component / name, work / name, dirs_exist_ok=True)
codec = pathlib.Path(a.codec_include)
for relative in ("esp_video_codec_types.h", "decoder/esp_video_dec.h", "decoder/esp_video_dec_reg.h"):
    shutil.copy2(codec / relative, work / "include" / pathlib.Path(relative).name)
exe = work / "jpegdec-tests.exe"
subprocess.run([a.tcc, "-DESP_PLATFORM=1", "-DNO_SIMD=1", "-DHOSINO_JPEGDEC_HOST_TEST=1",
                "-include", str(work / "tests/stubs/host_intrinsics.h"),
                "-I" + str(work / "include"), "-I" + str(work / "tests/stubs"),
                "-I" + str(work / "vendor/JPEGDEC/src"),
                str(work / "hosino_jpegdec.c"), str(work / "jpegdec_core.c"),
                str(work / "tests/test_decode.c"), "-o", str(exe)], check=True)
cases = []
for width, height in ((320, 180), (480, 270), (317, 181)):
    for pixfmt in ("yuvj420p", "yuvj422p", "yuvj444p"):
        path = work / f"{width}x{height}-{pixfmt}.jpg"
        subprocess.run([a.ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
                        f"testsrc=size={width}x{height}:rate=1", "-frames:v", "1",
                        "-c:v", "mjpeg", "-q:v", "5", "-pix_fmt", pixfmt, str(path)], check=True)
        cases.extend((str(path), str(width), str(height), "-1" if pixfmt == "yuvj420p" else "-2"))
    for label, sampling in (("420", (2, 2)), ("422", (2, 1)), ("444", (1, 1))):
        path = work / f"canonical-{width}x{height}-{label}.jpg"
        path.write_bytes(canonical_jpeg(width, height, sampling))
        subprocess.run([a.ffmpeg, "-v", "error", "-y", "-i", str(path), "-frames:v", "1",
                        "-pix_fmt", "rgb565le", "-f", "rawvideo", str(path) + ".rgb565le"], check=True)
        cases.extend((str(path), str(width), str(height), "-1"))
for color, yuv, index in (("red", (76, 85, 255), 0), ("green", (150, 44, 21), 1), ("blue", (29, 255, 107), 2)):
    path = work / f"{color}.jpg"
    path.write_bytes(canonical_jpeg(32, 18, (1, 1), yuv))
    subprocess.run([a.ffmpeg, "-v", "error", "-y", "-i", str(path), "-frames:v", "1",
                    "-pix_fmt", "rgb565le", "-f", "rawvideo", str(path) + ".rgb565le"], check=True)
    cases.extend((str(path), "32", "18", str(index)))
for avi in a.avi:
    data = pathlib.Path(avi).read_bytes()
    packets = []
    def chunks(start, end):
        offset = start
        while offset + 8 <= end:
            name = data[offset:offset + 4]
            size = int.from_bytes(data[offset + 4:offset + 8], "little")
            payload = offset + 8
            if payload + size > end:
                raise ValueError("Invalid AVI chunk bounds")
            if name in (b"RIFF", b"LIST"):
                chunks(payload + 4, payload + size)
            elif name == b"00dc":
                packets.append(data[payload:payload + size])
            offset = payload + size + size % 2
    chunks(0, len(data))
    if not packets:
        raise ValueError("No MJPEG packets in " + avi)
    for index in (0, len(packets) // 2, len(packets) - 1):
        packet = packets[index]
        sof = packet.index(b"\xff\xc0")
        height, width = struct.unpack(">HH", packet[sof + 5:sof + 9])
        path = work / f"{pathlib.Path(avi).stem}-packet-{index}.jpg"
        path.write_bytes(packet)
        print(f"AVI {avi}: frame {index}/{len(packets)} SOF {packet[sof:sof+19].hex()}", flush=True)
        cases.extend((str(path), str(width), str(height), "-1"))
subprocess.run([str(exe), *cases], check=True)
