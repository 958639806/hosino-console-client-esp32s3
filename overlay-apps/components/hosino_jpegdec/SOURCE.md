# JPEGDEC alternative decoder

This component adapts [bitbank2/JPEGDEC](https://github.com/bitbank2/JPEGDEC/tree/32132971bcc032b5b14f1e1e968c242d8d986854), pinned to commit `32132971bcc032b5b14f1e1e968c242d8d986854` (2026-11-09), under Apache-2.0. The upstream license and copyright notices are retained in `vendor/JPEGDEC`. `vendor-manifest.json` records original Git blob IDs and local SHA-256 digests. The portable C API is compiled from `jpeg.inl`; the upstream C++ wrapper is retained for provenance but is not compiled.

## Integration and ownership

`hosino_jpegdec_get_ops()` supplies `esp_video_dec_ops_t` for MJPEG with independent FourCC `JDEC`. The media facade registers it alongside Espressif's existing software MJPEG decoder; explicit factory selection chooses A or B for the whole playback instance. The component does not replace the AVI extractor, PCM renderer, audio ownership, storage lease, LVGL/Brookesia, status bar, gesture handling, or display driver. Unsupported JPEGs return an error; there is no silent fallback to A.

Each decoder instance owns a 16-byte-aligned PSRAM context, allocated once in `open` and freed in `close`. There is no additional full-frame allocation. Operations on a handle must be serialized by its caller. Input packets and output buffers remain caller-owned and are borrowed only for the synchronous `decode` call. The first valid header fixes stream geometry because GMF announces dimensions and allocates the pool once.

Only baseline 8-bit JPEG, one or three components, dimensions 1..480 in each axis, and RGB565 little endian are accepted. Canonical Y sampling `11`, `12`, `21`, or `22` with both chroma factors `11` is supported; grayscale requires factor `11`. Progressive, CMYK, larger images, changed in-stream dimensions, and non-unit chroma factors are rejected. In particular, FFmpeg 7.1's usual `yuvj422p` (`Y22/Cb12/Cr12`) and `yuvj444p` (`Y12/Cb12/Cr12`) packets do not match JPEGDEC's MCU assumptions. FFmpeg `yuvj420p` (`Y22/Cb11/Cr11`) is suitable for the supplied A/B fixtures. Canonical 4:2:2 and 4:4:4 are supported, so the restriction is the actual SOF sampling factors, not the format name alone. JPEGDEC also rejects compressed inputs shorter than 256 bytes.

GMF 1.1.1 allocates RGB565 output with width and height rounded up to 16 pixels. When width is divisible by 16, address is 16-byte aligned, and the actual capacity covers all padded rows, JPEGDEC writes directly into this output. Its last MCU may write below the visible height, hence all three checks are required. Otherwise a callback copies only valid row/column ranges using the callback's source stride into packed output. `decoded_size` remains visible `width * height * 2`, and timestamps are copied from input. The adapter does not perform another display byte swap.

## Local vendor changes

- An explicit `HOSINO_JPEGDEC_ESP32S3` branch enables upstream S3 color-conversion assembly in pure ESP-IDF without Arduino or ESP-DSP header probes. The 420, 444, and dequant assembly files use the same target guard; their instruction bodies are unchanged. The constants are explicitly 16-byte aligned. This selects existing S3 color conversion, not a new IDCT implementation. `s3_dequant` is provided upstream but currently unused by this core and may be removed at link time.
- Three platform guards in `jpeg.inl` admit `ESP_PLATFORM`, exposing its portable C API and file helper declarations.
- Two pointer alignment checks use `uintptr_t` and mask before narrowing, making host/target pointer widths explicit.
- One unused progressive-decoding expression uses `-(1 << n)` instead of shifting a negative value. Progressive images are rejected by the adapter before entering that path.

`jpegdec_core.c` fails target compilation if the S3 SIMD guard is missing. CMake restricts the component to `esp32s3`; no board clock, cache, PSRAM mode, or hardware setting is changed.

## Validation as of 2026-11-12

Actual Xtensa ESP32-S3 GCC 14.2.0 syntax checks of `hosino_jpegdec.c` and `jpegdec_core.c` pass without warnings using the current IDF 5.5.5 project include/configuration flags. This is not an assembly/link or device result; the formal builder must verify `s3_ycbcr_convert_420` and `s3_ycbcr_convert_444` in the linked image and then perform real A/B playback.

`tests/run_host_tests.py` executes the production adapter and actual JPEGDEC scalar C core under the existing TCC. The host shim selects the same 32-bit bitstream-reader branch as Xtensa while retaining normal host pointers. Only allocation/logging/intrinsic shims are used; no decoder is mocked. The reproducible result is `tests/host-validation-20261112.log`.

The 27 fixture checks pass: 320x180, 480x270, and 317x181; canonical 420/422/444; complex FFmpeg 420; explicit rejection of FFmpeg's non-unit chroma layouts; red/green/blue RGB565 byte order; first/middle/last original compressed packets from both 30-second A/B AVIs. Independent FFmpeg RGB565 references agree on canonical DC-tile/color fixtures within two quantized channel units. Exact-sized/misaligned callback output equals padded output byte for byte. Guard bytes, header-probe no-write, short-output no-write, oversized/progressive/malformed headers, changed geometry, EOS, and failed-open cleanup are checked.

These tests verify output geometry and core behavior, not S3 SIMD pixel accuracy, PSRAM SIMD access, firmware runtime stability, or performance. Device tests remain required. No frame-rate increase is claimed from host execution.

Example host invocation from workspace root (existing tools only):

```powershell
python overlay-apps/components/hosino_jpegdec/tests/run_host_tests.py `
  --tcc tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe `
  --ffmpeg .local/media-tools/imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v7.1.exe `
  --codec-include C:/esp/hosino-qspi/project-apps/managed_components/espressif__esp_video_codec/include `
  --work C:/esp/hosino-qspi/host-jpegdec `
  --avi firmware/media-tests/decoder-ab-320.avi `
  --avi firmware/media-tests/decoder-ab-480.avi
```
