"""Build the bounded 16px chat font; no firmware build, SDK edit or device access."""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

FONT_SHA256 = "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b"
FONT_URL = "https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"
PUNCTUATION = "，。！？；：、‘’“”（）【】《》〈〉〔〕［］｛｝…—·～￥％＋－＝／＼｜＃＠＆＊〇℃°"
REPRESENTATIVE_TEXTS = [
    "你好，星野！请查看今天的会话状态、模型用量和网络连接。",
    "对话与管理：登录、发送、停止、刷新、取消。中文输入和拼音候选。",
    "请先登录。正在连接服务器……返回主页不会重复发送消息。",
    "GB2312：一龠，100% OK! HTTPS / TLS 1.2",
]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def gb2312_hanzi():
    result = set()
    for lead in range(0xB0, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                char = bytes([lead, trail]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if "\u4e00" <= char <= "\u9fff":
                result.add(char)
    if len(result) != 6763:
        raise RuntimeError(f"Unexpected GB2312 Han count: {len(result)}")
    return result


def ime_candidates(path):
    source = path.read_text(encoding="utf-8")
    table = re.search(r"lv_ime_pinyin_def_dict\[\]\s*=\s*\{(.*?)\n\};", source, re.S)
    if not table:
        raise RuntimeError("Cannot locate the actual LVGL default Pinyin dictionary")
    pairs = re.findall(r'\{\s*"([a-z]+)"\s*,\s*"([^"\\]*)"\s*\}', table[1])
    if not pairs:
        raise RuntimeError("No Pinyin dictionary entries found")
    return pairs


def measure_xtensa(source, build_dir):
    """Compile only this font with an existing real configuration, outside the build tree."""
    entries = json.loads((build_dir / "compile_commands.json").read_text(encoding="utf-8"))
    entry = next(e for e in entries if e["file"].replace("\\", "/").endswith("/hosino_font_16.c"))
    command = entry["command"]
    compiler = Path(command.split(" ", 1)[0].strip('"'))
    size_tool = compiler.with_name(compiler.name.replace("gcc", "size"))
    with tempfile.TemporaryDirectory(prefix="hosino-chat-font-") as scratch:
        scratch = Path(scratch)
        cfile = scratch / "hosino_chat_font_16.c"
        obj = scratch / "hosino_chat_font_16.o"
        shutil.copyfile(source, cfile)
        command, replaced = re.subn(r" -o .*? -c .*$", lambda _: f' -o "{obj}" -c "{cfile}"', command)
        if replaced != 1:
            raise RuntimeError("Cannot isolate existing font compiler command")
        subprocess.run(command, cwd=entry["directory"], check=True)
        result = subprocess.run([str(size_tool), "-A", str(obj)], text=True,
                                encoding="utf-8", capture_output=True, check=True)
    sections = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0].startswith(".") and parts[1].isdigit():
            sections[parts[0]] = int(parts[1])
    rodata = sum(v for k, v in sections.items() if k.startswith(".rodata"))
    data = sum(v for k, v in sections.items() if k.startswith(".data"))
    bss = sum(v for k, v in sections.items() if k.startswith(".bss"))
    text = sum(v for k, v in sections.items() if k.startswith(".text"))
    if rodata + data + text >= 600 * 1024:
        raise RuntimeError(f"Chat font exceeds 600 KiB static payload budget: {rodata + data + text}")
    return {
        "kind": "Single Xtensa object compiled against current LVGL/SDK headers; not a firmware build",
        "rodata_bytes": rodata, "data_bytes": data, "bss_bytes": bss,
        "text_bytes": text, "static_flash_payload_bytes": rodata + data + text,
        "rodata_64k_pages_rounded_up": (rodata + 65535) // 65536,
        "sdkconfig_sha256": digest(build_dir / "config/sdkconfig.h"),
        "sections": sections,
        "psram_note": "This object measurement is static payload size, not a runtime heap measurement. CONFIG_SPIRAM_RODATA controls whether mapped rodata pages are copied into PSRAM; with it disabled, font constants remain in Flash. Confirm final linker mapping and device free memory separately.",
    }


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, default=root / ".local/fonts/NotoSansCJKsc-Regular.otf")
    parser.add_argument("--converter", type=Path, default=Path("C:/esp/hosino-qspi/ui-tools/node_modules/lv_font_conv/lv_font_conv.js"))
    parser.add_argument("--lvgl-root", type=Path, default=Path("C:/esp/hosino-qspi/project-apps/managed_components/lvgl__lvgl"))
    parser.add_argument("--measure-build", type=Path, help="Measure one font object using this existing compile_commands.json")
    args = parser.parse_args()
    if digest(args.font) != FONT_SHA256:
        raise SystemExit("Source Noto font SHA-256 does not match the pinned font")
    package = json.loads((args.converter.parent / "package.json").read_text(encoding="utf-8"))
    if package["version"] != "1.5.3":
        raise SystemExit("Requires lv_font_conv 1.5.3")
    dictionary = args.lvgl_root / "src/others/ime/lv_ime_pinyin.c"
    pairs = ime_candidates(dictionary)
    hanzi = gb2312_hanzi()
    candidates = {c for _, value in pairs for c in value if ord(c) > 127}
    ui_path = root / "overlay-apps/components/hosino_ui_fonts/glyphs.txt"
    ui = {c for c in ui_path.read_text(encoding="utf-8") if ord(c) > 127}
    symbols = hanzi | candidates | ui | set(PUNCTUATION)
    chars = "".join(sorted(symbols))
    out = root / "overlay-apps/components/hosino_chat_fonts"
    out.mkdir(parents=True, exist_ok=True)
    dest = out / "hosino_chat_font_16.c"
    subprocess.run(["node", str(args.converter), "--font", str(args.font), "--symbols", chars,
                    "--size", "16", "--bpp", "2", "--no-compress", "--no-kerning",
                    "--format", "lvgl", "--lv-include", "lvgl.h", "--lv-font-name", "hosino_chat_font_16",
                    "--lv-fallback", "hosino_chat_symbols_16", "-o", str(dest)], check=True)
    source = dest.read_text(encoding="utf-8")
    source = ("/* SPDX-License-Identifier: OFL-1.1\n"
              " * Noto Sans CJK SC Regular, 16px 2bpp chat subset.\n"
              " * See OFL.txt, SOURCE.md and font-manifest.json.\n"
              " * Regenerate with scripts/generate_chat_font.py.\n */" + source[source.index("*/") + 2:])
    dest.write_text(source, encoding="utf-8")
    found = {chr(int(x, 16)) for x in re.findall(r"/\* U\+([0-9a-fA-F]+)", source)}
    if missing := symbols - found:
        raise SystemExit("Missing glyphs: " + "".join(sorted(missing)))
    assert ".fallback = &hosino_chat_symbols_16" in source
    assert re.search(r"\.bpp\s*=\s*2", source)
    assert re.search(r"\.bitmap_format\s*=\s*0", source)
    assert not re.search(r"\b(?:malloc|calloc|realloc)\s*\(", source)
    results = []
    for sentence in REPRESENTATIVE_TEXTS + [value for _, value in pairs]:
        decoded = sentence.encode("utf-8").decode("utf-8", errors="strict")
        missing = {c for c in decoded if ord(c) > 127 and c not in found}
        assert not missing, repr(missing)
        results.append({"text": sentence, "pass": True})
    (out / "glyphs.txt").write_text(chars + "\n", encoding="utf-8")
    shutil.copy2(root / "overlay-apps/components/hosino_ui_fonts/OFL.txt", out / "OFL.txt")
    manifest = {
        "source_font": args.font.name, "source_url": FONT_URL, "source_sha256": FONT_SHA256,
        "license": "OFL-1.1", "converter": "lv_font_conv@1.5.3", "pixel_size": 16, "bpp": 2,
        "bitmap_compression": "none", "kerning": False, "fallback": "hosino_chat_symbols_16 -> lv_font_montserrat_16",
        "gb2312_hanzi_count": len(hanzi), "glyph_count": len(found),
        "default_pinyin_entry_count": len(pairs), "default_pinyin_candidate_codepoints": len(candidates),
        "extra_pinyin_codepoints_outside_gb2312": len(candidates - hanzi),
        "lvgl_default_dictionary_sha256": digest(dictionary), "ui_glyph_source_sha256": digest(ui_path),
        "line_height": int(re.search(r"\.line_height\s*=\s*(\d+)", source)[1]),
        "font_c_sha256": digest(dest), "font_c_source_bytes": dest.stat().st_size,
        "glyphs_sha256": digest(out / "glyphs.txt"), "license_sha256": digest(out / "OFL.txt"),
        "coverage": "All 6763 GB2312 Han, listed punctuation, existing UI glyphs, actual LVGL default Pinyin candidates. Separate symbols fallback adds monochrome codepoint emoji/kaomoji; see symbols-manifest.json. Not complete Unicode/CJK or composed emoji.",
        "storage": "Static const LVGL data; no application heap copy. Placement follows CONFIG_SPIRAM_RODATA: enabled copies mapped rodata pages to PSRAM; disabled retains font constants in memory-mapped Flash. Instruction placement is independent.",
    }
    if args.measure_build:
        manifest["object_measurement"] = measure_xtensa(dest, args.measure_build)
    (out / "font-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    validation = {"passed": True, "kind": "Host UTF-8/codepoint coverage and static-font checks; no visual/device acceptance",
                  "font_c_sha256": digest(dest), "gb2312_covered": len(hanzi & found),
                  "pinyin_entries_checked": len(pairs), "representative_texts": results[:len(REPRESENTATIVE_TEXTS)],
                  "all_pinyin_candidate_strings_covered": True, "fallback_ascii16": True,
                  "uncompressed_2bpp": True, "no_font_heap_copy": True}
    (out / "validation.json").write_text(json.dumps(validation, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
