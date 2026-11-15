#!/usr/bin/env python3
"""Build an isolated FFmpeg 9.0.2 with Espressif's private MJPEG-in-FLV ID.

No sudo, package manager, service, firewall, or credential operations. The build
is for a private compatibility probe, not a published server deployment.
"""
import argparse
from datetime import datetime, timezone
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import time
import urllib.request

VERSION = "9.0.2"
URL = f"https://ffmpeg.org/releases/ffmpeg-{VERSION}.tar.xz"
KEY_FINGERPRINT = "FCF986EA15E6E293A5644F10B4322F04D67658D8"
ORIGINALS = {
    "flv.h": "b117ed9b9227ce83a95a352dba5bfcd81e30fb30f418e07820287ab4a3b087e7",
    "flvdec.c": "d1d3e0ecce9e195e2bf4803779a7a469e271e770207f5ad5b15abdee559b4e4e",
    "flvenc.c": "ca8f1e6b4fed159fb6fa698fd07cf97ba7ae224c9e979c790df8bf7ca26a020e",
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def download(url, target, max_bytes):
    if target.exists():
        raise RuntimeError(f"Refusing to overwrite download: {target}")
    request = urllib.request.Request(url, headers={"User-Agent": "hosino-private-compatibility-probe/1"})
    with urllib.request.urlopen(request, timeout=30) as response, target.open("xb") as out:
        total = 0
        while chunk := response.read(1024 * 1024):
            total += len(chunk)
            if total > max_bytes:
                raise RuntimeError("Download exceeds expected bounded size")
            out.write(chunk)


def bounded_copy(source, target, max_bytes):
    source = source.expanduser().resolve(strict=True)
    if not source.is_file() or source.stat().st_size > max_bytes or target.exists():
        raise RuntimeError(f"Invalid bounded offline input: {source.name}")
    shutil.copyfile(source, target)


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f"Patch context not unique: {old[:100]!r}")
    return text.replace(old, new, 1)


def patched_sources(source):
    changes, patch = {}, []
    for name, expected in ORIGINALS.items():
        path = source / "libavformat" / name
        if digest(path) != expected:
            raise RuntimeError(f"Pinned FFmpeg {VERSION} source differs: {name}")
        before = path.read_text("utf-8")
        if name == "flv.h":
            after = replace_once(before, "    FLV_CODECID_H263    = 2,",
                "    FLV_CODECID_MJPEG   = 1, /* Espressif private extension */\n    FLV_CODECID_H263    = 2,")
        elif name == "flvenc.c":
            after = replace_once(before, "    { AV_CODEC_ID_H264,     FLV_CODECID_H264 },",
                "    { AV_CODEC_ID_H264,     FLV_CODECID_H264 },\n    { AV_CODEC_ID_MJPEG,    FLV_CODECID_MJPEG },")
        else:
            after = replace_once(before,
                "    case FLV_CODECID_H263:\n        return vpar->codec_id == AV_CODEC_ID_FLV1;",
                "    case FLV_CODECID_MJPEG:\n        return vpar->codec_id == AV_CODEC_ID_MJPEG;\n    case FLV_CODECID_H263:\n        return vpar->codec_id == AV_CODEC_ID_FLV1;")
            after = replace_once(after,
                "    case FLV_CODECID_H263:\n        par->codec_id = AV_CODEC_ID_FLV1;\n        break;",
                "    case FLV_CODECID_MJPEG:\n        par->codec_id = AV_CODEC_ID_MJPEG;\n        break;\n    case FLV_CODECID_H263:\n        par->codec_id = AV_CODEC_ID_FLV1;\n        break;")
        patch.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                     fromfile=f"a/libavformat/{name}", tofile=f"b/libavformat/{name}"))
        path.write_text(after, "utf-8")
        changes[name] = {"before": expected, "after": digest(path)}
    return changes, "".join(patch)


