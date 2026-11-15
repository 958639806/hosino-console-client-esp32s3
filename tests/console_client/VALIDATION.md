# Protocol test result — 2026-11-12

Executed `run_tests.py` against the production `console_protocol.c` using the workspace's existing TCC 0.9.27. No network, credentials, device, or server mutations.

Result: PASS. 43 explicit JSON cases; UTF-8 and Unicode/surrogate boundaries; embedded NUL and controls; exact token-capacity and nesting-capacity boundaries; short and malformed Cookie values; UTF-8-safe truncation; URL and JSON escaping; zeroization; failed POST outcome classification including unreadable 2xx; 10,000 deterministic bounded random parser inputs with token-buffer canaries.

Full-page regression: `fixtures/sessions-200-fictional.json` uses the 30-field per-session structure reported in `evidence/console-grok-contract-20261112.json`. All rows, UUIDs, names, and values are fictitious, not an account export. The sample's unknown `kind/status` placeholders are retained without interpreting them as valid enum values. Generate it with `python tests/console_client/generate_sessions_fixture.py`.

Actual production parser result: **147,845 bytes; 12,207 tokens; all 200 rows traversed**. It exceeds the old 128 KiB response cap and explicitly fails the old 8192-token parse. With the current **256 KiB / 16384 tokens** allocation it passes. The additional buffers use 256 KiB more PSRAM. A separate array hits exactly 16,384 tokens successfully; adding one token is rejected without overwriting the token-buffer canary. Oversized inputs remain fail-closed.

Fixture SHA-256: `a8758ba0f667b92c49d5c525bed20413006e88fdbf4a9e59df95d4a9144adaf5`.

```powershell
python tests/console_client/run_tests.py `
  --tcc tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe `
  --work C:/esp/hosino-qspi/host-console-client
```

Real Xtensa syntax-only checks passed for the latest C and C++ source against IDF 5.5.5. This test does not simulate FreeRTOS task scheduling, TLS/HTTP timing, GUI rendering or real server authentication. Those require integration/device validation.

Frozen source SHA-256:

- `hosino_console_client.cpp`: `c66f907431e1df89adeb71d5a8854432cf0d64a0a6fa09ee28f5faba4e0a0bb8`
- `console_protocol.c`: `0b4bb56d4a1f01f8abcbf4ada672401423d39a900c3cf88bad76c07b52ed89bd`
- `console_protocol.h`: `72510ed55e14fe136e4515a1901893e2618a1bdb04794dd4902da419cb912eba`
- `hosino_console_client.hpp`: `e38f696beb8a6ed18ef6ea96e7f561cc915d90e1fd99b8bec61579851ded8d85`

The final production build fixed a compiler `format-truncation` error in CreateSession by using the existing 1536-byte escaped-name scratch buffer instead of the 6144-byte message scratch buffer. The 120-byte name limit is unchanged; no truncation or warning suppression was introduced. The final C++ hash above includes this fix and passed the complete ESP-IDF build. Protocol C/header code and the host-test results are unchanged.
