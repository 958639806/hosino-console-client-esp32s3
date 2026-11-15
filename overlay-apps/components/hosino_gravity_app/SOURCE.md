# 重力小球来源与移植说明

应用外观、图标、单球运动、水平校准及矩形边界来自 Waveshare 同系列板官方 Brookesia 应用：

- Repository: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8
- Local verified revision: `78e13f852929c2ab4f9d5e0ad1c50ea378dbf2b4`
- Path: `firmware/brookesia/components/Gravitysphere`
- Upstream copyright: 2023–2026 Espressif Systems (Shanghai) CO LTD.
- License: Apache-2.0; full license retained in `LICENSE`.
- `assets/img_app_qmi8658ball.c` is copied unchanged from the donor.

The target board's sensor axis mapping and 8G/500Hz accelerometer setup follow:

- Repository: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16
- Local verified revision: `225a62bff11b5d0a0b607873860d39485a9a9685`
- Path: `examples/esp-idf/04_Immersive_block/main/main.c`
- License: Apache-2.0.

## Exact sensor dependency

`waveshare/qmi8658` is pinned to `2.0.1`, not a floating or caret constraint.
The 2.0.1 header and implementation were inspected from the official registry
archive before implementation. The package uses 1000 ms I2C transaction timeouts,
leaves its device handle registered after some initialization failures, and has
no deinit API. Accordingly, its private device handle is explicitly removed;
the BSP's shared bus is never reset or deleted.

- Package: https://components.espressif.com/components/waveshare/qmi8658/versions/2.0.1/readme
- License: https://components.espressif.com/components/waveshare/qmi8658/versions/2.0.1/license
- Source revision linked by registry: `73da778575969178e3898bee5b516be4fa8ec414`
- Package license: Apache-2.0.

## Local modifications

This is a Chinese Brookesia application, not another firmware entry point.
It keeps the existing display driver, 80 MHz clock, shared I2C bus, BOOT button,
status bar, and system gestures. The private arena leaves the bottom 68 pixels
of the app visual area free for the existing HUD and gesture region.

All QMI8658 probe, setup, calibration, sampling, disable, and handle removal
operations run in a worker task. No sensor operation or worker join runs in
the LVGL task. The worker never touches LVGL or the App object: its independent
reference-counted state contains a one-item sample queue and atomic controls.
Closing an application stops its timer, requests worker cancellation, and drops
the GUI's state reference immediately. The worker releases its own device/queue
and C++ state before deleting itself. A process-local sensor lease serializes
old-worker cleanup and rapid reopening. Pausing disables sampling after the
current transaction completes; an epoch prevents old samples from being drawn
after resume. The recorded GUI timer is deleted only by Brookesia.

The donor's 200-sample, five-attempt calibration uses real acceleration only;
the last valid sample set is retained when unstable, with an explicit Chinese
notice. Missing sensors and read/allocation failures remain visible and the
desktop back/home gesture stays available. No simulated motion is generated.

Hardware axis direction, physical calibration, error recovery, and repeated
open/pause/close remain subject to actual-board verification. This component
does not claim those checks have run merely because it compiles.
