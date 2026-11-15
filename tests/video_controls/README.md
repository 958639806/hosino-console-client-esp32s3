# Video display scale checks

`test_scale.c` includes the production `video_view_math.h` without a separate implementation. It checks contain/cover bounds, default and invalid dimensions, integer-rounding behavior, and the explicit 16x cap across all 480x372 supported source sizes: 178,569 cases.

The existing local TCC compiler executed this test successfully. Actual Xtensa C++ and C syntax checks for VideoPlayer and its media facade also passed without diagnostics. Results and source hashes: `evidence/video-controls-syntax-20261112.json`.

These are host and syntax checks, not device playback, LVGL allocation-peak or touch-layout acceptance. The new controls use existing frame buffers; LVGL scaled rendering may still need transient working memory.

## RGB565 sink scaling follow-up

`test_rgb565.c` links the actual production `video_rgb565_scale.c`. It verifies 3,099 source/mode combinations against direct rational pixel-coordinate calculations, both bytes of each pixel, byte-unaligned input, source/destination row padding, untouched guard bytes, rejected undersized buffers and native packed memcpy. It does not use a second copy of the production incremental sampler.

Both tests and actual Xtensa syntax checks passed: `evidence/video-fill-syntax-20261112.{json,log}`. The new sink outputs an already resized/cropped opaque frame, keeping LVGL at 1:1. Display buffers can grow in PSRAM but their count stays two. Device performance remains unverified by these host checks.

## v18 bounded internal row scratch

The actual production scaler still passes all 3,099 pixel cases. Additional checks reject missing scratch and source/output widths over 480 before writing destination bytes, and assert the scratch allocation is exactly 2,880 bytes. Native 1:1 copies accept a null scratch pointer. Pixel sampling now runs on two internal rows plus a precomputed x-index table; repeated vertical rows are copied from the internal output row. No full extra image is allocated.

`evidence/video-v18-syntax-20261112.json` records both real Xtensa translation units with zero diagnostics. `evidence/video-v18-source-check-20261112.json` contains v17 measured baseline windows and v18 source hashes. This verifies pixel correctness and source compatibility, not device FPS/audio improvement.
