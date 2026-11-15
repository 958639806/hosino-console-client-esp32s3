"""Generate small static CJK subsets for the localized overlay (no device access)."""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path

FONT_SHA256 = "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b"
FONT_URL = "https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--apps", action="store_true")
    parser.add_argument("--font", type=Path, default=root / ".local/fonts/NotoSansCJKsc-Regular.otf")
    parser.add_argument("--converter", type=Path,
                        default=Path(r"C:\esp\hosino-qspi\ui-tools\node_modules\lv_font_conv\lv_font_conv.js"))
    args = parser.parse_args()
    if hashlib.sha256(args.font.read_bytes()).hexdigest() != FONT_SHA256:
        raise SystemExit("Source font SHA-256 does not match the reviewed font")
    package = json.loads((args.converter.parent / "package.json").read_text(encoding="utf-8"))
    if package["version"] != "1.5.3":
        raise SystemExit("This font build requires lv_font_conv 1.5.3")
    overlay = root / ("overlay-apps" if args.apps else "overlay-localized")
    out = overlay / "components/hosino_ui_fonts"
    out.mkdir(parents=True, exist_ok=True)
    if args.apps:
        for name in ("CMakeLists.txt", "hosino_ui_fonts.h", "OFL.txt", "NOTICE.txt"):
            shutil.copy2(root / "overlay-localized/components/hosino_ui_fonts" / name, out / name)
    symbols = set()
    scan_roots = [root / "overlay-localized", overlay] if args.apps else [overlay]
    for path in (p for scan_root in scan_roots for p in scan_root.rglob("*")):
        if not path.is_file() or any(part in {
            "hosino_ui_fonts", "hosino_chat_fonts", "tests", "test", "test_apps", "fixtures",
            ".build", ".tools", "__pycache__",
        } for part in path.parts):
            continue
        if path.suffix in {".c", ".cpp", ".h", ".hpp"}:
            literals = re.findall(r'"(?:\\.|[^"\\])*"', path.read_text(encoding="utf-8"))
            symbols.update(c for literal in literals for c in literal if ord(c) > 127)
        elif path.name == "localization-glyphs.txt":
            symbols.update(c for c in path.read_text(encoding="utf-8") if ord(c) > 127)
    chars = "".join(sorted(symbols))
    if not chars:
        raise SystemExit("No translated UI text was found")
    (out / "glyphs.txt").write_text(chars + "\n", encoding="utf-8")
    records = []
    for size in (14, 16, 18, 20):
        dest = out / f"hosino_font_{size}.c"
        subprocess.run(["node", str(args.converter), "--font", str(args.font),
                        "--symbols", chars, "--size", str(size), "--bpp", "4", "--no-compress",
                        "--format", "lvgl", "--lv-include", "lvgl.h", "--lv-font-name", f"hosino_font_{size}",
                        "--lv-fallback", f"lv_font_montserrat_{size}", "-o", str(dest)], check=True)
        source = dest.read_text(encoding="utf-8")
        # Pin provenance without recording machine-specific converter arguments.
        header_end = source.index("*/") + 2
        source = ("/* SPDX-License-Identifier: OFL-1.1\n"
                  " * Noto Sans CJK SC Regular subset; see OFL.txt and font-manifest.json.\n"
                  " * Regenerate with scripts/generate_ui_fonts.py.\n */" + source[header_end:])
        dest.write_text(source, encoding="utf-8")
        found = set(int(x, 16) for x in re.findall(r"/\* U\+([0-9A-Fa-f]+)", source))
        missing = symbols - {chr(x) for x in found}
        if missing:
            raise SystemExit(f"Missing glyphs in {dest.name}: {''.join(sorted(missing))}")
        records.append({"file": dest.name, "font_size": size,
                        "line_height": int(re.search(r"\.line_height\s*=\s*(\d+)", source)[1]),
                        "glyph_count": len(found), "source_bytes": dest.stat().st_size,
                        "sha256": hashlib.sha256(dest.read_bytes()).hexdigest()})
    manifest = {"source_font": "NotoSansCJKsc-Regular.otf", "source_url": FONT_URL,
                "source_sha256": FONT_SHA256, "license": "OFL-1.1", "converter": "lv_font_conv@1.5.3",
                "format": "LVGL static 4bpp uncompressed bitmap; ASCII and symbols use Montserrat fallback",
                "subset_codepoints": [f"U+{ord(c):04X}" for c in chars], "fonts": records}
    (out / "font-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"subset_characters": len(chars), "fonts": records}, indent=2))


if __name__ == "__main__":
    main()
