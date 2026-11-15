"""Generate bounded monochrome chat fallback glyphs from pinned OFL fonts.

Uses existing desktop fontTools and lv_font_conv 1.5.3; never installs into IDF.
Run --fetch once to obtain SHA-verified source fonts in .local/fonts/chat-extra.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

from fontTools import __version__ as FONTTOOLS_VERSION
from fontTools.ttLib import TTFont

COMMIT = "c30b221beb0c1da59d563feb91ec8656a1fa972a"
SOURCES = [
    ("notoemoji", "NotoEmoji[wght].ttf", "de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551", "500bb1ccf43df7bbb522112f9133a52b16e1c35e809632f5d8609b179152de5b"),
    ("notosanssymbols2", "NotoSansSymbols2-Regular.ttf", "7d5fb73b7ca67a6798101741f5d280a3d016a56a197afcd4199dbb57b4b82a21", "b118dd41337806a5d4797052c77caf3bd096aed783e5eb21b4d11154351e1ac0"),
    ("notosans", "NotoSans[wdth,wght].ttf", "bfb7bb691513f12e734dc346c03a03f784912432d7e3fa8e56efcf906fe86b3d", "cee9892f9f0cc8fe882c9e9537ee6a89621d86ee7ceaf70b02e2b2b1c25c061a"),
    ("notosanskannada", "NotoSansKannada[wdth,wght].ttf", "cca4f3b3a8cb12fb261f1b43baf5d2f7f59d90fe123d41f0065ed3a183997ec9", "9d49a4e56df9b36403b139f48bf07fd87cc5e716cb3203c54b04b674adb16238"),
    ("notosansthai", "NotoSansThai[wdth,wght].ttf", "5a1c559bb539583c8a1fd99d1c5b9491e5e14478c9cd2bd0970d5c3096cc9ef8", "2e98fd23a52d253db8612cd5942c8f2ff4111b21d2367050fdca91d8ccc374a0"),
    ("notosansgeorgian", "NotoSansGeorgian[wdth,wght].ttf", "dc591156f36842d38996c4a7a17fee9bb58e45da3e2cac7a31b7d33de700adb9", "8c02263c5d73d40544f9ed91e30c4e947407057a3cc430d7b786189aeceff6df"),
    ("notosansarabic", "NotoSansArabic[wdth,wght].ttf", "63111b5b2e074dd48cc67692e0a2726d86ee94c1c37fe8598257b7b4e87e869e", "07fc70bfeb985cc1a87a8587d0a0c80bab11c86c9dc3fd95b6f0cb332f983e96"),
]
RANGES = {
    "notosans": [(0xA0, 0x52F), (0x1D00, 0x1EFF), (0x2000, 0x214F)],
    "notosanskannada": [(0xC80, 0xCFF)],
    "notosansthai": [(0xE00, 0xE7F)],
    "notosansgeorgian": [(0x10A0, 0x10FF), (0x1C90, 0x1CBF), (0x2D00, 0x2D2F)],
    "notosansarabic": [(0x600, 0x6FF)],
    "notocjk": [(0x2000, 0x2FFF), (0x3000, 0x318F), (0x31F0, 0x31FF), (0xFE10, 0xFE4F), (0xFF00, 0xFFEF)],
}
SAMPLES = ["(๑•̀ㅂ•́)و✧", "(╯°□°）╯︵ ┻━┻", "ಠ_ಠ", "ಥ_ಥ ง ლ", "ʕ•ᴥ•ʔ",
           "😭😂🥰👍❤️🎉🚀", "(≧▽≦) (´･ω･`) Σ(ﾟДﾟ) ☆⌒ヽ(*'､^*)", "Привет Ελληνικά かな カナ"]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def wanted(family, cp):
    return cp >= 0xA0 and (family not in RANGES or any(a <= cp <= b for a, b in RANGES[family]))


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--converter", type=Path, default=Path("C:/esp/hosino-qspi/ui-tools/node_modules/lv_font_conv/lv_font_conv.js"))
    parser.add_argument("--measure-build", type=Path)
    args = parser.parse_args()
    if json.loads((args.converter.parent / "package.json").read_text())["version"] != "1.5.3":
        raise SystemExit("Requires lv_font_conv 1.5.3")
    out = root / "overlay-apps/components/hosino_chat_fonts"
    licenses = out / "licenses"
    licenses.mkdir(exist_ok=True)
    cache = root / ".local/fonts/chat-extra"
    existing = {ord(c) for c in (out / "glyphs.txt").read_text(encoding="utf-8").strip()}
    covered = existing | set(range(32, 127))
    chosen = []
    provenance = []
    for family, filename, font_sha, ofl_sha in SOURCES:
        directory = cache / family
        directory.mkdir(parents=True, exist_ok=True)
        url_base = f"https://raw.githubusercontent.com/google/fonts/{COMMIT}/ofl/{family}/"
        for name, expected in ((filename, font_sha), ("OFL.txt", ofl_sha)):
            path = directory / name
            if args.fetch and not path.exists():
                path.write_bytes(urllib.request.urlopen(url_base + urllib.parse.quote(name)).read())
            if not path.exists() or sha(path) != expected:
                raise SystemExit(f"Pinned font/license missing or SHA mismatch: {path}")
        source = directory / filename
        cmap = TTFont(source).getBestCmap()
        selected = {cp for cp, glyph in cmap.items() if glyph != ".notdef" and wanted(family, cp)} - covered
        chosen.append({"source_path": str(source), "codepoints": sorted(selected)})
        covered |= selected
        shutil.copyfile(directory / "OFL.txt", licenses / (family + "-OFL.txt"))
        provenance.append({"family": family, "filename": filename, "source_url": url_base + urllib.parse.quote(filename),
                           "sha256": font_sha, "license": "OFL-1.1", "license_sha256": ofl_sha,
                           "source_cmap_count": len(cmap), "new_glyphs": len(selected)})
    cjk = root / ".local/fonts/NotoSansCJKsc-Regular.otf"
    from generate_chat_font import FONT_SHA256, FONT_URL, measure_xtensa
    assert sha(cjk) == FONT_SHA256
    selected = {cp for cp in TTFont(cjk).getBestCmap() if wanted("notocjk", cp)} - covered
    chosen.append({"source_path": str(cjk), "codepoints": sorted(selected)})
    covered |= selected
    provenance.append({"family": "notocjk", "source_url": FONT_URL, "sha256": FONT_SHA256,
                       "license": "OFL-1.1", "license_file": "OFL.txt", "new_glyphs": len(selected)})
    for text in SAMPLES:
        missing = [f"U+{ord(c):04X}" for c in text if ord(c) not in covered]
        assert not missing, (text, missing)
    dest = out / "hosino_chat_symbols_16.c"
    with tempfile.TemporaryDirectory(prefix="hosino-chat-symbols-") as scratch:
        job = Path(scratch) / "job.json"
        report = Path(scratch) / "raster.json"
        job.write_text(json.dumps({"converter_root": str(args.converter.parent), "fonts": chosen,
                                   "output": str(dest), "report": str(report)}), encoding="utf-8")
        subprocess.run(["node", str(root / "scripts/render_chat_symbols.cjs"), str(job)], check=True)
        raster = json.loads(report.read_text(encoding="utf-8"))
    source = dest.read_text(encoding="utf-8")
    source = ("/* SPDX-License-Identifier: OFL-1.1\n * Derived Noto 16px 2bpp monochrome fallback.\n"
              " * See symbols-manifest.json, SOURCE.md and licenses/.\n"
              " * Regenerate with scripts/generate_chat_symbols.py.\n */" + source[source.index("*/") + 2:])
    # Keep bidi/format controls out of C comments; the numeric codepoint remains auditable.
    source = re.sub(r'/\* U\+([0-9a-fA-F]+) .*? \*/', r'/* U+\1 */', source)
    dest.write_text(source, encoding="utf-8")
    found = {int(x, 16) for x in re.findall(r"/\* U\+([0-9a-fA-F]+)", source)}
    expected = {cp for item in chosen for cp in item["codepoints"]}
    assert expected == found, (expected - found, found - expected)
    assert ".line_height = 18" in source and ".base_line = 3" in source
    assert ".fallback = &lv_font_montserrat_16" in source
    (out / "symbols-glyphs.txt").write_text("".join(chr(x) for x in sorted(found)) + "\n", encoding="utf-8")
    manifest = {"google_fonts_commit": COMMIT, "sources": provenance, "converter": "lv_font_conv@1.5.3",
                "host_fonttools_version": FONTTOOLS_VERSION,
                "generator_sha256": sha(Path(__file__)),
                "raster_wrapper_sha256": sha(root / "scripts/render_chat_symbols.cjs"),
                "pixel_size": 16, "bpp": 2, "bitmap_compression": "none", "glyph_count": len(found),
                "line_height": 18, "baseline": 3, "raster_normalization": raster,
                "font_c_sha256": sha(dest), "glyphs_sha256": sha(out / "symbols-glyphs.txt"),
                "fallback": "hosino_chat_font_16 -> hosino_chat_symbols_16 -> lv_font_montserrat_16",
                "coverage_samples": [{"text": text, "all_codepoints_available": True} for text in SAMPLES],
                "limitations": "Monochrome single-codepoint bitmaps only. No GSUB/GPOS shaping: combining accents, Indic/Arabic text, ZWJ families, regional flags and skin-tone sequences are not composed. VS15/VS16/tag controls render zero-width; source text is not modified. Optional display helper renders ZWJ as + and tag controls as ASCII codepoint names.",
                "storage": "Static const; no application heap copy. CONFIG_SPIRAM_RODATA decides placement. Current Apps has RODATA disabled so constants remain in mapped Flash. Device visual/performance acceptance pending."}
    if args.measure_build:
        manifest["object_measurement"] = measure_xtensa(dest, args.measure_build)
    (out / "symbols-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"glyph_count": len(found), "font_c_sha256": sha(dest),
                      "adjusted_glyph_count": raster["adjusted_glyph_count"],
                      "object_rodata_bytes": manifest.get("object_measurement", {}).get("rodata_bytes")}, indent=2))


if __name__ == "__main__":
    main()
