# ESP Player memory and A/V latency overrides for the S3 board

For the managed Bili HTTPS-FLV path, an explicit selected bridge profile can
fill zero extractor geometry/FPS before the custom decoder factory runs. The
only admitted profiles are320x180/20 and480x270/15; nonzero conflicting geometry
is rejected, and ordinary file/other network tracks do not use the fallback.
Actual decoder dimensions are checked before decoder-output block allocation:
only the selected visible size and its8/16-pixel codec alignment are admitted.
The renderer keeps the selected visible rectangle when the native extractor
still reports zero. This targets the first device FLV factory rejection; host
guard and Xtensa syntax checks do not establish subsequent hardware playback.

Based on the exact Espressif `esp_player` 1.1.1 registry component already used
by Apps v6. Original license, SPDX notices and implementation are retained.
`HOSINO_PROVENANCE.json` records upstream version/content hash and the exact
changed source hashes. Registry checksum metadata is omitted for this local copy.

`ESP_PLAYER_VIDEO_BLOCK_NUM` changes from 4 to 2. This changes the decoded
video ring size and its corresponding metadata queue depth. The official public
buffer API tunes compressed demux/network buffers, not decoded frame count.
This override is project-local; the global ESP-IDF installation and downloaded
registry component are not patched.

The application uses one synchronous, uncached MJPEG stream. The decoder holds
one output frame while rendering holds one; the render callback copies into
independent LVGL buffers before returning. No callback retains a vendor frame.
Pause, seeking, repeated open/close and higher-resolution playback still require
device validation. Two blocks can have less scheduling tolerance than four.

For 480x270 RGB565, the vendor allocates 480x272-aligned slots: two decoded slots
consume 522,240 bytes. The application's two visible buffers consume 518,400
bytes, total 1,040,640 before decoder workspace, compressed buffers and UI.

For the Apps v9 A/B experiment, `src/audio/player_pipeline_audio.c` selects a
20 ms audio render input block only when video is enabled and an active video
track exists. Both initial creation and reused-renderer setup apply the same
selection. Audio-only playback, including FLAC music, retains the upstream
100 ms block. `AUDIO_RENDER_CHUNK` logs the actual source sample rate, channels,
bit depth, requested frame bytes and calculated ring size at setup, not per frame.
The ring capacity limits, 400 ms nominal ring sizing constant,
task priorities/affinities and hardware clocks are unchanged. Extractor pacing
was unchanged in v9; the following v10 experiment makes one scoped exception.

This targets the upstream ring reader's accumulation of a complete render input
block before it can write audio. It is a single-variable latency experiment,
not a confirmed diagnosis or claimed performance improvement. The changed C
translation unit passed a real Xtensa syntax check; full firmware build and
device A/B validation are pending at the time of this change. Compare the same
media position before/after seeking and sustained 480x270 playback using the
existing `MEDIA_PROFILE` audio-gap/FPS data and audible continuity. Ring duration
must be calculated from the logged source format, not the fixed 48 kHz output.

For Apps v10, `src/video/player_ports_video.c` skips the upstream 0.7-frame
post-video sleep only when the selected audio track is valid, its compressed
queue exists and is empty, the A/V mask is enabled, and extractor-mode input
is an absolute local VFS path or `file://` URI. The existing seek guard remains.
Pure-video pacing, HTTP(S)/other nonlocal sources, the bounded video queue,
the extractor's 10 ms audio-fairness hold and 10 ms pool-full wait are retained.
This lets a local demuxer read the following audio packets without that extra
sleep; whether it fixes the observed intermittent stutter requires device data.

`include/hosino_player_diagnostics.h` is a new optional weak-hook contract.
The video output port reports skipped/local and retained video-only/network
yields plus the video queue-push duration. `src/extractor/player_extractor.c`
reports the two retained waits, and `src/audio/player_audio_render.c` reports
input-acquire duration, successful bytes and the I/O result. The application
aggregates these with its existing profiler; vendor hooks allocate nothing,
create no tasks and emit no per-frame logs. All durations include preemption.
Wait-event `amount` is requested microseconds; push/acquire `amount` is bytes.
Seek aborts remain distinguishable I/O results and are not automatically errors.
The provider defines `HOSINO_PLAYER_DIAGNOSTICS_PROVIDER` before including the
header so its implementation is strong; absent providers are safely skipped.

The four touched audio/video C translation units passed real Xtensa syntax
checks. Apps v10 full build, flashing and playback validation are pending at
the time of this source change. Compare identical media/seek positions using
2-second aggregate counts/time, output PCM bytes, frame rate and audible gaps;
a skipped-yield count alone does not prove it caused every prior audio gap.

