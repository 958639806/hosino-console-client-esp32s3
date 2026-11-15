# Source and local port

Upstream: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/tree/e4344e70c2fa78a13e8a06566507f1ba8af6672a/firmware/brookesia/components/system_status

Pinned revision: `e4344e70c2fa78a13e8a06566507f1ba8af6672a`.

License: Apache-2.0. Original copyright/SPDX notices and assets are retained. Repository license is in the matching pinned checkout under `upstream/`.

Local changes: Use the ESP32-S3-Touch-AMOLED-2.16 BSP. Remove every AXP2101 configuration-write helper and charger-profile update; use only register reads. Expose init_wifi_stack() so native Wi-Fi can initialize before the launcher allocates internal RAM. Own the single SNTP client and start it after DHCP; expose is_time_synchronized().

The Apps-4 telemetry snapshot adds actual VBAT/VBUS/VSYS ADC readings, PMU die
temperature and read-only charger/input-limit configuration. Register mappings
come from the pinned 2.16 checkout's `examples/arduino/libraries/XPowersLib/src/`
(`XPowersAXP2101.tpp`, `XPowersParams.hpp`, `REG/AXP2101Constants.h`) and its
`datasheet/AXP2101_Datasheet_V1.0_en.pdf`, PDF pages 39–40 and 48.
VBAT uses the datasheet's six high data bits, correcting the bundled library's
five-bit mask. ADC REG30 bit 5 is an independent general-purpose channel, not
a master switch. Disabled channels, failed reads, absent sources and reserved
configuration encodings remain unavailable. Measurements do not reuse stale
values. No ADC, charge-current, charge-voltage, or power-register writes occur.

`disconnect_wifi(forget_saved)` pauses automatic reconnect for the current
session, while retaining the Wi-Fi enabled state. Both event-triggered and
monitor-triggered retries obey the pause. Explicit connection or toggling
Wi-Fi off and on clears it. Forgetting clears only the STA configuration via
`esp_wifi_set_config`; it does not erase the NVS partition. Errors propagate to
the caller, and a failed manual disconnect/forget remains paused until the
user explicitly connects again.

Only the application/service component is reused. The current 2.16 display/touch BSP, Brookesia core, refresh configuration and system gestures are not replaced.

Validation: source review and ESP32-S3 compiler syntax checks for this service
and the consuming Power/About pages passed. Full firmware linking and
physical-board readings/reconnect behavior are validated by the integrating
task separately.
