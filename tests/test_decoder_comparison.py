"""Focused parser regressions; no firmware/device or performance assertions."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("comparison", ROOT / "scripts/analyze_decoder_comparison.py")
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def line(t, payload, tag="hosino_media"):
    return f"I ({t}) {tag}: {payload}\n"


def window(t, engine="A-ESPRESSIF", state=2, seeking=0, decode_error=0, read_error=0, optional=True):
    text = line(t, f"MEDIA_PROFILE window_ms=2000 width=480 height=270 source_fps=23 decoder={engine} "
                   f"decode_ok=30 decode_non_ok={decode_error} decode_avg_us=40000 decode_max_us=50000 "
                   "sink_n=28 sink_avg_us=10000 sink_max_us=12000 audio_gap_max_us=60000")
    text += line(t + 1, f"MEDIA_PIPE window_ms=2000 state={state} seeking={seeking} pcm_bytes=192000 "
                       "short=0 write_errors=0 push_non_ok=0 ain_non_ok=0")
    if optional:
        text += line(t + 2, "MEDIA_DROP window_ms=2000 audio_n=2 audio_late_max_us=90000 "
                           "wall_n=3 wall_late_max_us=100000 reanchor_n=1")
        text += line(t + 3, f"MEDIA_FILE window_ms=2000 read_n=10 bytes=1234 read_us=30000 read_max_us=25000 "
                           f"over20=1 over40=0 over80=0 read_errors={read_error} seek_n=1 seek_errors=0")
    return text


class ParserTest(unittest.TestCase):
    def analyze(self, text):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "capture.log"
            path.write_text(text, encoding="utf-8")
            return comparison.analyze_file(path)

    def test_startup_seek_pause_errors_and_private_identity(self):
        text = line(0, "Opened local video media; exclusive audio=1")
        text += line(1, "Open, dir:1, uri:/sdcard/private title.avi", "ESP_GMF_FILE")
        text += window(2000) + window(4000) + window(6000)
        text += line(6100, "SEEK_REQUEST target=10000 ms")
        text += line(6500, "SEEK_DONE target=10000 ms elapsed=400000 us")
        text += window(8000) + window(10000) + window(12000)
        text += line(12900, "Handling cmd: PAUSE in state: PLAYING", "ESP_PLAYER_STATE")
        text += window(14000, state=3)
        text += line(15000, "Handling cmd: RESUME in state: PAUSED", "ESP_PLAYER_STATE")
        text += window(16000) + window(18000) + window(20000)
        text += window(22000, read_error=1) + window(24000, decode_error=1)
        result = self.analyze(text)
        group = result["groups"][0]
        self.assertEqual(group["steady"]["windows"], 2)  # 12000, 20000; seek lead-in excludes6000
        self.assertEqual(group["steady"]["sink_fps"], 14)
        self.assertEqual(group["steady"]["pcm_wall_ratio"], .5)
        self.assertEqual(group["steady"]["file"]["bytes"], 2468)
        self.assertEqual(group["steady"]["predecode_drop"]["wall_n"], 6)
        self.assertEqual(group["all_window_errors"]["read_errors"], 1)
        self.assertEqual(group["all_window_errors"]["decode_non_ok"], 1)
        self.assertNotIn("private title", str(result))
        self.assertEqual(len(group["media_path_sha256"]), 64)

    def test_sessions_engines_missing_optional_and_corrupt_lines(self):
        text = window(2000, optional=False).replace(" decoder=A-ESPRESSIF", "")
        text += line(3000, "Opened local video media; exclusive audio=1")
        text += line(3001, "Open, dir:1, uri:/sdcard/file2.avi", "ESP_GMF_FILE")
        text += window(6000, engine="B-JPEGDEC") + window(8000, engine="B-JPEGDEC")
        text += "I (9000) hosino_media: MEDIA_PROFILE window_ms=2000 I (9001) bad: joined\n"
        result = self.analyze(text)
        self.assertEqual(len(result["groups"]), 2)
        old, new = result["groups"]
        self.assertEqual(old["decoder"], "unknown")
        self.assertIsNone(old["steady"]["file"])
        self.assertIsNone(old["steady"]["predecode_drop"])
        self.assertEqual(new["decoder"], "B-JPEGDEC")
        self.assertNotEqual(old["id"], new["id"])
        self.assertEqual(result["rejected_records"][0]["reason"], "missing_or_multiple_log_prefixes")

    def test_previous_cleanup_stop_does_not_poison_new_open(self):
        text = line(0, "Opening selected clip:", "VideoPlayer")
        text += line(50, "Handling cmd: STOP in state: PLAYING", "ESP_PLAYER_STATE")
        text += line(100, "Opened local video media; exclusive audio=1")
        text += window(6000)
        self.assertEqual(self.analyze(text)["groups"][0]["steady"]["windows"], 1)


if __name__ == "__main__":
    unittest.main()
