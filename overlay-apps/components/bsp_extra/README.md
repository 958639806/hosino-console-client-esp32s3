# ESP32-S3-Touch-AMOLED-2.16 audio adapter

See [Chinese implementation notes](README_ZH.md). This port uses two-channel STD input, not the donor board four-slot TDM setup. Source: Waveshare 1.75 factory firmware e4344e70c2fa78a13e8a06566507f1ba8af6672a; Apache-2.0. The original display/touch BSP remains unchanged. Audio resources have a separate adapter owner; apps serialize complete audio sessions. Do not mix with bsp_audio_init from the display BSP.
