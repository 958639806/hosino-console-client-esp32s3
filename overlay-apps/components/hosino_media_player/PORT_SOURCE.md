# ESP Player bridge

This component is original Apache-2.0 integration code. It consumes the fixed
Espressif `esp_player` 1.1.1, `esp_audio_render` 1.1.0 and
`esp_video_render` 1.1.0 packages. Since v7, the player is a project-local copy
whose only upstream implementation change reduces decoded frame blocks from
four to two; see `../esp_player_compat/esp_player/HOSINO_SOURCE.md` and its
hash provenance. The two-block pool remains unchanged in v8. Vendor packages retain their original
`LicenseRef-Espressif-Modified-MIT` license and LICENSE files; this bridge does
not relicense them. Pool registration follows the public API and the Apache-2.0
`esp_player/test_apps/main/render_common.c` integration example.

The board display driver, LVGL buffers and Brookesia gesture/status layers are
not part of this component. The application owns LVGL and copies borrowed
RGB565 little-endian frames into its own presentation buffers. A single
uncached stream uses synchronous composition. The custom decoder factory is
only a geometry/size guard and installs the display backend before decoding;
it returns NOT_SUPPORT to select the official built-in decoder. It does not
supply a replacement decoder. Unaligned source geometry may use the official
crop path and one lazy, source-size PSRAM composition buffer. The common
320x240 path passes the decoded frame directly to the application's copy callback.
No artificial 10-FPS limiter is present. `render_fps` is the vendor compositor
cadence setting; the single-stream path follows source FPS/PTS and A/V sync.

Audio uses the existing exclusive board session and I2S API. The official GMF
channel, bit-depth and sample-rate conversion elements produce 48 kHz stereo
16-bit output. User volume is retained. This conversion supports differing
source formats without writing 24-bit or mono bytes to a 16-bit stereo bus.

All control calls and `poll` run on the application's control worker. The GUI
only reads a cached coherent snapshot. No extra facade worker is allocated.
Vendor task defaults (including 5120-byte external-stack pipeline tasks) remain
unchanged. The facade state/path and fallback image reside in PSRAM, while its
small shared spinlock stays in internal static memory.

Playback acquires its own storage lease. Stop first stops and deinitializes the
player (including its file IO), then destroys renderers, confirms callbacks are
quiet, stops board audio and finally releases the audio owner and storage lease.
Any uncertain failed stage keeps its remaining handles/ownership for retry.
Neither stop nor deinit has a facade-enforced deadline: vendor APIs may wait
indefinitely. Applications must never forcibly delete their worker/active handle.
`join(timeout_ms)` only observes that cleanup already completed; it does not
bound a blocking vendor stop.

The vendor player currently discards the return from video stream_write.
Therefore callback/allocation failures are separately latched; the next worker
poll stops playback and keeps the actual error in the snapshot. A callback may
return ESP_OK when deliberately dropping a presentation frame. `video_frames`
is delivered-callback count, not LCD FPS or total decoder frames.

## v8 seek completion and diagnostics

Seek acceptance is not seek completion. The facade publishes `snapshot.seeking`
and the target before calling the vendor API, then clears it on `SEEK_DONE` or
terminal/error events. A position epoch prevents an older concurrent position
query from overwriting the completed target. The application merges new seek
intent while a previous seek is active; the final completion log copies its
target and start time while holding the snapshot lock.

Worker polling treats a seek still pending after 15 seconds as a latched error
and requests normal stop instead of admitting another seek into an uncertain
pipeline. This is an error-containment threshold, not a guaranteed deadline for
a vendor API or stop call: ownership is retained until cleanup really succeeds.
No active task or borrowed buffer is forcibly freed.

`media_profile.c` wraps `esp_video_dec_process` at link time and calls the real
decoder unchanged. Roughly every two seconds the video session logs aggregate
decode, video-copy callback and audio-write elapsed times plus audio-write gaps.
These are wall-clock microseconds, including preemption, waits and normal I2S
pacing; they are not CPU utilization or proof of audio starvation. Initial
decoder sizing/header probes may contribute to the non-OK call count without
being fatal playback errors. Profiling adds no per-frame log or allocation.

The first v6 device playback probe exposed missing provider registration, which
was corrected by registering extractor/audio/simple-audio/video defaults once.
The accepted v6 short tests are archived in `evidence/apps-v6-accepted`.
V7 could play 480-wide video and show battery percentage, but users reported
seek/audio stutter; its capture ended on serial disconnect before 300 seconds,
with a final device timestamp of 178542 ms (not capture duration). The log and exact staged sources are
archived in `evidence/apps-v7-observed`. V8 is a repair/diagnostic revision, not
a demonstrated performance improvement. Its build and device evidence must be
checked separately; `evidence/apps-validation.json` records the current device.

V8 production build and artifact checks passed: `evidence/apps-v8-build-check.json`.
Final-ELF disassembly confirms three GMF indirect calls to the wrapper and the
wrapper call to the unchanged real decoder; see `evidence/apps-v8-decoder-wrap-calls.json`.
The build excludes embedded samples. Device seek/audio results remain pending.
