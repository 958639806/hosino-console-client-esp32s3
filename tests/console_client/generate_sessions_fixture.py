"""Generate fictitious 200-session data from the reported 30-field contract.

Every ID/name/value is a test example, not an account export. The placeholder
kind/status strings in Grok's example are preserved without treating them as
real enum values. No HTTP calls are made.
"""
import copy
import json
from pathlib import Path

root = Path(__file__).resolve().parents[2]
contract = json.loads((root / "evidence/console-grok-contract-20261112.json").read_text(encoding="utf-8-sig"))
sample = contract["sessions_example"]
template = sample["sessions"][0]
assert len(template) == 30, "Re-review the reported contract if its fields change"
fixture = {"totalCount": 200, "defaults": copy.deepcopy(sample["defaults"]), "sessions": []}
for index in range(1, 201):
    session = copy.deepcopy(template)
    session["key"] = f"agent:main:dashboard:00000000-0000-4000-8000-{index:012x}"
    session["sessionId"] = f"10000000-0000-4000-8000-{index:012x}"
    for name in ("displayName", "gatewayDisplayName", "label"):
        session[name] = f"fictitious-ESP32-device-{index:03}"
    fixture["sessions"].append(session)
directory = Path(__file__).with_name("fixtures")
directory.mkdir(exist_ok=True)
output = directory / "sessions-200-fictional.json"
output.write_text(json.dumps(fixture, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
print(f"Generated fictitious page: {output.stat().st_size} bytes, 200 sessions, 30 fields/session")