def run_logged(argv, cwd, log, timeout):
    started = time.monotonic()
    with log.open("wb") as output:
        result = subprocess.run(argv, cwd=cwd, stdout=output, stderr=subprocess.STDOUT,
                                timeout=timeout, check=False, stdin=subprocess.DEVNULL)
    row = {"argv": argv, "exit_code": result.returncode, "elapsed_seconds": round(time.monotonic()-started, 3),
           "log": str(log)}
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); inspect {log}")
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="Fresh directory beneath ~/.cache")
    parser.add_argument("--jobs", type=int, choices=[1, 2], default=2)
    parser.add_argument("--source-archive", type=Path, help="Offline official ffmpeg-9.0.2.tar.xz")
    parser.add_argument("--source-signature", type=Path, help="Offline official .tar.xz.asc")
    parser.add_argument("--signing-key", type=Path, help="Offline official ffmpeg-devel.asc")
    args = parser.parse_args()
    cache = (Path.home()/".cache").resolve()
    root = args.root.expanduser().resolve()
    if root == cache or not root.is_relative_to(cache) or root.exists():
        raise SystemExit("--root must be a fresh directory strictly beneath ~/.cache")
    if not all(shutil.which(x) for x in ("gcc", "make", "nice")):
        raise SystemExit("Existing gcc/make/nice required; this script installs no system packages")
    offline = any((args.source_archive, args.source_signature, args.signing_key))
    if offline and not all((args.source_archive, args.source_signature, args.signing_key)):
        raise SystemExit("Offline mode requires archive, detached signature and signing key together")
    if offline and not shutil.which("gpg"):
        raise SystemExit("Offline inputs require existing gpg to verify the pinned signature")
    root.mkdir(parents=True)
    record = {"version": VERSION, "created_at_utc": datetime.now(timezone.utc).isoformat(),
              "status": "started", "scope": "Isolated user-directory compatibility build; no services or system packages changed",
              "source_url": URL, "source_verification": "HTTPS and three independently pinned upstream source-file hashes",
              "system_packages_installed": False, "services_changed": False, "network_listeners_opened": False}
    report = root/"build-record.json"
    try:
        print("Reading offline source" if offline else "Downloading pinned official source", flush=True)
        archive = root/f"ffmpeg-{VERSION}.tar.xz"
        if offline:
            bounded_copy(args.source_archive, archive, 40*1024*1024)
        else:
            download(URL, archive, 40*1024*1024)
        record["source_archive_sha256"] = digest(archive)
        if shutil.which("gpg"):
            gpg_home = root/"gnupg"
            gpg_home.mkdir(mode=0o700)
            key, sig = root/"ffmpeg-devel.asc", root/(archive.name+".asc")
            if offline:
                bounded_copy(args.signing_key, key, 128*1024)
                bounded_copy(args.source_signature, sig, 128*1024)
            else:
                download("https://ffmpeg.org/ffmpeg-devel.asc", key, 128*1024)
                download(URL+".asc", sig, 128*1024)
            gpg = ["gpg", "--batch", "--homedir", str(gpg_home)]
            run_logged(gpg+["--import", str(key)], root, root/"gpg-import.log", 30)
            keys = subprocess.run(gpg+["--with-colons", "--fingerprint"], capture_output=True, text=True, timeout=15, check=True)
            if not any(line.startswith("fpr:") and line.split(":")[9] == KEY_FINGERPRINT for line in keys.stdout.splitlines()):
                raise RuntimeError("Downloaded FFmpeg key fingerprint differs from pinned release key")
            run_logged(gpg+["--status-fd", "1", "--verify", str(sig), str(archive)], root, root/"gpg-verify.log", 30)
            status = (root/"gpg-verify.log").read_text("utf-8", errors="replace")
            valid = [line.split() for line in status.splitlines() if line.startswith("[GNUPG:] VALIDSIG ")]
            if not any(KEY_FINGERPRINT in (row[2], row[-1]) for row in valid):
                raise RuntimeError("Source signature was not made by the pinned FFmpeg primary key")
            record["source_verification"] = "Official detached signature verified using isolated GPG home and pinned primary fingerprint"
            record["signing_key_fingerprint"] = KEY_FINGERPRINT
        print("Checking archive boundaries and exact patch contexts", flush=True)
        with tarfile.open(archive, "r:xz") as bundle:
            members = bundle.getmembers()
            if len(members) > 30000 or sum(x.size for x in members) > 400*1024*1024:
                raise RuntimeError("Unexpected archive size")
            for member in members:
                target = (root/member.name).resolve()
                if not target.is_relative_to(root) or not (member.isfile() or member.isdir()):
                    raise RuntimeError(f"Unsafe archive member: {member.name}")
                if member.name != f"ffmpeg-{VERSION}" and not member.name.startswith(f"ffmpeg-{VERSION}/"):
                    raise RuntimeError(f"Unexpected source archive path: {member.name}")
            bundle.extractall(root, members=members, filter="data")
        source = root/f"ffmpeg-{VERSION}"
        changes, patch = patched_sources(source)
        (root/"espressif-mjpeg-flv-9.0.2.patch").write_text(patch, "utf-8")
        record["patched_files"] = changes
        record["patch_sha256"] = digest(root/"espressif-mjpeg-flv-9.0.2.patch")
        prefix = root/"installed"
        configure = [str(source/"configure"), f"--prefix={prefix}", "--disable-everything", "--disable-autodetect",
            "--disable-doc", "--disable-debug", "--disable-shared", "--enable-static", "--enable-ffmpeg", "--enable-ffprobe",
            "--enable-avcodec", "--enable-avformat", "--enable-avfilter", "--enable-avdevice", "--enable-swscale", "--enable-swresample",
            "--enable-protocol=file,pipe,http,tcp", "--enable-indev=lavfi", "--enable-filter=testsrc2,sine,scale,fps,format,pad,crop,aresample,aformat,asetpts,setpts,anull,null",
            "--enable-encoder=mjpeg,pcm_s16le,rawvideo", "--enable-decoder=mjpeg,h264,aac,pcm_s16le,rawvideo,wrapped_avframe",
            "--enable-parser=mjpeg,h264,aac", "--enable-demuxer=mov,avi,flv,matroska,wav,pcm_s16le", "--enable-muxer=avi,flv,wav,null,pcm_s16le,rawvideo"]
        if not shutil.which("nasm"):
            configure.append("--disable-x86asm")
        openssl = bool(shutil.which("pkg-config") and subprocess.run(["pkg-config", "--exists", "openssl"], check=False).returncode == 0)
        if not openssl and Path("/usr/include/openssl/ssl.h").is_file():
            # FFmpeg configure has its own check_lib(-lssl -lcrypto) fallback;
            # verify the same headers/libraries without installing pkg-config.
            probe_argv = ["gcc", "-x", "c", "-", "-o", str(root/"openssl-link-probe"), "-lssl", "-lcrypto"]
            probe = subprocess.run(probe_argv, input="#include <openssl/ssl.h>\nint main(void){SSL_CTX *p=SSL_CTX_new(TLS_method());SSL_CTX_free(p);return 0;}\n",
                                   text=True, capture_output=True, timeout=20, check=False)
            (root/"openssl-link-probe.log").write_text(probe.stdout+probe.stderr, "utf-8")
            record["openssl_link_probe"] = {"argv": probe_argv, "exit_code": probe.returncode}
            openssl = probe.returncode == 0
        record["https_input_enabled"] = bool(openssl)
        if openssl:
            configure += ["--enable-version3", "--enable-openssl", "--enable-protocol=https,tls"]
        print("Configuring isolated build", flush=True)
        record["configure"] = run_logged(configure, source, root/"configure.log", 120)
        print(f"Building with {args.jobs} jobs at nice 10", flush=True)
        record["make"] = run_logged(["nice", "-n", "10", "make", f"-j{args.jobs}"], source, root/"make.log", 1200)
        record["install"] = run_logged(["nice", "-n", "10", "make", "install"], source, root/"install.log", 120)
        record["binaries"] = {name: {"path": str(prefix/"bin"/name), "sha256": digest(prefix/"bin"/name)} for name in ("ffmpeg", "ffprobe")}
        record["status"] = "built"
        print(json.dumps({"status": "built", "bin": str(prefix/"bin"), "https_input_enabled": bool(openssl)}), flush=True)
    except Exception as exc:
        record.update(status="failed", error=type(exc).__name__, error_message=str(exc))
        raise
    finally:
        report.write_text(json.dumps(record, ensure_ascii=False, indent=2)+"\n", "utf-8")


if __name__ == "__main__":
    main()
