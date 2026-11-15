"""Host validation tests only; never opens a serial port or fabricates an ACK."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[4]
spec=importlib.util.spec_from_file_location("provision_bili",ROOT/"scripts/Provision-Bili.py")
tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)

class HostConfigTests(unittest.TestCase):
    def test_config_and_command(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"config.json"
            value={"base_url":"https://101.42.20.91/bili///","device_token":"T"*43}
            path.write_text(json.dumps(value),"utf-8");os.chmod(path,0o600)
            config=tool.load_config(path)
            self.assertEqual(config["base_url"],"https://101.42.20.91/bili")
            payload=tool.command(config,"0123456789abcdef")
            self.assertLess(len(payload),768)
            self.assertEqual(json.loads(payload[len(b"BILI_CONFIG "):]),{**config,"request_id":"0123456789abcdef"})
    def test_ack_correlation(self):
        request_id="0123456789abcdef"
        self.assertIsNone(tool.matching_ack(b"BILI_CONFIG_OK",request_id))
        self.assertIsNone(tool.matching_ack(b"BILI_CONFIG_OK fedcba9876543210",request_id))
        self.assertIsNone(tool.matching_ack(b"BILI_CONFIG_ERROR_INVALID fedcba9876543210",request_id))
        self.assertIsNone(tool.matching_ack(b"BILI_CONFIG_OK 0123456789abcdef",request_id,sync=True))
        self.assertEqual(tool.matching_ack(b"BILI_SYNC_OK 0123456789abcdef",request_id,sync=True),"ok")
        self.assertEqual(tool.matching_ack(b"BILI_CONFIG_OK 0123456789abcdef",request_id),"ok")
        self.assertEqual(tool.matching_ack(b"BILI_CONFIG_ERROR_INVALID 0123456789abcdef",request_id),"BILI_CONFIG_ERROR_INVALID")
        with self.assertRaises(ValueError):tool.command({"base_url":"https://host","device_token":"T"*43},"bad")
    def test_bad_values(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"config.json"
            for base,token in (("http://host/bili","T"*43),("https://u@host/bili","T"*43),
                    ("https://host/a/../b","T"*43),("https://host/%2e","T"*43),
                    ("https://host/bili","T"*23),("https://host/bili","T"*40+"\r\nX")):
                path.write_text(json.dumps({"base_url":base,"device_token":token}),"utf-8");os.chmod(path,0o600)
                with self.assertRaises(ValueError):tool.load_config(path)

if __name__=="__main__":unittest.main()