For Apps v11, local-file MJPEG A/V at normal speed gains an audio-first
predecode policy. It releases an already-late compressed MJPEG frame through
the existing input-release path, before spending CPU on JPEG decoding. It does
not discard PCM or change H264, network, pure-video or audio-only pacing.
The generic AUDIO decode-sync function now records its first/zero timestamp,
while retaining its prior accept behavior for callers outside this policy.

The new clock is anchored only after the audio renderer successfully accepts
a PCM block, using that block's start PTS and a monotonic timestamp. It is not
a hardware DAC sample counter and does not invent an I2S latency correction.
Lateness uses both the latest accepted audio PTS and elapsed wall time from
that anchor, so slow audio input cannot conceal an overloaded video decoder.
The tolerance is the larger of two video frames and 60 ms. No valid audio
anchor means no new-policy dropping; the first frame after an anchor is kept.

Pause, seek, mode/speed changes, reset and audio EOS invalidate the generation.
An audio block acquired before invalidation cannot establish a new anchor
after it completes. Resume requires fresh accepted output. The policy is
disabled outside normal speed, during pause/seek/stop, or without a valid local
MJPEG A/V source. Timestamp discontinuities, audio output stalls over one
second or catch-up over one second retain a frame and await a fresh anchor.
Every eight consecutive predecode drops yield one RTOS tick. Existing bounded
queues, stop checks, EOS handling, ring depth, priorities and clocks remain.

`src/sync/private_inc/hosino_mjpeg_clock.h` contains the deterministic clock
helper used by production code. `tests/mjpeg_clock/run_tests.py` compiles both
this helper and the complete production `player_sync.c` with clock/RTOS stubs.
Both tests pass: zero/no-anchor, wall/audio lateness, discontinuities, stale
generation rejection, pause/resume, seek/reset, EOS/stop, speed/mode and generic
AUDIO behavior. They do not model cross-core scheduling or real audio hardware.
The changed sync, audio-render and video-port translation units also passed
real Xtensa syntax checks. Apps v11 full build and device playback remain
pending; the tests are not evidence of an audible or frame-rate improvement.

Three optional diagnostic events distinguish late-audio and late-wall drops
from clock reset/anchor reasons. Drop `elapsed_us` is measured lateness, not
execution time; no per-frame logging, allocation or new task is introduced.

Apps v11a corrects the tolerance input for extractor-based AVI playback:
`player_sync_local_mjpeg_decode_frame` now receives the actual track FPS from
the video input port. Its local frame interval is integer `1000 / source_fps`;
23 FPS therefore uses an 86 ms tolerance, while unknown/zero FPS retains the
60 ms minimum. It does not enable or modify the generic FPS pacing mechanism.
In v11, that mechanism was disabled for extractor input and its unset interval
caused this policy to use the 60 ms minimum even for 23 FPS files.

An additional optional clock-snapshot hook publishes the latest eligible
frame's video/audio/anchor PTS, anchor elapsed time, actual FPS, threshold,
gate flags, decision and reanchor reason. State is captured before the decision
and any reset, then the borrowed snapshot is delivered after releasing the sync
lock. The application aggregates it; the player still emits no per-frame log.
This instrumentation does not normalize PTS or change clock behavior.

The production-sync host test now also verifies track-23 FPS without generic
FPS setup, the zero-FPS fallback and pre-reset snapshot semantics. Both host
tests and real Xtensa syntax checks for sync/video-port code pass. Apps v11a
full build and start/seek device validation are pending at this source revision.

### Managed network input callback correction (v23)

Managed HTTPS MJPEG/PCM input uses a bounded forward-only seek callback, rather
than the pinned native NULL-seek fallback. Native FLV propagates its tail seek
return as a frame status; that fallback returns the positive skipped-byte count,
so a valid four-byte PrevTagSize skip can collide with native EOS=4. The managed
callback consumes forward bytes and returns zero on success. It compares the
absolute target with the IO consumer cursor, including native read-ahead;
backward misses and skips above 128KiB fail. It never invokes GMF byte seeking,
HTTP Range, reconnect, or a second stream request. Ordinary/local input retains
its existing seek and read paths.

Managed reads fill the native request across temporarily empty input buffers;
only a real source completion permits a positive short read. A single read or
entire forward skip has one cumulative 8s budget and checks cancellation in
at-most-50ms input waits. Existing startup 3s/128KiB limits remain in force.
Available-byte acquisitions use the sole-consumer IO buffer snapshot. Host
coverage executes these production helpers with deterministic GMF boundaries,
plus an explicitly modeled pinned native tail-return branch and a captured
public FLV prefix. Xtensa syntax checks passed; device playback remains pending.
