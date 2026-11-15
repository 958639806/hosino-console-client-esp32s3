# Source and local port

Upstream: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/tree/e4344e70c2fa78a13e8a06566507f1ba8af6672a/firmware/brookesia/components/storage_service

Pinned revision: `e4344e70c2fa78a13e8a06566507f1ba8af6672a`.

License: Apache-2.0. Original copyright/SPDX notices and assets are retained. Repository license is in the matching pinned checkout under `upstream/`.

Local changes: Use the ESP32-S3-Touch-AMOLED-2.16 BSP. Preserve mount leases, card-removal recovery, safe eject and non-destructive temporary-file benchmark. No formatting API is used. Build must keep CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL disabled. Board compatibility supplies bsp_sdcard_unmount().

Only the application/service component is reused. The current 2.16 display/touch BSP, Brookesia core, refresh configuration and system gestures are not replaced.

Validation: source review only in this component task; complete firmware build and physical-board verification are performed by the integrating task.
