# GMF IO compatibility override

Upstream: Espressif registry `espressif/gmf_io` 1.1.0, commit
`17ce627cd547d94acfc14890807dbccdc8b459c6`, path `elements/gmf_io`.
Registry: https://components.espressif.com/components/espressif/gmf_io/versions/1.1.0

Original SPDX notices and the upstream
`LicenseRef-Espressif-Modified-MIT` LICENSE are retained.
`idf_component.yml` pins `espressif/esp_codec_dev` to the board's existing
1.5.11 instead of the upstream 2.0.0-beta1 range. The codec IO source only uses
`esp_codec_dev_handle_t`, `esp_codec_dev_read` and `esp_codec_dev_write`, all
present with the same signatures in 1.5.11. The app keeps BSP audio ownership.
Tests/examples and registry checksum metadata are omitted from this local override.

The original package archive SHA-256 and retrieval URL are recorded in
`upstream/esp-player-registry-v1.1.1/gmf_io/REGISTRY_SOURCE.json`.
Full target compilation and device playback remain separate validation steps.

## qspi-apps-11 scoped file diagnostics

`esp_gmf_io_file.c` adds a per-reader session-generation token and optional
`hosino_file_diagnostics_bind` / `hosino_file_diagnostics_record` hooks. The public
header has no ESP Player dependency; the application supplies strong providers.
A reader binds its exact mounted pathname once after successful `fopen`; a zero
token bypasses timing. The provider invalidates stale session tokens on stop or
reopen, including when the pathname is reused. Writers are never bound.

Each original `fread` and `fseek` call retains its arguments, byte/result return,
EOF/error state and post-call `errno`. Initial resume positioning and reset seeks
are included. Hook failures cannot replace the original IO result. `esp_timer`
is a private CMake dependency. The measurements are elapsed stdio/file-system
wall time, including cache and scheduler effects, not pure SD hardware latency.
Buffer sizes, stdio cache settings, frequencies, IO-task configuration and the
upstream short-read/EOF policy remain unchanged. `fopen` itself is not timed.

The new diagnostic header is Apache-2.0. Original/local hashes for all three
modified upstream files and the new header are in `HOSINO_PROVENANCE.json`.
Real Xtensa syntax checking passed; final build and device measurements remain
separate validation steps.
