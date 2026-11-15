# Source and local port

Upstream: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/tree/78e13f852929c2ab4f9d5e0ad1c50ea378dbf2b4/firmware/brookesia/components/Gallery

Pinned revision: `78e13f852929c2ab4f9d5e0ad1c50ea378dbf2b4`.

License: Apache-2.0. Original copyright/SPDX notices and assets are retained. Repository license is in the matching pinned checkout under `upstream/`.

Local changes: Chinese status/error strings and fonts; app-local geometry; caption inset 40 px from bottom and sides; preserve gesture bubbling. Keeps upstream JPEG worker and launcher artwork.

Only the application/service component is reused. The current 2.16 display/touch BSP, Brookesia core, refresh configuration and system gestures are not replaced.

Validation: source review only in this component task; complete firmware build and physical-board verification are performed by the integrating task.
