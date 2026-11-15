# Source and local port

Upstream: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/tree/e4344e70c2fa78a13e8a06566507f1ba8af6672a/firmware/brookesia/components/Settings

Pinned revision: `e4344e70c2fa78a13e8a06566507f1ba8af6672a`.

License: Apache-2.0. Original copyright/SPDX notices and assets are retained. Repository license is in the matching pinned checkout under `upstream/`.

Local changes: Chinese labels/fonts; rounded-rectangle 40 px content margin and bottom margin; preserve Phone status bar through the normal app constructor. Remove AI chat-history UI, service dependency and diagnostics field. Use shared native Wi-Fi initialization; system_status exclusively owns SNTP. Keeps upstream artwork and page services.

Only the application/service component is reused. The current 2.16 display/touch BSP, Brookesia core, refresh configuration and system gestures are not replaced.

Validation: source review only in this component task; complete firmware build and physical-board verification are performed by the integrating task.

Lifecycle fix: bind navigation to the current Settings screen and foreground session. Cancel pending navigation on pause/close/deinit; all six child header returns use the same guarded asynchronous entry point. Close the previous child before replacing its page. This prevents a deferred Settings navigation from clearing another application or desktop screen. Verified against the current Brookesia resume order (loadRecentScreen before app resume) and LVGL 9 async cancellation implementation. No physical-device validation in this component task.
