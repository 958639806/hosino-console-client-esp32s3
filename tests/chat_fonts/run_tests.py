"""Test actual production display helper and generated font bitmaps; no device/build."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import struct
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
COMP = ROOT / "overlay-apps/components/hosino_chat_fonts"


def parse_font(path):
    text = path.read_text(encoding="utf8")
    bitmap = text.split("glyph_bitmap[] = {", 1)[1].split("};", 1)[0]
    chunks = re.findall(r"/\* U\+([0-9A-Fa-f]+)[^\n]*?\*/(.*?)(?=/\* U\+|\Z)", bitmap, re.S)
    desc = text.split("glyph_dsc[] = {", 1)[1].split("};", 1)[0]
    entries = re.findall(r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", desc)[1:]
    assert len(chunks) == len(entries)
    bpp = int(re.search(r"\.bpp = (\d+)", text)[1])
    result = {}
    for (code, raw), entry in zip(chunks, entries):
        _, advance, width, height, x, y = map(int, entry)
        data = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{1,2})", raw))
        assert len(data) >= (width * height * bpp + 7) // 8
        result[int(code, 16)] = (advance, width, height, x, y, data, bpp)
    return result


def main():
    tcc = ROOT / "tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe"
    with tempfile.TemporaryDirectory(prefix="hosino-chat-font-tests-") as temp:
        temp = Path(temp)
        for name in ["hosino_chat_display.c", "hosino_chat_display.h"]:
            shutil.copyfile(COMP / name, temp / name)
        shutil.copyfile(HERE / "test_display.c", temp / "test_display.c")
        exe = temp / "test.exe"
        subprocess.run([str(tcc), "-Wall", "-I" + str(temp), str(temp / "hosino_chat_display.c"),
                        str(temp / "test_display.c"), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
    symbols = parse_font(COMP / "hosino_chat_symbols_16.c")
    chat = parse_font(COMP / "hosino_chat_font_16.c")
    ascii_font = parse_font(Path("C:/esp/hosino-qspi/project-apps/managed_components/lvgl__lvgl/src/font/lv_font_montserrat_16.c"))
    manifest = json.loads((COMP / "symbols-manifest.json").read_text(encoding="utf8"))
    for cp, (advance, w, h, x, y, data, bpp) in symbols.items():
        assert bpp == 2 and h <= 18 and y >= -3 and y + h <= 15, hex(cp)
    samples = manifest["coverage_samples"]
    for sample in samples:
        for char in sample["text"]:
            cp = ord(char)
            glyph = chat.get(cp, symbols.get(cp, ascii_font.get(cp)))
            assert glyph is not None, hex(cp)
            if cp not in [0x20, 0xFE0E, 0xFE0F]:
                assert any(glyph[5]), (hex(cp), "empty glyph bitmap")
    # Render actual generated LVGL alpha rasters, not another desktop typeface.
    width, height = 700, len(samples) * 42 + 20
    background = (20, 26, 36)
    rgb = bytearray(background * (width * height))
    for i, sample in enumerate(samples):
        px, py = 12, 12 + i * 42
        for char in sample["text"]:
            advance, w, h, x, y, bitmap, bpp = chat.get(ord(char), symbols.get(ord(char), ascii_font.get(ord(char))))
            mask = (1 << bpp) - 1
            for row in range(h):
                for col in range(w):
                    bit = (row * w + col) * bpp
                    alpha = ((bitmap[bit // 8] >> (8 - bpp - bit % 8)) & mask) * 255 // mask
                    color = tuple((fg * alpha + bg * (255 - alpha)) // 255 for fg, bg in zip((239, 244, 255), background))
                    for dy in range(2):
                        for dx in range(2):
                            dest_x = round(px + x * 2) + col * 2 + dx
                            dest_y = py + (15 - h - y) * 2 + row * 2 + dy
                            if 0 <= dest_x < width and 0 <= dest_y < height and alpha:
                                offset = (dest_y * width + dest_x) * 3
                                rgb[offset:offset + 3] = bytes(color)
            px += advance / 16 * 2
    def chunk(name, value):
        return struct.pack('>I', len(value)) + name + value + struct.pack('>I', zlib.crc32(name + value))
    scanlines = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(scanlines)) + chunk(b'IEND', b'')
    (HERE / "glyph-preview.png").write_bytes(png)
    files = ["hosino_chat_font_16.c", "hosino_chat_symbols_16.c", "hosino_chat_display.c", "hosino_chat_display.h"]
    validation = {"passed": True, "kind": "Actual production C host tests + generated LVGL bitmap coverage/layout; no device visual acceptance",
                  "host_test_output": run.stdout.strip(), "symbol_glyphs_checked": len(symbols),
                  "sample_bitmap_coverage": samples, "all_supplemental_glyphs_fit_18px": True,
                  "source_sha256": {name: hashlib.sha256((COMP / name).read_bytes()).hexdigest() for name in files},
                  "preview": "glyph-preview.png", "not_validated": ["device rendering", "combined emoji shaping", "whole firmware link"]}
    (HERE / "validation.json").write_text(json.dumps(validation, ensure_ascii=False, indent=2) + "\n", encoding="utf8")
    print(run.stdout.strip())
    print(f"PASS {len(symbols)} generated symbol glyph line boxes and {len(samples)} complete sample bitmap strings")


if __name__ == "__main__":
    main()
