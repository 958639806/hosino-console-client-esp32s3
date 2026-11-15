# Source and local port

Upstream: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/tree/78e13f852929c2ab4f9d5e0ad1c50ea378dbf2b4/firmware/brookesia/components/draw

Pinned revision: `78e13f852929c2ab4f9d5e0ad1c50ea378dbf2b4`.

License: Apache-2.0. Original copyright/SPDX notices and assets are retained. Repository license is in the matching pinned checkout under `upstream/`.

Local changes: Chinese launcher name and clear button; preserve gesture bubbling; bound the original dot-object drawing history to 600 objects. Keeps upstream launcher artwork.

Only the application/service component is reused. The current 2.16 display/touch BSP, Brookesia core, refresh configuration and system gestures are not replaced.

Validation: source review only in this component task; complete firmware build and physical-board verification are performed by the integrating task.
