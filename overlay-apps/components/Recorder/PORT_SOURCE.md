# Source and local port

Upstream: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/tree/e4344e70c2fa78a13e8a06566507f1ba8af6672a/firmware/brookesia/components/Recorder

Pinned revision: `e4344e70c2fa78a13e8a06566507f1ba8af6672a`. License: Apache-2.0. Original SPDX notices and icon are retained.

Local changes: Chinese title, controls and statuses; use hosino_ui_fonts; retain Phone status bar by default; place text inside the app visual area with safe side/bottom insets. Adapt the 1.75 four-slot input to the 2.16 board's confirmed ES7210 STD stereo stream: channels 0 and 1, CODEC_VOICE_INPUT_CHANNELS=2, 24 kHz/16-bit. Shared bsp_extra owns the board codec and capture masks. Pause the UI timer only after recording stops; retain the UI if a close request times out.

Recording is user-triggered and saved under `/sdcard/Waveshare/Recordings`. The inherited worker uses exclusive file creation, five-second header checkpoints, `.wav.partial` recovery files, and publishes `.wav` only after a synchronized close. No SD format operation is added. Backgrounding stops and saves capture; it does not secretly continue recording.

Validation: source review only here. Complete firmware build and board capture/playback, no-card, card-removal, and app-switch checks remain integration responsibilities.
