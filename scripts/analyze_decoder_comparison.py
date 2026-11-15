"""Analyze ESP Player logs without conflating files, engines or control windows.

Usage: python scripts/analyze_decoder_comparison.py capture.log [more.log ...]
       --json evidence/comparison.json --text evidence/comparison.txt
No device access. Only explicitly requested output files are written.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
PREFIX = re.compile(r"[IWEVD] \((\d+)\) ")
RECORD = re.compile(r"\b(MEDIA_PROFILE|MEDIA_PIPE|MEDIA_FILE|MEDIA_DROP)\s+(.*)")
KV = re.compile(r"([a-zA-Z_][\w]*)=([^\s]+)")
SOURCE = re.compile(r"Video source (\d+)x(\d+) @ (\d+) FPS(?:.*?decoder=([^\s;]+))?")
REQUIRED = {
    "MEDIA_PROFILE": {"window_ms", "width", "height", "source_fps", "decode_ok",
                      "decode_non_ok", "decode_avg_us", "decode_max_us", "sink_n",
                      "sink_avg_us", "sink_max_us", "audio_gap_max_us"},
    "MEDIA_PIPE": {"window_ms", "state", "seeking", "pcm_bytes", "short", "write_errors",
                   "push_non_ok", "ain_non_ok"},
    "MEDIA_FILE": {"window_ms", "read_n", "bytes", "read_us", "read_max_us", "over20",
                   "over40", "over80", "read_errors", "seek_n", "seek_errors"},
    "MEDIA_DROP": {"window_ms", "audio_n", "audio_late_max_us", "wall_n",
                   "wall_late_max_us", "reanchor_n"},
}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def values(text: str) -> dict:
    result = {}
    for key, value in KV.findall(text):
        result[key] = int(value) if re.fullmatch(r"-?\d+", value) else value
    return result


def summarize(rows: list[dict]) -> dict:
    if not rows:
        return {"windows": 0, "duration_ms": 0}

    def total(kind, key):
        return sum(row.get(kind, {}).get(key, 0) for row in rows)

    def maximum(kind, key):
        return max((row.get(kind, {}).get(key, 0) for row in rows), default=0)

    def weighted(key, count):
        n = total("profile", count)
        return round(sum(r["profile"].get(key, 0) * r["profile"].get(count, 0)
                         for r in rows) / n, 3) if n else None

    ms = total("profile", "window_ms")
    file_rows = sum("file" in r for r in rows)
    drop_rows = sum("drop" in r for r in rows)
    file_ms = sum(r["profile"]["window_ms"] for r in rows if "file" in r)
    result = {
        "windows": len(rows), "duration_ms": ms,
        "uptime_first_ms": rows[0]["time_ms"] - rows[0]["profile"]["window_ms"],
        "uptime_last_ms": rows[-1]["time_ms"],
        "profile_line_first": rows[0]["line"], "profile_line_last": rows[-1]["line"],
        "sink_frames": total("profile", "sink_n"),
        "sink_fps": round(total("profile", "sink_n") * 1000 / ms, 6) if ms else None,
        "decode_calls": total("profile", "decode_ok"),
        "decode_avg_us": weighted("decode_avg_us", "decode_ok"),
        "decode_max_us": maximum("profile", "decode_max_us"),
        "sink_avg_us": weighted("sink_avg_us", "sink_n"),
        "sink_max_us": maximum("profile", "sink_max_us"),
        "pcm_bytes": total("pipe", "pcm_bytes"),
        "pcm_wall_ratio": round(total("pipe", "pcm_bytes") / (192 * ms), 6) if ms else None,
        "audio_gap_max_us": maximum("profile", "audio_gap_max_us"),
        "pipeline": {key: total("pipe", key) for key in
                     ("hold_n", "hold_ms", "pool_n", "pool_ms", "push_n", "push_ms",
                      "ain_n", "ain_ms", "ain_bytes", "av_skip_n", "skipped_request_ms")},
        "file": None, "predecode_drop": None,
    }
    result["pipeline"].update({key: maximum("pipe", key) for key in
                               ("hold_max_us", "pool_max_us", "push_max_us", "ain_max_us")})
    if file_rows:
        result["file"] = {"observed_windows": file_rows, "coverage_ms": file_ms,
                          **{key: total("file", key) for key in
                             ("read_n", "bytes", "read_us", "over20", "over40", "over80",
                              "read_errors", "eof", "seek_n", "seek_us", "seek_errors")},
                          "read_max_us": maximum("file", "read_max_us"),
                          "seek_max_us": maximum("file", "seek_max_us")}
        reads = result["file"]["read_n"]
        result["file"]["read_avg_us"] = round(total("file", "read_us") / reads, 3) if reads else None
    if drop_rows:
        result["predecode_drop"] = {"observed_windows": drop_rows,
                                    **{key: total("drop", key) for key in ("audio_n", "wall_n", "reanchor_n")},
                                    "audio_late_max_us": maximum("drop", "audio_late_max_us"),
                                    "wall_late_max_us": maximum("drop", "wall_late_max_us")}
    return result


def analyze_file(path: Path, startup_ms=3000, settle_ms=2500, pair_ms=100) -> dict:
    data = path.read_bytes()
    source_hash = digest(data)
    lines = data.decode("utf-8-sig", errors="replace").splitlines()
    sessions, events, rejected = [], [], []
    counts = Counter()
    current = None
    last_time = None
    pending = None

    def new_session(t, line, reason):
        nonlocal current, pending
        current = {"session": len(sessions) + 1, "start_ms": t, "start_line": line,
                   "start_reason": reason, "media_path_sha256": None, "source_info": None,
                   "events": [], "rows": [], "blocks": [], "seek_start": None,
                   "pause_start": None, "terminal_start": None}
        sessions.append(current)
        pending = None
        return current

    def event(t, line, kind, **extra):
        item = {"time_ms": t, "line": line, "kind": kind, **extra}
        events.append(item)
        if current:
            current["events"].append(item)

    for number, raw in enumerate(lines, 1):
        text = ANSI.sub("", raw)
        prefixes = list(PREFIX.finditer(text))
        if len(prefixes) != 1:
            if RECORD.search(text):
                rejected.append({"line": number, "reason": "missing_or_multiple_log_prefixes"})
            continue
        t = int(prefixes[0].group(1))
        if last_time is not None and t + 1000 < last_time:
            new_session(t, number, "uptime_reset")
            event(t, number, "uptime_reset")
        last_time = t

        opening = "Opening selected clip:" in text or "Opened local video media;" in text
        if opening:
            if current is None or current["rows"] or t - current["start_ms"] > 5000:
                new_session(t, number, "observed_open")
            # Opening-selected is logged before cleanup of the previous player.
            # The successful new open supersedes that cleanup's STOP event.
            if "Opened local video media;" in text:
                current["terminal_start"] = None
            event(t, number, "open")

        # Do not expose the user's SD directory/filename in generated reports.
        uri = re.search(r"ESP_GMF_FILE: Open,.*?uri:(.+)$", text)
        if uri:
            identity = digest(uri.group(1).strip().encode("utf-8"))
            if current is None or (current["media_path_sha256"] and
                                   current["media_path_sha256"] != identity):
                new_session(t, number, "file_open")
            current["media_path_sha256"] = identity
            event(t, number, "file_open", media_path_sha256=identity)

        source = SOURCE.search(text)
        if source:
            info = {"width": int(source[1]), "height": int(source[2]),
                    "source_fps": int(source[3]), "decoder": values(text).get("decoder", "unknown")}
            if current is None:
                new_session(t, number, "source_header")
            current["source_info"] = info
            event(t, number, "source_info", **info)

        if "SEEK_REQUEST" in text or re.search(r"Handling cmd: SEEK\b", text):
            if current and current["seek_start"] is None:
                current["seek_start"] = t
                event(t, number, "seek_request", target_ms=values(text).get("target"))
        if "SEEK_DONE" in text:
            if current:
                start = current["seek_start"]
                if start is None:
                    start = t - max(0, values(text).get("elapsed", 0)) // 1000
                current["blocks"].append((start - 500, t + settle_ms, "seek"))
                current["seek_start"] = None
            event(t, number, "seek_done", target_ms=values(text).get("target"))
        pause = bool(re.search(r"Handling cmd: PAUSE\b|State transition: .* -> PAUSED|Entering PAUSED state", text))
        resume = bool(re.search(r"Handling cmd: RESUME\b|State transition: PAUSED -> PLAYING", text))
        if pause and current and current["pause_start"] is None:
            current["pause_start"] = t
            event(t, number, "pause")
        if resume and current:
            pause_at = current["pause_start"] if current["pause_start"] is not None else t
            current["blocks"].append((pause_at - 500, t + settle_ms, "pause_resume"))
            current["pause_start"] = None
            event(t, number, "resume")
        if re.search(r"Handling cmd: STOP\b|ESP Player runtime error", text) and current:
            current["terminal_start"] = t
            event(t, number, "stop_or_error")

        match = RECORD.search(text)
        if not match:
            continue
        kind, payload = match.groups()
        counts[kind] += 1
        val = values(payload)
        if not REQUIRED[kind] <= val.keys() or any(not isinstance(val[k], int) for k in REQUIRED[kind]):
            rejected.append({"line": number, "reason": "incomplete_or_invalid_" + kind})
            continue
        if val["window_ms"] <= 0:
            rejected.append({"line": number, "reason": "nonpositive_window"})
            continue
        if kind == "MEDIA_PROFILE":
            # A captured log may begin mid-playback. Do not pretend its first
            # timestamp is a known open, or invent an A-decoder identity.
            if current is None:
                new_session(t - val["window_ms"], number, "capture_midplay_unknown")
            elif current["rows"] and t - current["rows"][-1]["time_ms"] > max(5000, val["window_ms"] * 2 + 500):
                new_session(t - val["window_ms"], number, "capture_gap_unknown")
            pending = {"time_ms": t, "line": number, "profile": val}
            current["rows"].append(pending)
        else:
            if pending is None or not 0 <= t - pending["time_ms"] <= pair_ms or val["window_ms"] != pending["profile"]["window_ms"]:
                rejected.append({"line": number, "reason": "unpaired_" + kind})
                continue
            key = kind.removeprefix("MEDIA_").lower()
            if key in pending:
                rejected.append({"line": number, "reason": "duplicate_" + kind})
                continue
            pending[key] = val

    groups = []
    for session in sessions:
        blocks = session["blocks"][:]
        for row in session["rows"]:
            pipe = row.get("pipe")
            if pipe and (pipe["state"] != 2 or pipe["seeking"]):
                blocks.append((row["time_ms"] - row["profile"]["window_ms"],
                               row["time_ms"] + settle_ms, "observed_state_transition"))
        for key, reason in (("seek_start", "seek_without_done"), ("pause_start", "pause_without_resume"),
                            ("terminal_start", "stop_or_error")):
            if session[key] is not None:
                blocks.append((session[key] - 500, float("inf"), reason))
        if session["start_reason"] in ("observed_open", "file_open", "source_header", "uptime_reset"):
            blocks.append((session["start_ms"] - 500, session["start_ms"] + startup_ms, "startup"))
        segmented = {}
        for row in session["rows"]:
            p = row["profile"]
            key = (p.get("decoder", "unknown"), p["width"], p["height"], p["source_fps"])
            segment = segmented.setdefault(key, {"rows": [], "accepted": [], "excluded": []})
            segment["rows"].append(row)
            reasons = []
            start, end = row["time_ms"] - p["window_ms"], row["time_ms"]
            reasons.extend(reason for lo, hi, reason in blocks if start < hi and end > lo)
            pipe = row.get("pipe")
            if pipe is None:
                reasons.append("missing_pipe")
            else:
                if pipe["state"] != 2: reasons.append("not_playing")
                if pipe["seeking"]: reasons.append("seeking")
                if any(pipe[k] for k in ("short", "write_errors", "push_non_ok", "ain_non_ok")):
                    reasons.append("pipeline_non_ok")
            if p["decode_non_ok"]: reasons.append("decode_non_ok")
            if p["width"] <= 0 or p["height"] <= 0: reasons.append("no_geometry")
            if any(row.get("file", {}).get(k, 0) for k in ("read_errors", "seek_errors")):
                reasons.append("file_error")
            if reasons:
                segment["excluded"].append({"line": row["line"], "reasons": sorted(set(reasons))})
            else:
                segment["accepted"].append(row)
        for (engine, width, height, fps), segment in segmented.items():
            all_rows = segment["rows"]
            groups.append({
                "id": f"{source_hash[:12]}:session-{session['session']}:{engine}:{width}x{height}:{fps}",
                "session": session["session"], "session_start_reason": session["start_reason"],
                "media_path_sha256": session["media_path_sha256"],
                "media_identity": "path_hash_only_not_content_hash" if session["media_path_sha256"] else "unknown_never_merged",
                "decoder": engine, "width": width, "height": height, "source_fps": fps,
                "observed_windows": len(all_rows), "steady": summarize(segment["accepted"]),
                "excluded": segment["excluded"],
                "all_window_errors": {
                    "decode_non_ok": sum(r["profile"]["decode_non_ok"] for r in all_rows),
                    **{k: sum(r.get("pipe", {}).get(k, 0) for r in all_rows) for k in
                       ("short", "write_errors", "push_non_ok", "ain_non_ok")},
                    **{k: (sum(r.get("file", {}).get(k, 0) for r in all_rows)
                           if any("file" in r for r in all_rows) else None)
                       for k in ("read_errors", "seek_errors")},
                },
                "events": session["events"],
            })
    return {"source_name": path.name, "sha256": source_hash, "bytes": len(data), "line_count": len(lines),
            "record_counts": dict(counts), "rejected_records": rejected, "groups": groups}


def report(paths, startup_ms=3000, settle_ms=2500, pair_ms=100):
    return {"schema_version": 1, "method": {
        "startup_ms": startup_ms, "control_settle_ms": settle_ms, "pair_tolerance_ms": pair_ms,
        "playing_state": 2, "pcm_bytes_per_ms": 192,
        "filter": "Whole windows overlapping observed startup/control intervals, non-PLAYING/seeking or non-OK calls are excluded. Missing mandatory PROFILE/PIPE is excluded. FILE/DROP absence is null, not zero.",
        "averages": "Per-call mean weighted by call counts; firmware mean truncation is retained.",
    }, "sources": [analyze_file(Path(p), startup_ms, settle_ms, pair_ms) for p in paths],
        "limitations": [
            "FPS counts frames accepted by the RGB565 sink, not optical panel refresh or unique displayed frames.",
            "Decode, sink, read and wait times are wall times including preemption; overlapping tasks cannot be summed into CPU usage.",
            "MEDIA_FILE measures scoped libc fread/fseek calls including buffering/scheduling, not raw SD bus throughput. Missing FILE records cannot diagnose SD speed.",
            "PCM/wall ratio is accepted 48 kHz stereo 16-bit output bytes, not proof of uninterrupted audible output; per-window boundary effects are possible.",
            "Adaptive predecode dropping changes the decoded frame selection. A/B means are not a pure decoder-CPU benchmark, even on the same file.",
            "Different path hashes/files/resolutions/scenes cannot establish a causal decoder comparison. A path hash identifies a path, not immutable file contents.",
            "Unobserved open, pause, seek, capture loss or file changes cannot be recovered. Unknown sessions/engines remain distinct; missing engine is never assumed to be A.",
            "Long-tail read counters are nested counts (>20, >40, >80 ms), not a latency histogram; maxima are maxima of captured windows.",
        ]}


def text_report(result):
    lines = ["Decoder comparison - retained steady windows (sink FPS, not panel FPS)"]
    for source in result["sources"]:
        lines.append(f"{source['source_name']} sha256={source['sha256']} lines={source['line_count']}")
        for group in source["groups"]:
            s = group["steady"]
            identity = (group["media_path_sha256"] or "unknown")[:12]
            label = f"  session {group['session']} {group['decoder']} {group['width']}x{group['height']}@{group['source_fps']} path={identity}"
            if not s["windows"]:
                lines.append(label + f" no steady data ({group['observed_windows']} windows excluded)")
                continue
            lines.append(label + f" {s['windows']}/{group['observed_windows']} windows, {s['duration_ms']/1000:.3f}s, sink={s['sink_fps']:.3f}fps, decode={s['decode_avg_us']}us, sink={s['sink_avg_us']}us, PCM={s['pcm_wall_ratio']:.4f}, gap_max={s['audio_gap_max_us']}us")
            f, d = s["file"], s["predecode_drop"]
            lines.append("    fread " + (f"bytes={f['bytes']} max={f['read_max_us']}us tails20/40/80={f['over20']}/{f['over40']}/{f['over80']}" if f else "unavailable"))
            lines.append("    predecode drop " + (f"audio={d['audio_n']} wall={d['wall_n']} reanchor={d['reanchor_n']}" if d else "unavailable"))
    lines.append("Limits: wall-time metrics; fread is not raw SD speed; adaptive frame selection and different files/scenes prevent a pure causal CPU A/B.")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--json", type=Path, dest="json_path")
    parser.add_argument("--text", type=Path, dest="text_path")
    parser.add_argument("--startup-ms", type=int, default=3000)
    parser.add_argument("--settle-ms", type=int, default=2500)
    args = parser.parse_args()
    if args.startup_ms < 0 or args.settle_ms < 0:
        parser.error("Filter durations must be non-negative")
    result = report(args.logs, args.startup_ms, args.settle_ms)
    text = text_report(result)
    for path, content in ((args.json_path, json.dumps(result, ensure_ascii=False, indent=2) + "\n"),
                          (args.text_path, text)):
        if path:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
    try:
        print(text, end="")
    except UnicodeEncodeError:
        sys.stdout.buffer.write(text.encode(sys.stdout.encoding or "utf-8", errors="replace"))


if __name__ == "__main__":
    main()
