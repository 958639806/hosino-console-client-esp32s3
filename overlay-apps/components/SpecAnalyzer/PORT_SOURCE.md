# Source and local port

Upstream: https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/tree/e4344e70c2fa78a13e8a06566507f1ba8af6672a/firmware/brookesia/components/SpecAnalyzer

Pinned revision: `e4344e70c2fa78a13e8a06566507f1ba8af6672a`. Repository license: Apache-2.0. Original app artwork is retained.

Local changes: Chinese launcher name and status text with hosino_ui_fonts; Phone status bar enabled by default; 400x240 RGB565 canvas stays 40 pixels inside both sides and 68 pixels above the app bottom. ES7210 reads the 2.16 board's STD stereo MIC1/MIC2 at channel indices 0/1, using shared bsp_extra ownership and CODEC_VOICE_INPUT_CHANNELS=2. This replaces the donor's four-slot reference-channel assumptions and associated debug-array reads. ESP-DSP is pinned at 1.8.0.

The official worker keeps microphone capture/FFT off the GUI task. GUI snapshots remain protected by the spectrum mutex. Pause requires codec-release acknowledgement; failure preserves the page and resources. Close joins the worker before destroying the canvas and allows the singleton to reopen. A live worker is never force-deleted. The join budget is 2500 ms to cover the codec layer's separate 1000 ms lock/read waits.

Validation: source review only here; full build and physical microphone/app-switch checks belong to the integration task.
